/*
 * Frame source interface of libdroiddesk_v4l2.so.
 *
 * The V4L2 emulation (v4l2_shim.c) owns the fake device, the buffers and the ioctl state
 * machine. A frame source only produces pixels. The prototype has one source, a moving
 * test pattern (frame_source_pattern.c). The Android camera bridge replaces it later with
 * a source that copies the newest frame from the app's shared-memory ring (memfd +
 * @droiddesk.camera socket) into dst.
 *
 * Threading: start/stop run on the application thread that issues STREAMON/STREAMOFF.
 * next_frame runs on the shim's producer thread, without the shim's lock held.
 */
#ifndef DROIDDESK_FRAME_SOURCE_H
#define DROIDDESK_FRAME_SOURCE_H

#include <stdint.h>
#include <time.h>

struct dd_format {
    uint32_t fourcc;       /* V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_NV12 or V4L2_PIX_FMT_YUYV */
    uint32_t width;
    uint32_t height;
    uint32_t bytesperline; /* of the first plane */
    uint32_t sizeimage;    /* bytes of one complete frame */
    uint32_t fps;          /* frames per second requested by VIDIOC_S_PARM */
};

struct dd_frame_source {
    const char *name;
    /* STREAMON: returns a context, or NULL on failure (STREAMON then fails with EIO). */
    void *(*start)(const struct dd_format *format, int device);
    /*
     * Waits until the next frame is due or available, then writes it into dst
     * (format->sizeimage bytes). dst == NULL means "no buffer queued, drop this frame"
     * (the source still paces itself). Fills *timestamp (CLOCK_MONOTONIC) of the frame.
     * Must return within about 100 ms, so STREAMOFF does not hang. Returns 0 when a
     * frame was produced, -1 when none was (the producer just calls again).
     */
    int (*next_frame)(void *ctx, uint8_t *dst, uint32_t sequence, struct timespec *timestamp);
    /* STREAMOFF or the last close. */
    void (*stop)(void *ctx);
};

extern const struct dd_frame_source dd_pattern_source;

#endif
