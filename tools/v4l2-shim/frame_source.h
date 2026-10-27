/*
 * Frame source interface of libdroiddesk_v4l2.so.
 *
 * The V4L2 emulation (v4l2_shim.c) owns the fake device, the buffers and the ioctl state
 * machine. A frame source only produces pixels. Two sources: the Android camera bridge
 * (frame_source_bridge.c, copies the newest frame from the app's shared-memory ring, see
 * camera_bridge_protocol.h) and a moving test pattern (frame_source_pattern.c), which the
 * bridge source also falls back to when the app's camera is not available.
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
     * frame was written to dst, 1 when a frame was due but dst was NULL (dropped; the
     * sequence number counts on), -1 when no frame was due (the producer just calls again
     * with the same sequence number).
     */
    int (*next_frame)(void *ctx, uint8_t *dst, uint32_t sequence, struct timespec *timestamp);
    /* STREAMOFF or the last close. */
    void (*stop)(void *ctx);
};

extern const struct dd_frame_source dd_pattern_source;
extern const struct dd_frame_source dd_bridge_source;

/* The shim's log ($TMPDIR/droiddesk-v4l2.log), for the sources. */
__attribute__((visibility("hidden"), format(printf, 1, 2))) void dd_log(const char *fmt, ...);

#endif
