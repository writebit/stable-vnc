#include "vnc_server.h"
#include "input_inject.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#define TAG "VNC"

static VNCServerStatusCallback g_status_callback = NULL;

void vnc_server_set_status_callback(VNCServerStatusCallback cb) {
    g_status_callback = cb;
}

static void notify_status(VNCServer *server) {
    if (g_status_callback) {
        g_status_callback(server->active_client_count, server->running);
    }
}

#define TILE_SIZE 32
#define MAX_DIRTY_RECTS 4096

typedef struct {
    uint16_t x, y, w, h;
} DirtyRect;

static void send_framebuffer(VNCClientConnection *conn) {
    VNCServer *server = conn->server;
    ScreenCapture *cap = &server->capture;

    if (!screen_capture_grab(cap)) return;

    size_t expected_size = (size_t)cap->stride * cap->height;
    if (cap->data_size != expected_size || !server->prev_frame) {
        free(server->prev_frame);
        cap->data_size = expected_size;
        server->prev_frame = calloc(1, cap->data_size);
        if (!server->prev_frame) {
            LOG_ERROR(TAG, "Failed to allocate prev_frame (%zu bytes)", cap->data_size);
            return;
        }
    }

    uint16_t rx = conn->rfb.req_x;
    uint16_t ry = conn->rfb.req_y;
    uint16_t rw = conn->rfb.req_w;
    uint16_t rh = conn->rfb.req_h;

    if (rx + rw > cap->width) rw = cap->width - rx;
    if (ry + rh > cap->height) rh = cap->height - ry;
    if (rw == 0 || rh == 0) return;

    /*
    bool use_zrle = rfb_client_supports_encoding(&conn->rfb, RFB_ENCODING_ZRLE);
    bool use_zlib = !use_zrle && rfb_client_supports_encoding(&conn->rfb, RFB_ENCODING_ZLIB);
    */
    
    bool use_zlib = rfb_client_supports_encoding(&conn->rfb, RFB_ENCODING_ZLIB);
    bool use_zrle = !use_zlib && rfb_client_supports_encoding(&conn->rfb, RFB_ENCODING_ZRLE);

    if (!conn->rfb.incremental || !server->prev_frame) {
        const uint8_t *region = cap->data + (size_t)ry * cap->stride + (size_t)rx * 4;
        bool ok;
        if (use_zrle) {
            ok = rfb_send_fb_update_header(&conn->rfb, 1) &&
                 rfb_send_fb_update_rect_zrle(&conn->rfb, rx, ry, rw, rh, region, cap->stride, 4);
        } else if (use_zlib)
            ok = rfb_send_framebuffer_update_zlib(&conn->rfb, rx, ry, rw, rh, region, cap->stride, 4);
        else
            ok = rfb_send_framebuffer_update_raw(&conn->rfb, rx, ry, rw, rh, region, cap->stride, 4);
        if (!ok) {
            LOG_ERROR(TAG, "Failed to send full framebuffer to client %d", conn->index);
            conn->active = false;
        }
        memcpy(server->prev_frame, cap->data, cap->data_size);
        conn->rfb.update_pending = false;
        return;
    }

    DirtyRect rects[MAX_DIRTY_RECTS];
    int num_rects = 0;

    for (uint16_t row = ry; row < ry + rh; row += TILE_SIZE) {
        uint16_t th = (row + TILE_SIZE <= ry + rh) ? TILE_SIZE : (ry + rh - row);
        int span_start = -1;

        for (uint16_t col = rx; col <= rx + rw; col += TILE_SIZE) {
            uint16_t tw = (col + TILE_SIZE <= rx + rw) ? TILE_SIZE : (rx + rw - col);
            bool dirty = false;

            if (col < rx + rw) {
                for (uint16_t by = 0; by < th; by++) {
                    size_t offset = (size_t)(row + by) * cap->stride + (size_t)col * 4;
                    if (offset + (size_t)tw * 4 <= cap->data_size &&
                        memcmp(cap->data + offset, server->prev_frame + offset, tw * 4) != 0) {
                        dirty = true;
                        break;
                    }
                }
            }

            if (dirty) {
                if (span_start < 0) span_start = col;
            }

            if (!dirty || col + TILE_SIZE >= rx + rw) {
                if (span_start >= 0) {
                    uint16_t span_end = dirty ? (col + tw) : col;
                    if (num_rects < MAX_DIRTY_RECTS) {
                        rects[num_rects].x = (uint16_t)span_start;
                        rects[num_rects].y = row;
                        rects[num_rects].w = span_end - (uint16_t)span_start;
                        rects[num_rects].h = th;
                        num_rects++;
                    }
                    span_start = -1;
                }
            }
        }
    }

    if (num_rects == 0)
        return;

    // Vertical merge: combine rects with same x/w in consecutive tile rows
    for (int i = 1; i < num_rects; i++) {
        for (int j = i - 1; j >= 0; j--) {
            if (rects[j].h == 0) continue;
            if (rects[j].y + rects[j].h < rects[i].y) break;
            if (rects[j].x == rects[i].x && rects[j].w == rects[i].w &&
                rects[j].y + rects[j].h == rects[i].y) {
                rects[j].h += rects[i].h;
                rects[i].h = 0;
                break;
            }
        }
    }

    int send_count = 0;
    for (int i = 0; i < num_rects; i++)
        if (rects[i].h > 0) send_count++;

    int nopush = 1;
    setsockopt(conn->rfb.sock_fd, IPPROTO_TCP, TCP_NOPUSH, &nopush, sizeof(nopush));

    if (!rfb_send_fb_update_header(&conn->rfb, (uint16_t)send_count)) {
        conn->active = false;
        goto flush;
    }

    for (int i = 0; i < num_rects; i++) {
        if (rects[i].h == 0) continue;
        const uint8_t *tile_data = cap->data + (size_t)rects[i].y * cap->stride + (size_t)rects[i].x * 4;
        bool ok;
        if (use_zrle)
            ok = rfb_send_fb_update_rect_zrle(&conn->rfb, rects[i].x, rects[i].y,
                                               rects[i].w, rects[i].h, tile_data, cap->stride, 4);
        else if (use_zlib)
            ok = rfb_send_fb_update_rect_zlib(&conn->rfb, rects[i].x, rects[i].y,
                                               rects[i].w, rects[i].h, tile_data, cap->stride, 4);
        else
            ok = rfb_send_fb_update_rect_raw(&conn->rfb, rects[i].x, rects[i].y,
                                              rects[i].w, rects[i].h, tile_data, cap->stride, 4);
        if (!ok) {
            LOG_ERROR(TAG, "Failed to send rect %d to client %d", i, conn->index);
            conn->active = false;
            goto flush;
        }
    }

    LOG_DEBUG(TAG, "Sent %d dirty rects to client %d (from %d tiles)", send_count, conn->index, num_rects);

    for (int i = 0; i < num_rects; i++) {
        if (rects[i].h == 0) continue;
        for (uint16_t row = 0; row < rects[i].h; row++) {
            size_t off = (size_t)(rects[i].y + row) * cap->stride + (size_t)rects[i].x * 4;
            memcpy(server->prev_frame + off, cap->data + off, (size_t)rects[i].w * 4);
        }
    }
    conn->rfb.update_pending = false;

flush:
    nopush = 0;
    setsockopt(conn->rfb.sock_fd, IPPROTO_TCP, TCP_NOPUSH, &nopush, sizeof(nopush));
}

