#ifndef RFB_PROTOCOL_H
#define RFB_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <zlib.h>

#define RFB_VERSION_STRING "RFB 003.008\n"
#define RFB_VERSION_LEN 12

#define RFB_SEC_NONE 1
#define RFB_SEC_VNC_AUTH 2
#define RFB_SEC_RESULT_OK 0
#define RFB_SEC_RESULT_FAILED 1

// Client-to-server message types
#define RFB_MSG_SET_PIXEL_FORMAT 0
#define RFB_MSG_SET_ENCODINGS 2
#define RFB_MSG_FB_UPDATE_REQUEST 3
#define RFB_MSG_KEY_EVENT 4
#define RFB_MSG_POINTER_EVENT 5
#define RFB_MSG_CLIENT_CUT_TEXT 6

// Server-to-client message types
#define RFB_MSG_FB_UPDATE 0
#define RFB_MSG_SET_COLOR_MAP 1
#define RFB_MSG_BELL 2
#define RFB_MSG_SERVER_CUT_TEXT 3

// Encoding types
#define RFB_ENCODING_RAW 0
#define RFB_ENCODING_COPYRECT 1
#define RFB_ENCODING_ZLIB 6
#define RFB_ENCODING_ZRLE 16
#define RFB_ENCODING_CURSOR 0xFFFFFF11
#define RFB_ENCODING_DESKTOP_SIZE 0xFFFFFF21

#pragma pack(push, 1)

typedef struct {
    uint8_t bpp;
    uint8_t depth;
    uint8_t big_endian;
    uint8_t true_color;
    uint16_t red_max;
    uint16_t green_max;
    uint16_t blue_max;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
    uint8_t padding[3];
} RFBPixelFormat;

typedef struct {
    uint16_t width;
    uint16_t height;
    RFBPixelFormat pixel_format;
    uint32_t name_length;
} RFBServerInit;

typedef struct {
    uint8_t msg_type;
    uint8_t padding[3];
    RFBPixelFormat pixel_format;
} RFBSetPixelFormat;

typedef struct {
    uint8_t msg_type;
    uint8_t padding;
    uint16_t num_encodings;
} RFBSetEncodings;

typedef struct {
    uint8_t msg_type;
    uint8_t incremental;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} RFBFBUpdateRequest;

typedef struct {
    uint8_t msg_type;
    uint8_t down_flag;
    uint8_t padding[2];
    uint32_t key;
} RFBKeyEvent;

typedef struct {
    uint8_t msg_type;
    uint8_t button_mask;
    uint16_t x;
    uint16_t y;
} RFBPointerEvent;

typedef struct {
    uint8_t msg_type;
    uint8_t padding[3];
    uint32_t length;
} RFBClientCutText;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    int32_t encoding;
} RFBFramebufferRect;

#pragma pack(pop)

typedef struct {
    int sock_fd;
    RFBPixelFormat client_format;
    bool format_set;
    bool format_is_bgra;
    int32_t *supported_encodings;
    int num_encodings;
    bool zlib_initialized;
    z_stream zlib_stream;
    bool zrle_initialized;
    z_stream zrle_stream;
    uint8_t *zrle_buf;
    size_t zrle_buf_cap;
    bool update_pending;
    bool incremental;
    uint16_t req_x, req_y, req_w, req_h;
    uint8_t *convert_buf;
    size_t convert_buf_cap;
    uint8_t *compress_buf;
    size_t compress_buf_cap;
    uint32_t last_key;
    uint8_t last_key_down;
    uint8_t last_pointer_buttons;
    uint16_t last_pointer_x;
    uint16_t last_pointer_y;
} RFBClientState;

bool rfb_send_all(int fd, const void *buf, size_t len);
bool rfb_recv_all(int fd, void *buf, size_t len);
bool rfb_perform_handshake(int client_fd, const char *password);
bool rfb_send_server_init(int client_fd, uint16_t width, uint16_t height, const char *name);
int rfb_read_client_message(RFBClientState *client);
bool rfb_send_framebuffer_update_raw(RFBClientState *client, uint16_t x, uint16_t y,
                                      uint16_t width, uint16_t height,
                                      const uint8_t *pixels, int stride,
                                      int src_bpp);
bool rfb_send_framebuffer_update_zlib(RFBClientState *client, uint16_t x, uint16_t y,
                                       uint16_t width, uint16_t height,
                                       const uint8_t *pixels, int stride,
                                       int src_bpp);
bool rfb_send_fb_update_header(RFBClientState *client, uint16_t num_rects);
bool rfb_send_fb_update_rect_raw(RFBClientState *client, uint16_t x, uint16_t y,
                                  uint16_t width, uint16_t height,
                                  const uint8_t *pixels, int stride, int src_bpp);
bool rfb_send_fb_update_rect_zlib(RFBClientState *client, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint8_t *pixels, int stride, int src_bpp);
bool rfb_send_fb_update_rect_zrle(RFBClientState *client, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint8_t *pixels, int stride, int src_bpp);
bool rfb_client_supports_encoding(RFBClientState *client, int32_t encoding);
void rfb_client_state_init(RFBClientState *client, int sock_fd);
void rfb_client_state_cleanup(RFBClientState *client);

#endif
