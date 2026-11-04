/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 NU Hachimi (Alia Pad cart SDK)
 *
 * The Alia Pad camera's frame protocol, version 2 (the devkit's docs/camera.md; the client is
 * alia/camera.h). A SOCK_SEQPACKET socket: one message
 * per packet, little-endian, every message starts with its type; sizes are fixed (asserted below:
 * the Python client's struct formats are checked against them).
 *
 *   client                          server
 *   HELLO {version}           ->    SERVER_HELLO {version}
 *   OPEN {w, h, full_fov}     ->    [PENDING: the user is being asked; the system pauses the cart meanwhile]
 *                                   MODE {...}, BUFFER {index, size} + fd, nbuf times,
 *                                   PARAMS {id, ...}  (always before the first FRAME after a MODE)
 *                                   FRAME {index, seq, ts, ..., params id} ...
 *                                   PARAMS {id, ...}  again whenever the colour changes; a FRAME
 *                                                     never names a PARAMS id not yet sent
 *   RELEASE {index}           ->    (the buffer goes back to the camera)
 *   PHOTO {request}           ->    PHOTO_DONE {request, size} + memfd (JPEG), or ERROR {request}
 *                                   meanwhile: PAUSED {photo}, MODE + BUFFERs (new: drop the
 *                                   old imports), PARAMS, RESUMED
 *   CLOSE / hang up           ->    the stream stops, the sensor powers down
 *                                   ERROR {code, 0, message} then hang up, on anything fatal
 *
 * Buffers: 8-bit Bayer, imported once each as an R8 texture (stride a multiple of 64, offset 0,
 * modifier LINEAR); mmap-able too (where a dmabuf import isn't possible). A client may hold
 * CAM_MAX_HELD frames: RELEASE each once drawn (after its GPU fence); one that doesn't simply
 * gets fewer frames, the camera never waits. One client at a time.
 *
 * Colour (the reference GLSL does exactly this): bilinear demosaic; (v - black) / (255 - black);
 * x white balance x colour shading (red, blue); clip to 1; x luminance shading (all channels);
 * x ccm (-> linear sRGB); clamp 0..1; tone[] (-> display-encoded, nothing after). The shading grid
 * covers the current mode's picture, samples at cell centres (a bilinear 16x12 texture read at
 * (pixel + 0.5) / size). turn: 0 or 90, clockwise from the handheld's landscape screen-up; the
 * intrinsics are in raw pixels before it. */
#ifndef ALIA_CAMERA_PROTO_H
#define ALIA_CAMERA_PROTO_H

#include <stdint.h>

#define CAM_PROTO_VERSION 2
#define CAM_SOCKET "/run/alia-camera/frames.sock"
#define CAM_MAX_HELD 2

enum {
    /* client -> server */
    CAM_MSG_HELLO = 1,
    CAM_MSG_OPEN = 2,
    CAM_MSG_RELEASE = 3,
    CAM_MSG_CLOSE = 4,
    CAM_MSG_PHOTO = 5,
    /* server -> client */
    CAM_MSG_SERVER_HELLO = 16,
    CAM_MSG_MODE = 17,
    CAM_MSG_BUFFER = 18,
    CAM_MSG_FRAME = 19,
    CAM_MSG_PARAMS = 20,
    CAM_MSG_PENDING = 21,
    CAM_MSG_PAUSED = 22,
    CAM_MSG_RESUMED = 23,
    CAM_MSG_PHOTO_DONE = 24,
    CAM_MSG_ERROR = 25,
};

enum {
    CAM_ERR_DENIED = 1,       /* the user said no */
    CAM_ERR_REVOKED = 2,      /* the permission was taken back while streaming: stopped */
    CAM_ERR_BUSY = 3,         /* someone else has the camera */
    CAM_ERR_UNAVAILABLE = 4,  /* no camera (not fitted, or it can't stream) */
    CAM_ERR_SUSPENDED = 5,    /* the cart isn't in front */
    CAM_ERR_BAD_REQUEST = 6,  /* a message out of place (PHOTO without a stream, ...) */
    CAM_ERR_FAILED = 7,       /* something else went wrong (the message says what) */
};

enum { CAM_PAUSE_PHOTO = 1, CAM_PAUSE_SUSPENDED = 2, CAM_PAUSE_TAKEN = 3 };

struct cam_hello { uint32_t type, version; };                  /* HELLO and SERVER_HELLO */
struct cam_open { uint32_t type, width, height, full_fov; };   /* the size wanted; the mode is the server's */
struct cam_release { uint32_t type, index; };
struct cam_close { uint32_t type; };
struct cam_photo { uint32_t type, request; };

struct cam_mode {
    uint32_t type;
    uint32_t width, height, stride, offset;  /* stride % 64 == 0; offset 0 */
    uint32_t reserved;
    uint64_t modifier;                       /* DRM_FORMAT_MOD_LINEAR (0) */
    uint32_t fps, nbuf;
    uint32_t turn;                           /* 0 or 90, clockwise from landscape screen-up */
    char order[4];                           /* "RGGB", "BGGR", ... (after the sensor's flips) */
    uint32_t black_level;                    /* 16 */
    uint32_t line_time_ns;
    float fx, fy, cx, cy, k1, k2, p1, p2, k3;  /* raw frame pixels, before turn */
    uint32_t intrinsics_measured;            /* 0: the module's typical values */
};
struct cam_buffer { uint32_t type, index, size; };   /* + one fd */
struct cam_frame {
    uint32_t type, index, sequence, params;          /* params: the PARAMS id for this frame */
    uint64_t timestamp_ns;                           /* CLOCK_MONOTONIC, end of exposure (V4L2) */
    uint32_t exposure_us;
    float gain, wb_red, wb_blue;                     /* analogue gain; white balance (green = 1) */
    float colour_temperature;                        /* K */
    uint32_t reserved;
};
struct cam_params {
    uint32_t type, id;
    float ccm[9];                 /* row-major, white-balanced linear RGB -> linear sRGB */
    float shading[12][16][3];     /* red gain, luminance gain, blue gain */
    float tone[1024];             /* tone[i]: display value for linear i / 1023 */
};
struct cam_pending { uint32_t type; };
struct cam_paused { uint32_t type, reason; };        /* CAM_PAUSE_* */
struct cam_resumed { uint32_t type; };
struct cam_photo_done { uint32_t type, request, size; };   /* + one fd: a memfd holding the JPEG */
struct cam_error { uint32_t type, code, request; char message[116]; };  /* request 0: not a PHOTO's */

#ifdef __cplusplus
#define CAM_STATIC_ASSERT static_assert
#else
#define CAM_STATIC_ASSERT _Static_assert
#endif
CAM_STATIC_ASSERT(sizeof(struct cam_hello) == 8, "cam_hello");
CAM_STATIC_ASSERT(sizeof(struct cam_open) == 16, "cam_open");
CAM_STATIC_ASSERT(sizeof(struct cam_release) == 8, "cam_release");
CAM_STATIC_ASSERT(sizeof(struct cam_close) == 4, "cam_close");
CAM_STATIC_ASSERT(sizeof(struct cam_photo) == 8, "cam_photo");
CAM_STATIC_ASSERT(sizeof(struct cam_mode) == 96, "cam_mode");
CAM_STATIC_ASSERT(sizeof(struct cam_buffer) == 12, "cam_buffer");
CAM_STATIC_ASSERT(sizeof(struct cam_frame) == 48, "cam_frame");
CAM_STATIC_ASSERT(sizeof(struct cam_params) == 6444, "cam_params");
CAM_STATIC_ASSERT(sizeof(struct cam_pending) == 4, "cam_pending");
CAM_STATIC_ASSERT(sizeof(struct cam_paused) == 8, "cam_paused");
CAM_STATIC_ASSERT(sizeof(struct cam_resumed) == 4, "cam_resumed");
CAM_STATIC_ASSERT(sizeof(struct cam_photo_done) == 12, "cam_photo_done");
CAM_STATIC_ASSERT(sizeof(struct cam_error) == 128, "cam_error");

#endif