static void *client_thread(void *arg) {
    VNCClientConnection *conn = (VNCClientConnection *)arg;
    VNCServer *server = conn->server;

    LOG_INFO(TAG, "Client %d connected (fd=%d)", conn->index, conn->rfb.sock_fd);

    if (!rfb_perform_handshake(conn->rfb.sock_fd, server->password)) {
        LOG_ERROR(TAG, "Handshake failed for client %d", conn->index);
        goto done;
    }

    if (!rfb_send_server_init(conn->rfb.sock_fd, server->capture.width,
                               server->capture.height, VNC_SERVER_NAME)) {
        LOG_ERROR(TAG, "ServerInit failed for client %d", conn->index);
        goto done;
    }

    while (conn->active && server->running) {
        struct pollfd pfd;
        pfd.fd = conn->rfb.sock_fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 33); // ~30fps max
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR(TAG, "poll() failed for client %d: %s", conn->index, strerror(errno));
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_INFO(TAG, "Client %d: connection error/hangup (revents=0x%x)",
                     conn->index, pfd.revents);
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            int msg = rfb_read_client_message(&conn->rfb);
            if (msg < 0) break;

            if (msg == RFB_MSG_KEY_EVENT) {
                input_inject_key_event(conn->rfb.last_key, conn->rfb.last_key_down);
            } else if (msg == RFB_MSG_POINTER_EVENT) {
                uint16_t px = conn->rfb.last_pointer_x;
                uint16_t py = conn->rfb.last_pointer_y;
                uint8_t buttons = conn->rfb.last_pointer_buttons;
                input_inject_mouse_move(px, py, server->capture.width, server->capture.height);
                input_inject_mouse_button(px, py, buttons,
                                           server->capture.width, server->capture.height);
            }
        }

        if (conn->rfb.update_pending) {
            send_framebuffer(conn);
        }
    }

