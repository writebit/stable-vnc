#include "rfb_protocol.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <CommonCrypto/CommonCryptor.h>
#include <Security/SecRandom.h>

#define TAG "RFB"

bool rfb_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = send(fd, p, remaining, 0);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            LOG_ERROR(TAG, "send failed: %s", strerror(errno));
            return false;
        }
        p += sent;
        remaining -= sent;
    }
    return true;
}

bool rfb_recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t recvd = recv(fd, p, remaining, 0);
        if (recvd <= 0) {
            if (recvd < 0 && errno == EINTR) continue;
            if (recvd == 0)
                LOG_INFO(TAG, "Connection closed by peer");
            else
                LOG_ERROR(TAG, "recv failed: %s", strerror(errno));
            return false;
        }
        p += recvd;
        remaining -= recvd;
    }
    return true;
}

static uint8_t vnc_reverse_bits(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

static bool rfb_parse_version(const char *version, int *major, int *minor) {
    return sscanf(version, "RFB %03d.%03d\n", major, minor) == 2;
}

static bool vnc_auth_verify(int client_fd, const char *password) {
    uint8_t challenge[16];
    if (SecRandomCopyBytes(kSecRandomDefault, 16, challenge) != errSecSuccess) {
        LOG_ERROR(TAG, "Failed to generate random challenge");
        return false;
    }

    if (!rfb_send_all(client_fd, challenge, 16))
        return false;

    uint8_t response[16];
    if (!rfb_recv_all(client_fd, response, 16))
        return false;

    // Build DES key from password (VNC bit-reversal quirk)
    uint8_t key[8] = {0};
    size_t pw_len = strlen(password);
    if (pw_len > 8) pw_len = 8;
    for (size_t i = 0; i < pw_len; i++)
        key[i] = vnc_reverse_bits((uint8_t)password[i]);

    uint8_t expected[16];
    size_t moved;
    CCCrypt(kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode,
            key, kCCKeySizeDES, NULL,
            challenge, 8, expected, 8, &moved);
    CCCrypt(kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode,
            key, kCCKeySizeDES, NULL,
            challenge + 8, 8, expected + 8, 8, &moved);

    uint32_t result;
    if (memcmp(response, expected, 16) == 0) {
        result = htonl(RFB_SEC_RESULT_OK);
        rfb_send_all(client_fd, &result, 4);
        LOG_INFO(TAG, "VNC Authentication succeeded");
        return true;
    }

    result = htonl(RFB_SEC_RESULT_FAILED);
    rfb_send_all(client_fd, &result, 4);
    const char *msg = "Authentication failed";
    uint32_t msg_len = htonl((uint32_t)strlen(msg));
    rfb_send_all(client_fd, &msg_len, 4);
    rfb_send_all(client_fd, msg, strlen(msg));
    LOG_WARN(TAG, "VNC Authentication failed");
    return false;
}

bool rfb_perform_handshake(int client_fd, const char *password) {
    if (!rfb_send_all(client_fd, RFB_VERSION_STRING, RFB_VERSION_LEN))
        return false;

    char client_version[RFB_VERSION_LEN + 1];
    memset(client_version, 0, sizeof(client_version));
    if (!rfb_recv_all(client_fd, client_version, RFB_VERSION_LEN))
        return false;

    LOG_INFO(TAG, "Client version: %.12s", client_version);

    int major = 0;
    int minor = 0;
    if (!rfb_parse_version(client_version, &major, &minor) || major != 3) {
        LOG_ERROR(TAG, "Unsupported client version: %.12s", client_version);
        return false;
    }

    bool use_auth = (password && password[0] != '\0');
    bool legacy_33 = (minor == 3);

    if (legacy_33) {
        uint32_t sec_type = htonl(use_auth ? RFB_SEC_VNC_AUTH : RFB_SEC_NONE);
        if (!rfb_send_all(client_fd, &sec_type, sizeof(sec_type)))
            return false;

        if (use_auth)
            return vnc_auth_verify(client_fd, password);

        LOG_INFO(TAG, "Handshake completed (RFB 3.3 SecurityNone)");
        return true;
    }

    if (use_auth) {
        uint8_t sec_types[2] = { 1, RFB_SEC_VNC_AUTH };
        if (!rfb_send_all(client_fd, sec_types, 2))
            return false;
    } else {
        uint8_t sec_types[2] = { 1, RFB_SEC_NONE };
        if (!rfb_send_all(client_fd, sec_types, 2))
            return false;
    }

    uint8_t chosen_sec;
    if (!rfb_recv_all(client_fd, &chosen_sec, 1))
        return false;

    if (use_auth && chosen_sec == RFB_SEC_VNC_AUTH) {
        return vnc_auth_verify(client_fd, password);
    } else if (!use_auth && chosen_sec == RFB_SEC_NONE) {
        uint32_t result = htonl(RFB_SEC_RESULT_OK);
        if (!rfb_send_all(client_fd, &result, 4))
            return false;
        LOG_INFO(TAG, "Handshake completed (SecurityNone)");
        return true;
    }

    LOG_ERROR(TAG, "Client chose unsupported security type %d", chosen_sec);
    return false;
}

bool rfb_send_server_init(int client_fd, uint16_t width, uint16_t height, const char *name) {
    // Receive ClientInit (shared flag)
    uint8_t shared_flag;
    if (!rfb_recv_all(client_fd, &shared_flag, 1))
        return false;

    LOG_INFO(TAG, "ClientInit: shared=%d", shared_flag);

    uint32_t name_len = (uint32_t)strlen(name);

    RFBServerInit init;
    init.width = htons(width);
    init.height = htons(height);

    // Default pixel format: 32bpp BGRA (macOS native)
    init.pixel_format.bpp = 32;
    init.pixel_format.depth = 24;
    init.pixel_format.big_endian = 0;
    init.pixel_format.true_color = 1;
    init.pixel_format.red_max = htons(255);
    init.pixel_format.green_max = htons(255);
    init.pixel_format.blue_max = htons(255);
    init.pixel_format.red_shift = 16;
    init.pixel_format.green_shift = 8;
    init.pixel_format.blue_shift = 0;
    memset(init.pixel_format.padding, 0, 3);
    init.name_length = htonl(name_len);

    if (!rfb_send_all(client_fd, &init, sizeof(init)))
        return false;
    if (!rfb_send_all(client_fd, name, name_len))
        return false;

    LOG_INFO(TAG, "ServerInit sent: %dx%d, name='%s'", width, height, name);
    return true;
}

// Skip a fixed number of bytes from the client (for unknown/unsupported messages)
static bool rfb_skip_bytes(int fd, size_t count) {
    uint8_t buf[256];
    while (count > 0) {
        size_t chunk = count > sizeof(buf) ? sizeof(buf) : count;
        if (!rfb_recv_all(fd, buf, chunk))
            return false;
        count -= chunk;
    }
    return true;
}

int rfb_read_client_message(RFBClientState *client) {
    uint8_t msg_type;
    if (!rfb_recv_all(client->sock_fd, &msg_type, 1))
        return -1;

    switch (msg_type) {
        case RFB_MSG_SET_PIXEL_FORMAT: { // 0
            uint8_t padding[3];
            rfb_recv_all(client->sock_fd, padding, 3);
            RFBPixelFormat pf;
            if (!rfb_recv_all(client->sock_fd, &pf, sizeof(pf)))
                return -1;
            client->client_format = pf;
            client->client_format.red_max = ntohs(pf.red_max);
            client->client_format.green_max = ntohs(pf.green_max);
            client->client_format.blue_max = ntohs(pf.blue_max);
            client->format_set = true;
            client->format_is_bgra = (pf.bpp == 32 && pf.true_color &&
                                       !pf.big_endian &&
                                       client->client_format.red_max == 255 &&
                                       client->client_format.green_max == 255 &&
                                       client->client_format.blue_max == 255 &&
                                       pf.red_shift == 16 && pf.green_shift == 8 &&
                                       pf.blue_shift == 0);
            LOG_INFO(TAG, "SetPixelFormat: bpp=%d depth=%d bigendian=%d truecolor=%d (bgra_match=%d)",
                     pf.bpp, pf.depth, pf.big_endian, pf.true_color, client->format_is_bgra);
            LOG_INFO(TAG, "  red: max=%d shift=%d, green: max=%d shift=%d, blue: max=%d shift=%d",
                     client->client_format.red_max, pf.red_shift,
                     client->client_format.green_max, pf.green_shift,
                     client->client_format.blue_max, pf.blue_shift);
            break;
        }
        case RFB_MSG_SET_ENCODINGS: { // 2
            uint8_t padding;
            rfb_recv_all(client->sock_fd, &padding, 1);
            uint16_t num;
            if (!rfb_recv_all(client->sock_fd, &num, 2))
                return -1;
            num = ntohs(num);
            free(client->supported_encodings);
            client->supported_encodings = malloc(num * sizeof(int32_t));
            client->num_encodings = num;
            for (int i = 0; i < num; i++) {
                int32_t enc;
                if (!rfb_recv_all(client->sock_fd, &enc, 4))
                    return -1;
                client->supported_encodings[i] = (int32_t)ntohl(enc);
            }
            LOG_INFO(TAG, "SetEncodings: %d encodings", num);
            for (int i = 0; i < num; i++) {
                LOG_DEBUG(TAG, "  encoding[%d] = %d (0x%08X)", i,
                          client->supported_encodings[i], client->supported_encodings[i]);
            }
            break;
        }
        case RFB_MSG_FB_UPDATE_REQUEST: { // 3
            // After msg_type: incremental(1) + x(2) + y(2) + width(2) + height(2) = 9 bytes
            RFBFBUpdateRequest req;
            req.msg_type = msg_type;
            if (!rfb_recv_all(client->sock_fd, &req.incremental, 9))
                return -1;
            client->update_pending = true;
            client->incremental = req.incremental;
            client->req_x = ntohs(req.x);
            client->req_y = ntohs(req.y);
            client->req_w = ntohs(req.width);
            client->req_h = ntohs(req.height);
            LOG_DEBUG(TAG, "FBUpdateRequest: inc=%d x=%d y=%d w=%d h=%d",
                      client->incremental, client->req_x, client->req_y,
                      client->req_w, client->req_h);
            break;
        }
        case RFB_MSG_KEY_EVENT: { // 4
            uint8_t down_flag;
            uint8_t pad[2];
            uint32_t key;
            if (!rfb_recv_all(client->sock_fd, &down_flag, 1)) return -1;
            rfb_recv_all(client->sock_fd, pad, 2);
            if (!rfb_recv_all(client->sock_fd, &key, 4)) return -1;
            client->last_key_down = down_flag;
            client->last_key = ntohl(key);
            LOG_DEBUG(TAG, "KeyEvent: key=0x%04X down=%d", client->last_key, down_flag);
            return RFB_MSG_KEY_EVENT;
        }
        case RFB_MSG_POINTER_EVENT: { // 5
            uint8_t buttons;
            uint16_t px, py;
            if (!rfb_recv_all(client->sock_fd, &buttons, 1)) return -1;
            if (!rfb_recv_all(client->sock_fd, &px, 2)) return -1;
            if (!rfb_recv_all(client->sock_fd, &py, 2)) return -1;
            client->last_pointer_buttons = buttons;
            client->last_pointer_x = ntohs(px);
            client->last_pointer_y = ntohs(py);
            LOG_DEBUG(TAG, "PointerEvent: x=%d y=%d buttons=0x%02X",
                      client->last_pointer_x, client->last_pointer_y, buttons);
            return RFB_MSG_POINTER_EVENT;
        }
        case RFB_MSG_CLIENT_CUT_TEXT: { // 6
            uint8_t padding[3];
            rfb_recv_all(client->sock_fd, padding, 3);
            uint32_t length;
            if (!rfb_recv_all(client->sock_fd, &length, 4))
                return -1;
            length = ntohl(length);
            LOG_INFO(TAG, "ClientCutText: %u bytes", length);
            if (length > 0 && length < 10 * 1024 * 1024) {
                char *text = malloc(length + 1);
                if (text) {
                    rfb_recv_all(client->sock_fd, text, length);
                    text[length] = '\0';
                    free(text);
                }
            } else if (length > 0) {
                rfb_skip_bytes(client->sock_fd, length);
            }
            break;
        }

        // === RFB Extension Messages ===

        case 7: { // EnableContinuousUpdates
            // Format: 1 byte enable + 2 bytes x + 2 bytes y + 2 bytes w + 2 bytes h = 9 bytes
            uint8_t buf[9];
            if (!rfb_recv_all(client->sock_fd, buf, 9))
                return -1;
            LOG_INFO(TAG, "EnableContinuousUpdates: enable=%d (ignored, not supported)", buf[0]);
            break;
        }

        case 8: { // ClientFence
            // Format: 3 bytes padding + 4 bytes flags + 1 byte length + variable payload
            uint8_t padding[3];
            if (!rfb_recv_all(client->sock_fd, padding, 3)) return -1;
            uint32_t flags;
            if (!rfb_recv_all(client->sock_fd, &flags, 4)) return -1;
            flags = ntohl(flags);
            uint8_t payload_len;
            if (!rfb_recv_all(client->sock_fd, &payload_len, 1)) return -1;
            uint8_t payload[64];
            if (payload_len > 0) {
                if (payload_len > sizeof(payload)) {
                    rfb_skip_bytes(client->sock_fd, payload_len);
                } else {
                    if (!rfb_recv_all(client->sock_fd, payload, payload_len)) return -1;
                }
            }
            LOG_INFO(TAG, "ClientFence: flags=0x%08X len=%d", flags, payload_len);

            // If request flag (bit 31) is set, respond with ServerFence
            if (flags & (1u << 31)) {
                uint8_t resp[1 + 3 + 4 + 1];
                resp[0] = 248; // ServerFence message type
                resp[1] = resp[2] = resp[3] = 0; // padding
                uint32_t resp_flags = htonl(flags & ~(1u << 31));
                memcpy(resp + 4, &resp_flags, 4);
                resp[8] = payload_len;
                if (rfb_send_all(client->sock_fd, resp, 9)) {
                    if (payload_len > 0 && payload_len <= sizeof(payload)) {
                        rfb_send_all(client->sock_fd, payload, payload_len);
                    }
                }
                LOG_DEBUG(TAG, "  Responded with ServerFence");
            }
            break;
        }

        case 15: { // SetDesktopSize
            // Format: 1 byte padding + 2 bytes width + 2 bytes height +
            //         1 byte num_screens + 1 byte padding, then per screen 16 bytes
            uint8_t buf[5];
            if (!rfb_recv_all(client->sock_fd, buf, 5)) return -1;
            uint8_t num_screens = buf[4];
            uint8_t padding2;
            if (!rfb_recv_all(client->sock_fd, &padding2, 1)) return -1;
            // Skip screen data: each screen is 16 bytes (id=4, x=2, y=2, w=2, h=2, flags=4)
            if (num_screens > 0) {
                rfb_skip_bytes(client->sock_fd, (size_t)num_screens * 16);
            }
            LOG_INFO(TAG, "SetDesktopSize: num_screens=%d (ignored)", num_screens);
            break;
        }

        case 20: { // QEMU extended key event
            // Format: 2 bytes sub-message-type + 2 bytes down-flag + 4 bytes keysym + 4 bytes keycode
            // Total: 12 bytes after the message type byte, but first byte is already read
            // Actually: submessage(2) + downflag(2) + keysym(4) + keycode(4) = 12 bytes
            // But byte 0 was the type. The spec says 1+1(submsg)+2(downflag)+4(keysym)+4(keycode)
            // Let's just skip 11 bytes
            uint8_t buf[11];
            if (!rfb_recv_all(client->sock_fd, buf, 11)) return -1;
            LOG_DEBUG(TAG, "QEMU extended key event (skipped)");
            break;
        }

        case 150: { // TightVNC file transfer or similar
            // Variable length - try to read a 3-byte header to get length
            uint8_t buf[3];
            if (!rfb_recv_all(client->sock_fd, buf, 3)) return -1;
            uint16_t length = ((uint16_t)buf[1] << 8) | buf[2];
            if (length > 0) {
                rfb_skip_bytes(client->sock_fd, length);
            }
            LOG_INFO(TAG, "TightVNC extension msg 150, subtype=%d, len=%d (skipped)", buf[0], length);
            break;
        }

        case 252: { // VMware extension
            // Skip 3 bytes
            rfb_skip_bytes(client->sock_fd, 3);
            LOG_DEBUG(TAG, "VMware extension msg 252 (skipped)");
            break;
        }

        case 254: { // Colin Dean xvp
            // 1 byte padding + 1 byte version + 1 byte code = 3 bytes
            uint8_t buf[3];
            if (!rfb_recv_all(client->sock_fd, buf, 3)) return -1;
            LOG_INFO(TAG, "xvp message: version=%d code=%d (skipped)", buf[1], buf[2]);
            break;
        }

        case 255: { // QEMU message
            // 1 byte submessage-type, then variable
            uint8_t subtype;
            if (!rfb_recv_all(client->sock_fd, &subtype, 1)) return -1;
            if (subtype == 0) {
                // Extended key event: 2 bytes downflag + 4 bytes keysym + 4 bytes keycode = 10
                uint8_t buf[10];
                if (!rfb_recv_all(client->sock_fd, buf, 10)) return -1;
            } else if (subtype == 1) {
                // Audio: variable, skip 2 bytes (submsg-specific)
                uint8_t buf[2];
                if (!rfb_recv_all(client->sock_fd, buf, 2)) return -1;
            }
            LOG_INFO(TAG, "QEMU msg subtype=%d (skipped)", subtype);
            break;
        }

        default:
            LOG_WARN(TAG, "Unknown client message type: %d (0x%02X) - disconnecting", msg_type, msg_type);
            return -1;
    }
    return msg_type;
}

static void convert_pixel(const uint8_t *src, uint8_t *dst, int src_bpp,
                           const RFBPixelFormat *fmt) {
    uint8_t r, g, b;

    if (src_bpp == 4) {
        // macOS BGRA format
        b = src[0]; g = src[1]; r = src[2];
    } else {
        r = src[0]; g = src[1]; b = src[2];
    }

    if (fmt->bpp == 32) {
        uint32_t pixel = 0;
        pixel |= ((uint32_t)(r * fmt->red_max / 255)) << fmt->red_shift;
        pixel |= ((uint32_t)(g * fmt->green_max / 255)) << fmt->green_shift;
        pixel |= ((uint32_t)(b * fmt->blue_max / 255)) << fmt->blue_shift;
        if (fmt->big_endian) {
            dst[0] = (pixel >> 24) & 0xFF;
            dst[1] = (pixel >> 16) & 0xFF;
            dst[2] = (pixel >> 8) & 0xFF;
            dst[3] = pixel & 0xFF;
        } else {
            memcpy(dst, &pixel, 4);
        }
    } else if (fmt->bpp == 16) {
        uint16_t pixel = 0;
        pixel |= ((uint16_t)(r * fmt->red_max / 255)) << fmt->red_shift;
        pixel |= ((uint16_t)(g * fmt->green_max / 255)) << fmt->green_shift;
        pixel |= ((uint16_t)(b * fmt->blue_max / 255)) << fmt->blue_shift;
        if (fmt->big_endian) {
            dst[0] = (pixel >> 8) & 0xFF;
            dst[1] = pixel & 0xFF;
        } else {
            memcpy(dst, &pixel, 2);
        }
    } else {
        uint8_t pixel = 0;
        pixel |= ((r * fmt->red_max / 255)) << fmt->red_shift;
        pixel |= ((g * fmt->green_max / 255)) << fmt->green_shift;
        pixel |= ((b * fmt->blue_max / 255)) << fmt->blue_shift;
        dst[0] = pixel;
    }
}

static uint8_t *convert_framebuffer(const uint8_t *pixels, uint16_t width, uint16_t height,
                                     int stride, int src_bpp, const RFBPixelFormat *fmt,
                                     size_t *out_size) {
    int dst_bpp = fmt->bpp / 8;
    size_t size = (size_t)width * height * dst_bpp;
    uint8_t *buf = malloc(size);
    if (!buf) return NULL;

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = pixels + y * stride;
        uint8_t *dst_row = buf + y * width * dst_bpp;
        for (int x = 0; x < width; x++) {
            convert_pixel(src_row + x * src_bpp, dst_row + x * dst_bpp, src_bpp, fmt);
        }
    }
    *out_size = size;
    return buf;
}

