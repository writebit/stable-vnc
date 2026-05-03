#ifndef VNC_SERVER_H
#define VNC_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "rfb_protocol.h"
#include "screen_capture.h"

#define VNC_MAX_CLIENTS 8
#define VNC_DEFAULT_PORT 5900
#define VNC_SERVER_NAME "StableVNC"

typedef struct {
    RFBClientState rfb;
    pthread_t thread;
    bool active;
    int index;
    struct VNCServer *server;
} VNCClientConnection;

#define VNC_MAX_PASSWORD 128

typedef struct VNCServer {
    int listen_fd4;
    int listen_fd6;
    uint16_t port;
    bool running;
    pthread_t accept_thread4;
    pthread_t accept_thread6;
    ScreenCapture capture;
    uint8_t *prev_frame;
    VNCClientConnection clients[VNC_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    int active_client_count;
    char password[VNC_MAX_PASSWORD];
} VNCServer;

typedef void (*VNCServerStatusCallback)(int client_count, bool running);

bool vnc_server_init(VNCServer *server, uint16_t port);
void vnc_server_set_password(VNCServer *server, const char *password);
bool vnc_server_start(VNCServer *server);
void vnc_server_stop(VNCServer *server);
void vnc_server_cleanup(VNCServer *server);
int vnc_server_get_client_count(VNCServer *server);
void vnc_server_set_status_callback(VNCServerStatusCallback cb);

#endif
