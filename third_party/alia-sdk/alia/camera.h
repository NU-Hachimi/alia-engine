/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NU Hachimi (Alia Pad cart SDK)
 *
 * The camera for carts. Header-only, C99 / C++, POSIX; no GL, no threads. Protocol v2
 * (alia/camera_proto.h; the devkit's docs/camera.md); the Python alia.camera is the same client.
 *
 *   alia_camera cam;
 *   alia_camera_connect(&cam, NULL);              // the camera's socket (inside a cart, or CAM_SOCKET)
 *   alia_camera_open(&cam, 1280, 720, 1);         // the size you draw at; cam.mode says what you got
 *   each frame of your game:
 *     alia_camera_poll(&cam);                     // reads what arrived, never blocks
 *     if (cam.mode_generation != mine) { drop your imports }
 *       (buffer_fd[i] can still be -1 just after mode_generation changes: the BUFFER messages
 *       may arrive in a later poll than their MODE. Import each buffer when a FRAME first
 *       names it; a FRAME's buffer has always arrived before the FRAME.)
 *     const alia_camera_frame* f = alia_camera_next(&cam);   // newest; older ones go back
 *     if (f) { draw with cam.buffer_fd[f->index], cam.mode, alia_camera_params_of(&cam, f) and
 *              the reference shader (sdk/camera/alia_camera.glsl); after its GPU fence:
 *              alia_camera_release(&cam, f->index); }
 *   alia_camera_photo(&cam, 7); ... cam.photo.request == 7: cam.photo.fd is a memfd with the JPEG
 *     (cam.photo.size bytes; yours to close), or cam.error.request == 7
 *   alia_camera_close(&cam);
 *
 * poll() the fd (alia_camera_fd) in your own loop if you like. cam.pending: the user is being
 * asked; cam.paused: a photo is being taken (a new MODE and buffers come before frames resume).
 * Functions return >= 0, or -errno; -EPROTO after a fatal ERROR (cam.error says which).
 */
#ifndef ALIA_CAMERA_H
#define ALIA_CAMERA_H

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "camera_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALIA_CAMERA_MAX_BUFFERS 16
#define ALIA_CAMERA_PARAMS_KEPT 4

typedef struct {
    int index;                     /* the buffer: cam.buffer_fd[index] */
    uint32_t sequence, params;     /* params: the PARAMS id (alia_camera_params_of) */
    uint64_t timestamp_ns;         /* CLOCK_MONOTONIC, end of exposure */
    uint32_t exposure_us;
    float gain, wb_red, wb_blue, colour_temperature;
} alia_camera_frame;

typedef struct {
    int fd;                        /* -1 while still connecting */
    int pending, paused;
    uint32_t server_version;
    struct cam_mode mode;          /* valid once mode_generation > 0 */
    uint32_t mode_generation;      /* bumped per MODE: the buffers are new, drop the old imports */
    int buffer_fd[ALIA_CAMERA_MAX_BUFFERS];
    struct cam_params params[ALIA_CAMERA_PARAMS_KEPT];  /* the newest few, ring */
    int params_next;
    alia_camera_frame frames[CAM_MAX_HELD + 2];  /* received, not yet taken by next() */
    int n_frames;
    unsigned held;                 /* bit per buffer: frames the client has (to RELEASE) */
    alia_camera_frame current;     /* what next() returned */
    struct { uint32_t request, size; int fd; } photo;            /* the newest PHOTO_DONE; fd -1: none */
    struct { uint32_t code, request; char message[116]; } error; /* the newest ERROR; code 0: none */
} alia_camera;

static inline int alia_camera_send_(alia_camera* c, const void* m, size_t n) {
    return send(c->fd, m, n, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)n ? 0 : -errno;
}

/* path NULL: the socket a cart sees (/run/alia/camera.sock), else CAM_SOCKET */
static inline int alia_camera_connect(alia_camera* c, const char* path) {
    memset(c, 0, sizeof *c);
    c->photo.fd = -1;
    for (int i = 0; i < ALIA_CAMERA_MAX_BUFFERS; i++) c->buffer_fd[i] = -1;
    if (!path) path = access("/run/alia/camera.sock", F_OK) == 0 ? "/run/alia/camera.sock" : CAM_SOCKET;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) return -ENAMETOOLONG;
    strcpy(a.sun_path, path);
    c->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0) return -errno;
    if (connect(c->fd, (struct sockaddr*)&a, sizeof a) < 0) {
        int e = -errno;
        close(c->fd);
        c->fd = -1;
        return e;
    }
    struct cam_hello h = {CAM_MSG_HELLO, CAM_PROTO_VERSION};
    return alia_camera_send_(c, &h, sizeof h);
}