bool rfb_send_framebuffer_update_raw(RFBClientState *client, uint16_t x, uint16_t y,
                                      uint16_t width, uint16_t height,
                                      const uint8_t *pixels, int stride,
                                      int src_bpp) {
    const RFBPixelFormat *fmt = client->format_set ? &client->client_format : NULL;

    size_t pixel_data_size;
    uint8_t *converted = NULL;

    if (fmt) {
        converted = convert_framebuffer(pixels, width, height, stride, src_bpp, fmt, &pixel_data_size);
        if (!converted) return false;
    } else {
        int dst_bpp = 4;
        pixel_data_size = (size_t)width * height * dst_bpp;
        converted = malloc(pixel_data_size);
        if (!converted) return false;
        for (int row = 0; row < height; row++) {
            const uint8_t *src_row = pixels + row * stride;
            uint8_t *dst_row = converted + row * width * dst_bpp;
            for (int col = 0; col < width; col++) {
                // BGRA -> default format (BGRA with red_shift=16)
                dst_row[col * 4 + 0] = src_row[col * src_bpp + 2]; // R
                dst_row[col * 4 + 1] = src_row[col * src_bpp + 1]; // G
                dst_row[col * 4 + 2] = src_row[col * src_bpp + 0]; // B
                dst_row[col * 4 + 3] = 0;
            }
        }
    }

    // FramebufferUpdate header
    uint8_t header[4];
    header[0] = RFB_MSG_FB_UPDATE;
    header[1] = 0;
    uint16_t num_rects = htons(1);
    memcpy(header + 2, &num_rects, 2);

    RFBFramebufferRect rect;
    rect.x = htons(x);
    rect.y = htons(y);
    rect.width = htons(width);
    rect.height = htons(height);
    rect.encoding = htonl(RFB_ENCODING_RAW);

    LOG_DEBUG(TAG, "Sending FB update (raw): x=%d y=%d w=%d h=%d size=%zu",
              x, y, width, height, pixel_data_size);

    bool ok = rfb_send_all(client->sock_fd, header, 4) &&
              rfb_send_all(client->sock_fd, &rect, sizeof(rect)) &&
              rfb_send_all(client->sock_fd, converted, pixel_data_size);

    free(converted);
    return ok;
}