done:
    LOG_INFO(TAG, "Client %d disconnected", conn->index);
    rfb_client_state_cleanup(&conn->rfb);
    conn->active = false;

    pthread_mutex_lock(&server->clients_mutex);
    server->active_client_count--;
    pthread_mutex_unlock(&server->clients_mutex);

    notify_status(server);
    return NULL;
}

static int create_listen_socket(int family, uint16_t port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (family == AF_INET6) {
        int v6only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }

    if (family == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    }

    if (listen(fd, 5) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void *accept_thread_func(void *arg) {
    VNCServer *server = ((void **)arg)[0];
    int listen_fd = *(int *)((void **)arg)[1];
    free(arg);

    while (server->running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char addr_str[INET6_ADDRSTRLEN] = {0};
        uint16_t remote_port = 0;
        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
            remote_port = ntohs(sa->sin_port);
        } else {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &sa6->sin6_addr, addr_str, sizeof(addr_str));
            remote_port = ntohs(sa6->sin6_port);
        }
        LOG_INFO(TAG, "Connection from %s:%d", addr_str, remote_port);

        pthread_mutex_lock(&server->clients_mutex);
        int slot = -1;
        for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
            if (!server->clients[i].active) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            pthread_mutex_unlock(&server->clients_mutex);
            LOG_WARN(TAG, "Max clients reached, rejecting connection from %s", addr_str);
            close(client_fd);
            continue;
        }

        VNCClientConnection *conn = &server->clients[slot];
        rfb_client_state_init(&conn->rfb, client_fd);
        conn->active = true;
        conn->index = slot;
        conn->server = server;
        server->active_client_count++;
        pthread_mutex_unlock(&server->clients_mutex);

        notify_status(server);
        pthread_create(&conn->thread, NULL, client_thread, conn);
        pthread_detach(conn->thread);
    }
    return NULL;
}

void vnc_server_set_password(VNCServer *server, const char *password) {
    memset(server->password, 0, VNC_MAX_PASSWORD);
    if (password && password[0] != '\0') {
        strncpy(server->password, password, VNC_MAX_PASSWORD - 1);
        LOG_INFO(TAG, "Password authentication enabled");
    } else {
        LOG_INFO(TAG, "Password authentication disabled");
    }
}