static inline int alia_camera_fd(const alia_camera* c) { return c->fd; }

static inline int alia_camera_open(alia_camera* c, uint32_t width, uint32_t height, int full_fov) {
    struct cam_open o = {CAM_MSG_OPEN, width, height, full_fov ? 1u : 0u};
    return alia_camera_send_(c, &o, sizeof o);
}

static inline int alia_camera_release(alia_camera* c, int index) {
    if (index < 0 || index >= ALIA_CAMERA_MAX_BUFFERS || !(c->held & (1u << index))) return 0;
    c->held &= ~(1u << index);
    struct cam_release r = {CAM_MSG_RELEASE, (uint32_t)index};
    return alia_camera_send_(c, &r, sizeof r);
}

static inline int alia_camera_photo(alia_camera* c, uint32_t request) {
    struct cam_photo p = {CAM_MSG_PHOTO, request};
    return alia_camera_send_(c, &p, sizeof p);
}

/* The PARAMS a frame names (always received before the frame), or NULL. */
static inline const struct cam_params* alia_camera_params_of(const alia_camera* c, const alia_camera_frame* f) {
    for (int i = 0; i < ALIA_CAMERA_PARAMS_KEPT; i++)
        if (c->params[i].type == CAM_MSG_PARAMS && c->params[i].id == f->params) return &c->params[i];
    return NULL;
}

static inline void alia_camera_drop_buffers_(alia_camera* c) {
    for (int i = 0; i < ALIA_CAMERA_MAX_BUFFERS; i++)
        if (c->buffer_fd[i] >= 0) close(c->buffer_fd[i]), c->buffer_fd[i] = -1;
    c->n_frames = 0, c->held = 0, c->current.index = -1;
}

/* Read everything that arrived. Returns the number of messages, 0 if none, -EPIPE when the
 * camera hung up, -EPROTO after a fatal ERROR (cam.error), or -errno. */
