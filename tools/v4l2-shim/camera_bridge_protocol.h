/*
 * DroidDesk camera bridge, protocol version 1 (the app's CameraBridge <-> the shim's
 * frame_source_bridge.c, and the test server fake_camera_bridge.c).
 *
 * Transport: abstract Unix stream socket "droiddesk.camera" (env DROIDDESK_CAMERA_SOCKET
 * overrides the name). Text lines ending in '\n', key=value fields separated by spaces.
 *
 * Client -> server
 *   open v=1 width=W height=H fps=F format=I420 facing=front|back rotate=0|90|180|270 mirror=0|1
 *       Starts the camera. facing is optional (default: the camera last chosen with "facing",
 *       front at first). rotate is added to the automatic rotation (sensor orientation +
 *       display rotation), mirror flips the picture horizontally; both default to 0.
 *   close
 *       Stops the camera at once (closing the socket does the same).
 *   facing front|back    (on its own connection)
 *       Switches the running stream and makes that camera the default. Answer "ok" / "err".
 *   status               (on its own connection) -> "ok idle" or "ok streaming <fields>"
 *
 * Server -> client
 *   ok v=1 width=W height=H fps=F format=I420 slots=N slot_stride=S data_offset=O size=Z
 *      camera=<id> facing=<front|back> sensor=<w>x<h> rotation=<deg>
 *       Answer to open. The shared memory fd (memfd/ashmem, Z bytes) comes with this line
 *       (SCM_RIGHTS). It holds struct ddcam_header at offset 0 and N frames of tightly
 *       packed I420 (Y, then U, then V; width x height) at O + i * S.
 *   err <code> <text>    code: busy, no-permission, no-camera, bad-request, camera-error
 *   frame <slot> <seq> <timestamp_ns>
 *       A new frame is in <slot>. seq counts from 1 per stream; timestamp is the camera's
 *       (SENSOR_TIMESTAMP, informative only).
 *   state paused <reason> | state running
 *       The camera stopped delivering (taken by another app, app in background, error) and
 *       the server keeps trying to reopen it; frames resume after "state running".
 *
 * Shared memory: one writer (server), any number of readers. No acks: the server writes the
 * slots round-robin, so a reader has (N - 1) frame periods to copy a slot. Each slot has a
 * sequence word as a seqlock: the server stores 0 before writing the pixels and the frame's
 * seq after; a reader loads seq (acquire), copies, fences, loads seq again and discards the
 * copy if the two differ or are 0.
 */
#ifndef DROIDDESK_CAMERA_BRIDGE_PROTOCOL_H
#define DROIDDESK_CAMERA_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define DDCAM_SOCKET_NAME "droiddesk.camera"
#define DDCAM_VERSION 1
#define DDCAM_MAGIC 0x4D434444u /* "DDCM" little-endian */
#define DDCAM_MAX_SLOTS 8
#define DDCAM_DATA_OFFSET 4096u

struct ddcam_header {
    uint32_t magic;       /* DDCAM_MAGIC */
    uint32_t version;     /* DDCAM_VERSION */
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;      /* V4L2_PIX_FMT_YUV420 ('YU12') */
    uint32_t frame_size;  /* width * height * 3 / 2 */
    uint32_t slots;
    uint32_t slot_stride; /* page-aligned */
    uint32_t data_offset; /* DDCAM_DATA_OFFSET */
    uint32_t reserved[7];
    uint64_t slot_seq[DDCAM_MAX_SLOTS]; /* offset 64; 0 = empty or being written */
    int64_t slot_ts[DDCAM_MAX_SLOTS];   /* offset 128 */
};

_Static_assert(sizeof(struct ddcam_header) == 192, "ddcam_header layout");

#endif