bool rfb_send_framebuffer_update_zlib(RFBClientState *client, uint16_t x, uint16_t y,
                                       uint16_t width, uint16_t height,
                                       const uint8_t *pixels, int stride,
                                       int src_bpp) {
    const RFBPixelFormat *fmt = client->format_set ? &client->client_format : NULL;
    size_t raw_size;
    uint8_t *raw_data;

    if (fmt) {
        raw_data = convert_framebuffer(pixels, width, height, stride, src_bpp, fmt, &raw_size);
    } else {
        int dst_bpp = 4;
        raw_size = (size_t)width * height * dst_bpp;
        raw_data = malloc(raw_size);
        for (int row = 0; row < height; row++) {
            const uint8_t *src_row = pixels + row * stride;
            uint8_t *dst_row = raw_data + row * width * dst_bpp;
            for (int col = 0; col < width; col++) {
                dst_row[col * 4 + 0] = src_row[col * src_bpp + 2];
                dst_row[col * 4 + 1] = src_row[col * src_bpp + 1];
                dst_row[col * 4 + 2] = src_row[col * src_bpp + 0];
                dst_row[col * 4 + 3] = 0;
            }
        }
    }
    if (!raw_data) return false;

    if (!client->zlib_initialized) {
        memset(&client->zlib_stream, 0, sizeof(client->zlib_stream));
        if (deflateInit(&client->zlib_stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
            free(raw_data);
            return false;
        }
        client->zlib_initialized = true;
    }

    size_t max_compressed = deflateBound(&client->zlib_stream, raw_size);
    uint8_t *compressed = malloc(max_compressed);
    if (!compressed) { free(raw_data); return false; }

    client->zlib_stream.next_in = raw_data;
    client->zlib_stream.avail_in = (uInt)raw_size;
    client->zlib_stream.next_out = compressed;
    client->zlib_stream.avail_out = (uInt)max_compressed;

    if (deflate(&client->zlib_stream, Z_SYNC_FLUSH) != Z_OK) {
        free(raw_data);
        free(compressed);
        return false;
    }

    size_t compressed_size = max_compressed - client->zlib_stream.avail_out;

    uint8_t header[4];
    header[0] = RFB_MSG_FB_UPDATE;
    header[1] = 0;
    uint16_t num_rects = htons(1);
    memcpy(header + 2, &num_rects, 2);

    RFBFramebufferRect rect;
    rect.x = htons(x);
    rect.y = htons(y);
    rect.width = htons(width);
    rect.height = htons(height);
    rect.encoding = htonl(RFB_ENCODING_ZLIB);

    uint32_t zlib_len = htonl((uint32_t)compressed_size);

    LOG_DEBUG(TAG, "Sending FB update (zlib): x=%d y=%d w=%d h=%d raw=%zu compressed=%zu",
              x, y, width, height, raw_size, compressed_size);

    bool ok = rfb_send_all(client->sock_fd, header, 4) &&
              rfb_send_all(client->sock_fd, &rect, sizeof(rect)) &&
              rfb_send_all(client->sock_fd, &zlib_len, 4) &&
              rfb_send_all(client->sock_fd, compressed, compressed_size);

    free(raw_data);
    free(compressed);
    return ok;
}

bool rfb_send_fb_update_header(RFBClientState *client, uint16_t num_rects) {
    uint8_t header[4];
    header[0] = RFB_MSG_FB_UPDATE;
    header[1] = 0;
    uint16_t n = htons(num_rects);
    memcpy(header + 2, &n, 2);
    return rfb_send_all(client->sock_fd, header, 4);
}

static uint8_t *ensure_convert_buf(RFBClientState *client, size_t needed) {
    if (needed > client->convert_buf_cap) {
        free(client->convert_buf);
        client->convert_buf = malloc(needed);
        client->convert_buf_cap = client->convert_buf ? needed : 0;
    }
    return client->convert_buf;
}

static uint8_t *convert_rect_pixels(RFBClientState *client, const uint8_t *pixels,
                                     uint16_t width, uint16_t height, int stride,
                                     int src_bpp, size_t *out_size) {
    int dst_bpp = client->format_set ? (client->client_format.bpp / 8) : 4;
    size_t size = (size_t)width * height * dst_bpp;
    uint8_t *buf = ensure_convert_buf(client, size);
    if (!buf) return NULL;

    if (client->format_set && client->format_is_bgra && src_bpp == 4) {
        for (int row = 0; row < height; row++)
            memcpy(buf + row * width * 4, pixels + row * stride, (size_t)width * 4);
        *out_size = size;
        return buf;
    }

    if (client->format_set) {
        const RFBPixelFormat *fmt = &client->client_format;
        for (int y = 0; y < height; y++) {
            const uint8_t *src_row = pixels + y * stride;
            uint8_t *dst_row = buf + y * width * dst_bpp;
            for (int x = 0; x < width; x++)
                convert_pixel(src_row + x * src_bpp, dst_row + x * dst_bpp, src_bpp, fmt);
        }
    } else {
        for (int row = 0; row < height; row++) {
            const uint8_t *src_row = pixels + row * stride;
            uint8_t *dst_row = buf + row * width * 4;
            for (int col = 0; col < width; col++) {
                dst_row[col * 4 + 0] = src_row[col * src_bpp + 2];
                dst_row[col * 4 + 1] = src_row[col * src_bpp + 1];
                dst_row[col * 4 + 2] = src_row[col * src_bpp + 0];
                dst_row[col * 4 + 3] = 0;
            }
        }
    }
    *out_size = size;
    return buf;
}

bool rfb_send_fb_update_rect_raw(RFBClientState *client, uint16_t x, uint16_t y,
                                  uint16_t width, uint16_t height,
                                  const uint8_t *pixels, int stride, int src_bpp) {
    size_t pixel_data_size;
    uint8_t *converted = convert_rect_pixels(client, pixels, width, height, stride, src_bpp, &pixel_data_size);
    if (!converted) return false;

    RFBFramebufferRect rect;
    rect.x = htons(x);
    rect.y = htons(y);
    rect.width = htons(width);
    rect.height = htons(height);
    rect.encoding = htonl(RFB_ENCODING_RAW);

    return rfb_send_all(client->sock_fd, &rect, sizeof(rect)) &&
           rfb_send_all(client->sock_fd, converted, pixel_data_size);
}

bool rfb_send_fb_update_rect_zlib(RFBClientState *client, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint8_t *pixels, int stride, int src_bpp) {
    size_t raw_size;
    uint8_t *raw_data = convert_rect_pixels(client, pixels, width, height, stride, src_bpp, &raw_size);
    if (!raw_data) return false;

    if (!client->zlib_initialized) {
        memset(&client->zlib_stream, 0, sizeof(client->zlib_stream));
        if (deflateInit(&client->zlib_stream, Z_BEST_SPEED) != Z_OK)
            return false;
        client->zlib_initialized = true;
    }

    size_t max_compressed = deflateBound(&client->zlib_stream, raw_size);
    if (max_compressed > client->compress_buf_cap) {
        free(client->compress_buf);
        client->compress_buf = malloc(max_compressed);
        client->compress_buf_cap = client->compress_buf ? max_compressed : 0;
    }
    if (!client->compress_buf) return false;

    client->zlib_stream.next_in = raw_data;
    client->zlib_stream.avail_in = (uInt)raw_size;
    client->zlib_stream.next_out = client->compress_buf;
    client->zlib_stream.avail_out = (uInt)client->compress_buf_cap;

    if (deflate(&client->zlib_stream, Z_SYNC_FLUSH) != Z_OK)
        return false;

    size_t compressed_size = client->compress_buf_cap - client->zlib_stream.avail_out;

    RFBFramebufferRect rect;
    rect.x = htons(x);
    rect.y = htons(y);
    rect.width = htons(width);
    rect.height = htons(height);
    rect.encoding = htonl(RFB_ENCODING_ZLIB);

    uint32_t zlib_len = htonl((uint32_t)compressed_size);

    return rfb_send_all(client->sock_fd, &rect, sizeof(rect)) &&
           rfb_send_all(client->sock_fd, &zlib_len, 4) &&
           rfb_send_all(client->sock_fd, client->compress_buf, compressed_size);
}

// --- ZRLE encoding ---

#define ZRLE_TILE 64

static inline uint32_t zrle_read_cpx(const uint8_t *p, int n) {
    uint32_t v = 0;
    memcpy(&v, p, n);
    return v;
}

static inline void zrle_write_cpx(uint8_t *d, uint32_t v, int n) {
    memcpy(d, &v, n);
}

static int zrle_write_rle_len(uint8_t *out, int run) {
    int n = 0, rl = run - 1;
    while (rl >= 255) { out[n++] = 255; rl -= 255; }
    out[n++] = (uint8_t)rl;
    return n;
}

static size_t zrle_encode_tile(const uint8_t *px, int tw, int th,
                                int row_stride, int pbytes,
                                int cpx_n, int cpx_off,
                                uint8_t *out) {
    int np = tw * th;
    uint32_t cpx[ZRLE_TILE * ZRLE_TILE];

    for (int y = 0; y < th; y++) {
        const uint8_t *row = px + y * row_stride;
        for (int x = 0; x < tw; x++)
            cpx[y * tw + x] = zrle_read_cpx(row + x * pbytes + cpx_off, cpx_n);
    }

    uint32_t pal[128];
    uint8_t pidx[ZRLE_TILE * ZRLE_TILE];
    int pal_n = 0;
    bool overflow = false;

    for (int i = 0; i < np; i++) {
        uint32_t c = cpx[i];
        int j;
        for (j = 0; j < pal_n; j++)
            if (pal[j] == c) break;
        if (j == pal_n) {
            if (pal_n >= 128) { overflow = true; break; }
            pal[pal_n++] = c;
        }
        pidx[i] = (uint8_t)j;
    }

    size_t pos = 0;

    if (!overflow && pal_n == 1) {
        // Solid
        out[pos++] = 1;
        zrle_write_cpx(out + pos, pal[0], cpx_n);
        pos += cpx_n;
    } else if (!overflow && pal_n <= 16) {
        // Packed palette
        out[pos++] = (uint8_t)pal_n;
        for (int i = 0; i < pal_n; i++) {
            zrle_write_cpx(out + pos, pal[i], cpx_n);
            pos += cpx_n;
        }
        int bpp = (pal_n <= 2) ? 1 : (pal_n <= 4) ? 2 : 4;
        for (int y = 0; y < th; y++) {
            int bp = 0;
            uint8_t byte = 0;
            for (int x = 0; x < tw; x++) {
                byte |= pidx[y * tw + x] << (8 - bpp - bp);
                bp += bpp;
                if (bp >= 8) { out[pos++] = byte; byte = 0; bp = 0; }
            }
            if (bp > 0) out[pos++] = byte;
        }
    } else if (!overflow && pal_n <= 127) {
        // Palette RLE
        out[pos++] = (uint8_t)(128 + pal_n);
        for (int i = 0; i < pal_n; i++) {
            zrle_write_cpx(out + pos, pal[i], cpx_n);
            pos += cpx_n;
        }
        int i = 0;
        while (i < np) {
            uint8_t idx = pidx[i];
            int run = 1;
            while (i + run < np && pidx[i + run] == idx) run++;
            if (run == 1) {
                out[pos++] = idx;
            } else {
                out[pos++] = idx | 0x80;
                pos += zrle_write_rle_len(out + pos, run);
            }
            i += run;
        }
    } else {
        // Plain RLE
        out[pos++] = 128;
        int i = 0;
        while (i < np) {
            uint32_t c = cpx[i];
            int run = 1;
            while (i + run < np && cpx[i + run] == c) run++;
            zrle_write_cpx(out + pos, c, cpx_n);
            pos += cpx_n;
            pos += zrle_write_rle_len(out + pos, run);
            i += run;
        }
    }

    // Fallback to raw if smaller
    size_t raw_sz = 1 + (size_t)np * cpx_n;
    if (pos > raw_sz) {
        pos = 0;
        out[pos++] = 0;
        for (int i = 0; i < np; i++) {
            zrle_write_cpx(out + pos, cpx[i], cpx_n);
            pos += cpx_n;
        }
    }
    return pos;
}

static bool ensure_zrle_buf(RFBClientState *c, size_t need) {
    if (need > c->zrle_buf_cap) {
        free(c->zrle_buf);
        c->zrle_buf = malloc(need);
        c->zrle_buf_cap = c->zrle_buf ? need : 0;
    }
    return c->zrle_buf != NULL;
}

bool rfb_send_fb_update_rect_zrle(RFBClientState *client, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint8_t *pixels, int stride, int src_bpp) {
    const RFBPixelFormat *fmt = client->format_set ? &client->client_format : NULL;
    int pixel_bytes = fmt ? (fmt->bpp / 8) : 4;
    bool use_cpx3 = fmt ? (fmt->bpp == 32 && fmt->depth <= 24 && fmt->true_color)
                        : true;
    int cpx_n = use_cpx3 ? 3 : pixel_bytes;
    int cpx_off = (use_cpx3 && fmt && fmt->big_endian) ? 1 : 0;

    int tiles_x = (width + ZRLE_TILE - 1) / ZRLE_TILE;
    int tiles_y = (height + ZRLE_TILE - 1) / ZRLE_TILE;
    size_t max_raw = (size_t)tiles_x * tiles_y *
                     (1 + ZRLE_TILE * ZRLE_TILE * (cpx_n + 1));
    if (!ensure_zrle_buf(client, max_raw)) return false;

    // Per-tile conversion buffer on stack
    uint8_t tile_conv[ZRLE_TILE * ZRLE_TILE * 4];
    // Per-tile encoding output (worst case: plain RLE with no runs)
    uint8_t tile_enc[1 + ZRLE_TILE * ZRLE_TILE * 8];

    size_t raw_pos = 0;

    for (uint16_t ty = 0; ty < height; ty += ZRLE_TILE) {
        uint16_t th = (ty + ZRLE_TILE <= height) ? ZRLE_TILE : height - ty;
        for (uint16_t tx = 0; tx < width; tx += ZRLE_TILE) {
            uint16_t tw = (tx + ZRLE_TILE <= width) ? ZRLE_TILE : width - tx;

            const uint8_t *tile_src = pixels + (size_t)ty * stride + (size_t)tx * src_bpp;
            const uint8_t *tile_px;
            int tile_stride, tile_pb;

            if (client->format_is_bgra && src_bpp == 4) {
                tile_px = tile_src;
                tile_stride = stride;
                tile_pb = 4;
            } else if (fmt) {
                for (int r = 0; r < th; r++) {
                    const uint8_t *sr = tile_src + r * stride;
                    uint8_t *dr = tile_conv + r * tw * pixel_bytes;
                    for (int c = 0; c < tw; c++)
                        convert_pixel(sr + c * src_bpp, dr + c * pixel_bytes, src_bpp, fmt);
                }
                tile_px = tile_conv;
                tile_stride = tw * pixel_bytes;
                tile_pb = pixel_bytes;
            } else {
                for (int r = 0; r < th; r++) {
                    const uint8_t *sr = tile_src + r * stride;
                    uint8_t *dr = tile_conv + r * tw * 4;
                    for (int c = 0; c < tw; c++) {
                        dr[c*4+0] = sr[c*src_bpp+2];
                        dr[c*4+1] = sr[c*src_bpp+1];
                        dr[c*4+2] = sr[c*src_bpp+0];
                        dr[c*4+3] = 0;
                    }
                }
                tile_px = tile_conv;
                tile_stride = tw * 4;
                tile_pb = 4;
            }

            size_t tsz = zrle_encode_tile(tile_px, tw, th, tile_stride,
                                           tile_pb, cpx_n, cpx_off, tile_enc);
            memcpy(client->zrle_buf + raw_pos, tile_enc, tsz);
            raw_pos += tsz;
        }
    }

    // Compress
    if (!client->zrle_initialized) {
        memset(&client->zrle_stream, 0, sizeof(client->zrle_stream));
        if (deflateInit(&client->zrle_stream, Z_BEST_SPEED) != Z_OK)
            return false;
        client->zrle_initialized = true;
    }

    size_t max_comp = deflateBound(&client->zrle_stream, raw_pos);
    if (max_comp > client->compress_buf_cap) {
        free(client->compress_buf);
        client->compress_buf = malloc(max_comp);
        client->compress_buf_cap = client->compress_buf ? max_comp : 0;
    }
    if (!client->compress_buf) return false;

    client->zrle_stream.next_in = client->zrle_buf;
    client->zrle_stream.avail_in = (uInt)raw_pos;
    client->zrle_stream.next_out = client->compress_buf;
    client->zrle_stream.avail_out = (uInt)client->compress_buf_cap;

    if (deflate(&client->zrle_stream, Z_SYNC_FLUSH) != Z_OK)
        return false;

    size_t comp_sz = client->compress_buf_cap - client->zrle_stream.avail_out;

    RFBFramebufferRect rect;
    rect.x = htons(x);
    rect.y = htons(y);
    rect.width = htons(width);
    rect.height = htons(height);
    rect.encoding = htonl(RFB_ENCODING_ZRLE);

    uint32_t zlen = htonl((uint32_t)comp_sz);

    return rfb_send_all(client->sock_fd, &rect, sizeof(rect)) &&
           rfb_send_all(client->sock_fd, &zlen, 4) &&
           rfb_send_all(client->sock_fd, client->compress_buf, comp_sz);
}

bool rfb_client_supports_encoding(RFBClientState *client, int32_t encoding) {
    for (int i = 0; i < client->num_encodings; i++) {
        if (client->supported_encodings[i] == encoding)
            return true;
    }
    return false;
}

void rfb_client_state_init(RFBClientState *client, int sock_fd) {
    memset(client, 0, sizeof(*client));
    client->sock_fd = sock_fd;
}

void rfb_client_state_cleanup(RFBClientState *client) {
    free(client->supported_encodings);
    client->supported_encodings = NULL;
    free(client->convert_buf);
    client->convert_buf = NULL;
    client->convert_buf_cap = 0;
    free(client->compress_buf);
    client->compress_buf = NULL;
    client->compress_buf_cap = 0;
    free(client->zrle_buf);
    client->zrle_buf = NULL;
    client->zrle_buf_cap = 0;
    if (client->zlib_initialized) {
        deflateEnd(&client->zlib_stream);
        client->zlib_initialized = false;
    }
    if (client->zrle_initialized) {
        deflateEnd(&client->zrle_stream);
        client->zrle_initialized = false;
    }
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }
}