static inline int alia_camera_poll(alia_camera* c) {
    static union { struct cam_params p; char bytes[sizeof(struct cam_params) + 64]; } buf; /* one thread */
    int count = 0;
    for (;;) {
        char ctl[CMSG_SPACE(sizeof(int))];
        struct iovec iov = {buf.bytes, sizeof buf.bytes};
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = &iov, m.msg_iovlen = 1, m.msg_control = ctl, m.msg_controllen = sizeof ctl;
        ssize_t n = recvmsg(c->fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? count : -errno;
        if (n == 0) return -EPIPE;
        int fd = -1;
        for (struct cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(h), sizeof fd);
        uint32_t type;
        if (n < (ssize_t)sizeof type) return -EPROTO;
        memcpy(&type, buf.bytes, sizeof type);
        count++;
        if (type == CAM_MSG_SERVER_HELLO && n >= (ssize_t)sizeof(struct cam_hello)) {
            struct cam_hello h;
            memcpy(&h, buf.bytes, sizeof h);
            c->server_version = h.version;
        } else if (type == CAM_MSG_MODE && n >= (ssize_t)sizeof(struct cam_mode)) {
            alia_camera_drop_buffers_(c);
            memcpy(&c->mode, buf.bytes, sizeof c->mode);
            c->mode_generation++, c->pending = 0;
        } else if (type == CAM_MSG_BUFFER && n >= (ssize_t)sizeof(struct cam_buffer)) {
            struct cam_buffer b;
            memcpy(&b, buf.bytes, sizeof b);
            if (b.index < ALIA_CAMERA_MAX_BUFFERS && fd >= 0) c->buffer_fd[b.index] = fd, fd = -1;
        } else if (type == CAM_MSG_PARAMS && n >= (ssize_t)sizeof(struct cam_params)) {
            memcpy(&c->params[c->params_next], buf.bytes, sizeof(struct cam_params));
            c->params_next = (c->params_next + 1) % ALIA_CAMERA_PARAMS_KEPT;
        } else if (type == CAM_MSG_FRAME && n >= (ssize_t)sizeof(struct cam_frame)) {
            struct cam_frame f;
            memcpy(&f, buf.bytes, sizeof f);
            if (f.index < ALIA_CAMERA_MAX_BUFFERS) {
                alia_camera_frame x = {(int)f.index, f.sequence, f.params, f.timestamp_ns, f.exposure_us,
                                       f.gain, f.wb_red, f.wb_blue, f.colour_temperature};
                c->held |= 1u << f.index;
                if (c->n_frames == (int)(sizeof c->frames / sizeof c->frames[0])) {  /* full: the oldest goes back */
                    alia_camera_release(c, c->frames[0].index);
                    memmove(c->frames, c->frames + 1, sizeof c->frames[0] * (size_t)(c->n_frames - 1));
                    c->n_frames--;
                }
                c->frames[c->n_frames++] = x;
            }
        } else if (type == CAM_MSG_PENDING) {
            c->pending = 1;
        } else if (type == CAM_MSG_PAUSED) {
            c->paused = 1;
        } else if (type == CAM_MSG_RESUMED) {
            c->paused = 0;
        } else if (type == CAM_MSG_PHOTO_DONE && n >= (ssize_t)sizeof(struct cam_photo_done)) {
            struct cam_photo_done d;
            memcpy(&d, buf.bytes, sizeof d);
            if (c->photo.fd >= 0) close(c->photo.fd);  /* the last one was never taken */
            c->photo.request = d.request, c->photo.size = d.size, c->photo.fd = fd, fd = -1;
        } else if (type == CAM_MSG_ERROR && n >= (ssize_t)sizeof(struct cam_error)) {
            struct cam_error e;
            memcpy(&e, buf.bytes, sizeof e);
            c->error.code = e.code, c->error.request = e.request;
            memcpy(c->error.message, e.message, sizeof c->error.message);
            c->error.message[sizeof c->error.message - 1] = 0;
            if (fd >= 0) close(fd);
            if (!e.request) return -EPROTO;  /* fatal: the camera hangs up */
            continue;
        }
        if (fd >= 0) close(fd);
    }
}

/* The newest frame received since the last call (the older ones go back to the camera), or
 * NULL. Valid until the next poll(); release its index once drawn. */
static inline const alia_camera_frame* alia_camera_next(alia_camera* c) {
    if (!c->n_frames) return NULL;
    for (int i = 0; i < c->n_frames - 1; i++) alia_camera_release(c, c->frames[i].index);
    c->current = c->frames[c->n_frames - 1];
    c->n_frames = 0;
    return &c->current;
}

static inline void alia_camera_close(alia_camera* c) {
    if (c->fd >= 0) {
        struct cam_close m = {CAM_MSG_CLOSE};
        alia_camera_send_(c, &m, sizeof m);
        close(c->fd);
        c->fd = -1;
    }
    alia_camera_drop_buffers_(c);
    if (c->photo.fd >= 0) close(c->photo.fd), c->photo.fd = -1;
}

#ifdef __cplusplus
}
#endif
#endif