bool vnc_server_init(VNCServer *server, uint16_t port) {
    char saved_pw[VNC_MAX_PASSWORD];
    memcpy(saved_pw, server->password, VNC_MAX_PASSWORD);
    memset(server, 0, sizeof(*server));
    memcpy(server->password, saved_pw, VNC_MAX_PASSWORD);
    server->port = port;
    server->listen_fd4 = -1;
    server->listen_fd6 = -1;
    pthread_mutex_init(&server->clients_mutex, NULL);

    LOG_INFO(TAG, "Initializing server on port %d", port);

    if (!screen_capture_init(&server->capture)) {
        LOG_ERROR(TAG, "Failed to initialize screen capture");
        return false;
    }

    server->prev_frame = calloc(1, server->capture.data_size);
    if (!server->prev_frame) {
        LOG_ERROR(TAG, "Failed to allocate prev_frame (%zu bytes)", server->capture.data_size);
        return false;
    }
    return true;
}

bool vnc_server_start(VNCServer *server) {
    server->listen_fd4 = create_listen_socket(AF_INET, server->port);
    if (server->listen_fd4 < 0) {
        LOG_WARN(TAG, "Failed to create IPv4 listen socket on port %d: %s",
                 server->port, strerror(errno));
    } else {
        LOG_INFO(TAG, "Listening on 0.0.0.0:%d (IPv4)", server->port);
    }

    server->listen_fd6 = create_listen_socket(AF_INET6, server->port);
    if (server->listen_fd6 < 0) {
        LOG_WARN(TAG, "Failed to create IPv6 listen socket on port %d: %s",
                 server->port, strerror(errno));
    } else {
        LOG_INFO(TAG, "Listening on [::]:%d (IPv6)", server->port);
    }

    if (server->listen_fd4 < 0 && server->listen_fd6 < 0) {
        LOG_ERROR(TAG, "Failed to bind any socket on port %d", server->port);
        return false;
    }

    server->running = true;

    if (server->listen_fd4 >= 0) {
        void **arg = malloc(2 * sizeof(void *));
        arg[0] = server;
        arg[1] = &server->listen_fd4;
        pthread_create(&server->accept_thread4, NULL, accept_thread_func, arg);
    }

    if (server->listen_fd6 >= 0) {
        void **arg = malloc(2 * sizeof(void *));
        arg[0] = server;
        arg[1] = &server->listen_fd6;
        pthread_create(&server->accept_thread6, NULL, accept_thread_func, arg);
    }

    notify_status(server);
    LOG_INFO(TAG, "Server started successfully");
    return true;
}

void vnc_server_stop(VNCServer *server) {
    LOG_INFO(TAG, "Stopping server...");
    server->running = false;

    if (server->listen_fd4 >= 0) {
        close(server->listen_fd4);
        server->listen_fd4 = -1;
        pthread_join(server->accept_thread4, NULL);
    }
    if (server->listen_fd6 >= 0) {
        close(server->listen_fd6);
        server->listen_fd6 = -1;
        pthread_join(server->accept_thread6, NULL);
    }

    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
        if (server->clients[i].active) {
            LOG_INFO(TAG, "Disconnecting client %d", i);
            server->clients[i].active = false;
            close(server->clients[i].rfb.sock_fd);
            server->clients[i].rfb.sock_fd = -1;
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);

    usleep(100000);
    notify_status(server);
    LOG_INFO(TAG, "Server stopped");
}

void vnc_server_cleanup(VNCServer *server) {
    vnc_server_stop(server);
    screen_capture_cleanup(&server->capture);
    free(server->prev_frame);
    server->prev_frame = NULL;
    pthread_mutex_destroy(&server->clients_mutex);
}

int vnc_server_get_client_count(VNCServer *server) {
    pthread_mutex_lock(&server->clients_mutex);
    int count = server->active_client_count;
    pthread_mutex_unlock(&server->clients_mutex);
    return count;
}
