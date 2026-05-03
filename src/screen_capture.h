#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *data;
    size_t data_size;
    void *impl; // opaque implementation pointer
} ScreenCapture;

bool screen_capture_init(ScreenCapture *cap);
void screen_capture_cleanup(ScreenCapture *cap);
bool screen_capture_grab(ScreenCapture *cap);
void screen_capture_get_display_size(uint32_t *width, uint32_t *height);

#endif
