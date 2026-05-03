#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include "screen_capture.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    SCContentFilter *filter;
    SCStreamConfiguration *config;
    SCStream *stream;
    bool stream_started;
    pthread_mutex_t frame_mutex;
    uint8_t *latest_frame;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_stride;
    size_t frame_size;
    bool frame_ready;
} ScreenCaptureImpl;

@interface StreamDelegate : NSObject <SCStreamOutput>
@property (nonatomic) ScreenCaptureImpl *impl;
@end

@implementation StreamDelegate

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    void *baseAddr = CVPixelBufferGetBaseAddress(imageBuffer);
    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);

    if (baseAddr && width > 0 && height > 0) {
        size_t dataSize = bytesPerRow * height;

        pthread_mutex_lock(&_impl->frame_mutex);

        if (_impl->frame_size != dataSize || !_impl->latest_frame) {
            free(_impl->latest_frame);
            _impl->latest_frame = malloc(dataSize);
            _impl->frame_size = dataSize;
        }

        if (_impl->latest_frame) {
            memcpy(_impl->latest_frame, baseAddr, dataSize);
            _impl->frame_width = (uint32_t)width;
            _impl->frame_height = (uint32_t)height;
            _impl->frame_stride = (uint32_t)bytesPerRow;
            _impl->frame_ready = true;
        }

        pthread_mutex_unlock(&_impl->frame_mutex);
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

static StreamDelegate *g_delegate = nil;

void screen_capture_get_display_size(uint32_t *width, uint32_t *height) {
    CGDirectDisplayID display = CGMainDisplayID();
    *width = (uint32_t)CGDisplayPixelsWide(display);
    *height = (uint32_t)CGDisplayPixelsHigh(display);
}

bool screen_capture_init(ScreenCapture *cap) {
    screen_capture_get_display_size(&cap->width, &cap->height);
    cap->stride = cap->width * 4;
    cap->data_size = (size_t)cap->stride * cap->height;
    cap->data = malloc(cap->data_size);
    if (!cap->data) return false;
    memset(cap->data, 0, cap->data_size);

    ScreenCaptureImpl *impl = calloc(1, sizeof(ScreenCaptureImpl));
    if (!impl) { free(cap->data); return false; }
    pthread_mutex_init(&impl->frame_mutex, NULL);
    cap->impl = impl;

    __block bool success = false;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
        if (error || !content) {
            NSLog(@"[Capture] Failed to get shareable content: %@", error);
            dispatch_semaphore_signal(sem);
            return;
        }

        SCDisplay *mainDisplay = nil;
        for (SCDisplay *d in content.displays) {
            if (d.displayID == CGMainDisplayID()) {
                mainDisplay = d;
                break;
            }
        }

        if (!mainDisplay && content.displays.count > 0) {
            mainDisplay = content.displays[0];
        }

        if (!mainDisplay) {
            NSLog(@"[Capture] No display found");
            dispatch_semaphore_signal(sem);
            return;
        }

        impl->filter = [[SCContentFilter alloc] initWithDisplay:mainDisplay excludingWindows:@[]];

        impl->config = [[SCStreamConfiguration alloc] init];
        impl->config.width = cap->width;
        impl->config.height = cap->height;
        impl->config.pixelFormat = kCVPixelFormatType_32BGRA;
        impl->config.minimumFrameInterval = CMTimeMake(1, 30);
        impl->config.showsCursor = YES;
        impl->config.queueDepth = 3;

        NSError *streamError = nil;
        impl->stream = [[SCStream alloc] initWithFilter:impl->filter
                                          configuration:impl->config
                                               delegate:nil];

        g_delegate = [[StreamDelegate alloc] init];
        g_delegate.impl = impl;

        [impl->stream addStreamOutput:g_delegate
                                  type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0)
                                 error:&streamError];

        if (streamError) {
            NSLog(@"[Capture] Failed to add stream output: %@", streamError);
            dispatch_semaphore_signal(sem);
            return;
        }

        [impl->stream startCaptureWithCompletionHandler:^(NSError *startError) {
            if (startError) {
                NSLog(@"[Capture] Failed to start capture: %@", startError);
            } else {
                impl->stream_started = true;
                success = true;
                NSLog(@"[Capture] Screen capture started: %ux%u", cap->width, cap->height);
            }
            dispatch_semaphore_signal(sem);
        }];
    }];

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));

    if (!success) {
        NSLog(@"[Capture] ScreenCaptureKit initialization failed. Grant Screen Recording permission.");
    }

    return true;
}

void screen_capture_cleanup(ScreenCapture *cap) {
    ScreenCaptureImpl *impl = (ScreenCaptureImpl *)cap->impl;
    if (impl) {
        if (impl->stream_started) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [impl->stream stopCaptureWithCompletionHandler:^(NSError *error) {
                (void)error;
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        }
        pthread_mutex_destroy(&impl->frame_mutex);
        free(impl->latest_frame);
        free(impl);
        cap->impl = NULL;
    }
    g_delegate = nil;
    free(cap->data);
    cap->data = NULL;
}

bool screen_capture_grab(ScreenCapture *cap) {
    ScreenCaptureImpl *impl = (ScreenCaptureImpl *)cap->impl;
    if (!impl) return false;

    if (impl->stream_started) {
        pthread_mutex_lock(&impl->frame_mutex);
        if (impl->frame_ready && impl->latest_frame) {
            if (impl->frame_width != cap->width || impl->frame_height != cap->height) {
                cap->width = impl->frame_width;
                cap->height = impl->frame_height;
                cap->stride = impl->frame_stride;
                cap->data_size = (size_t)cap->stride * cap->height;
                free(cap->data);
                cap->data = malloc(cap->data_size);
            }
            if (cap->stride == impl->frame_stride) {
                memcpy(cap->data, impl->latest_frame, cap->data_size);
            } else {
                uint32_t copy_stride = (cap->stride < impl->frame_stride) ? cap->stride : impl->frame_stride;
                for (uint32_t y = 0; y < cap->height; y++) {
                    memcpy(cap->data + y * cap->stride,
                           impl->latest_frame + y * impl->frame_stride,
                           copy_stride);
                }
            }
            pthread_mutex_unlock(&impl->frame_mutex);
            return true;
        }
        pthread_mutex_unlock(&impl->frame_mutex);
        return false;
    }

    return false;
}
