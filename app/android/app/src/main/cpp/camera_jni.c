// CameraBridge (Kotlin) helpers: the shared-memory frame ring for the Linux side and the
// YUV_420_888 -> I420 conversion with rotation, centre crop, scaling and mirroring.
// Layout and protocol: tools/v4l2-shim/camera_bridge_protocol.h (struct copied below).
#include <android/sharedmem.h>
#include <errno.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DDCAM_MAGIC 0x4D434444u
#define DDCAM_VERSION 1
#define DDCAM_MAX_SLOTS 8
#define V4L2_PIX_FMT_YUV420 0x32315559u // 'YU12'

struct ddcam_header {
    uint32_t magic, version, width, height, fourcc, frame_size, slots, slot_stride, data_offset;
    uint32_t reserved[7];
    uint64_t slot_seq[DDCAM_MAX_SLOTS];
    int64_t slot_ts[DDCAM_MAX_SLOTS];
};
_Static_assert(sizeof(struct ddcam_header) == 192, "same layout as camera_bridge_protocol.h");

// ASharedMemory (ashmem or memfd, NDK API 26). Returns the fd or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeCreate(JNIEnv *env, jclass cls, jint size) {
    (void) env;
    (void) cls;
    int fd = ASharedMemory_create("droiddesk-camera", (size_t) size);
    return fd < 0 ? -EIO : fd;
}

// Maps the region read-write for the app, then lowers the region's protection to read-only,
// so the Linux side can only map it for reading. Returns the address or 0.
JNIEXPORT jlong JNICALL
Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeMap(JNIEnv *env, jclass cls, jint fd, jint size) {
    (void) env;
    (void) cls;
    void *m = mmap(NULL, (size_t) size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return 0;
    memset(m, 0, sizeof(struct ddcam_header));
    ASharedMemory_setProt(fd, PROT_READ);
    return (jlong) (uintptr_t) m;
}

JNIEXPORT void JNICALL
Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeUnmap(JNIEnv *env, jclass cls, jlong address, jint size) {
    (void) env;
    (void) cls;
    if (address) munmap((void *) (uintptr_t) address, (size_t) size);
}

JNIEXPORT void JNICALL
Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeInitHeader(JNIEnv *env, jclass cls, jlong address,
                                                                  jint width, jint height, jint slots,
                                                                  jint slot_stride, jint data_offset) {
    (void) env;
    (void) cls;
    struct ddcam_header *h = (struct ddcam_header *) (uintptr_t) address;
    h->version = DDCAM_VERSION;
    h->width = (uint32_t) width;
    h->height = (uint32_t) height;
    h->fourcc = V4L2_PIX_FMT_YUV420;
    h->frame_size = (uint32_t) width * (uint32_t) height * 3 / 2;
    h->slots = (uint32_t) slots;
    h->slot_stride = (uint32_t) slot_stride;
    h->data_offset = (uint32_t) data_offset;
    for (int i = 0; i < DDCAM_MAX_SLOTS; i++) h->slot_seq[i] = 0;
    __atomic_store_n(&h->magic, DDCAM_MAGIC, __ATOMIC_RELEASE);
}

// Bilinear sample at 16.16 fixed-point (fx, fy), clamped to the plane.
static inline uint8_t sample(const uint8_t *src, int rs, int ps, int sw, int sh, int64_t fx, int64_t fy) {
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > ((int64_t) (sw - 1) << 16)) fx = (int64_t) (sw - 1) << 16;
    if (fy > ((int64_t) (sh - 1) << 16)) fy = (int64_t) (sh - 1) << 16;
    int x0 = (int) (fx >> 16), y0 = (int) (fy >> 16);
    int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
    int ax = (int) ((fx >> 8) & 0xff), ay = (int) ((fy >> 8) & 0xff);
    const uint8_t *r0 = src + (size_t) y0 * rs, *r1 = src + (size_t) y1 * rs;
    int top = r0[x0 * ps] * (256 - ax) + r0[x1 * ps] * ax;
    int bottom = r1[x0 * ps] * (256 - ax) + r1[x1 * ps] * ax;
    return (uint8_t) ((top * (256 - ay) + bottom * ay + 32768) >> 16);
}

// One plane: source sw x sh (row stride rs, pixel stride ps) -> destination dw x dh, tightly
// packed. The source is rotated clockwise by rot degrees, centre-cropped to the destination's
// aspect ratio, scaled and, with mirror, flipped horizontally.
static void map_plane(const uint8_t *src, int rs, int ps, int sw, int sh, uint8_t *dst, int dw, int dh,
                      int rot, int mirror) {
    if (rot == 0 && !mirror && sw == dw && sh == dh) {
        for (int y = 0; y < dh; y++) {
            const uint8_t *s = src + (size_t) y * rs;
            uint8_t *d = dst + (size_t) y * dw;
            if (ps == 1) memcpy(d, s, (size_t) dw);
            else for (int x = 0; x < dw; x++) d[x] = s[x * ps];
        }
        return;
    }
    int64_t rw = rot % 180 ? sh : sw, rh = rot % 180 ? sw : sh; // rotated size
    int64_t cw, ch;                                             // crop in rotated space
    if (rw * dh > rh * dw) {
        ch = rh;
        cw = rh * dw / dh;
    } else {
        cw = rw;
        ch = rw * dh / dw;
    }
    // rotated coordinate of destination pixel centre x: rx0 + x * step (16.16)
    int64_t xstep = (cw << 16) / dw, ystep = (ch << 16) / dh;
    int64_t rx0 = (((rw - cw) << 16) / 2) + xstep / 2 - (1 << 15);
    int64_t ry0 = (((rh - ch) << 16) / 2) + ystep / 2 - (1 << 15);
    if (mirror) {
        rx0 = ((rw - 1) << 16) - rx0;
        xstep = -xstep;
    }
    int64_t W1 = (int64_t) (sw - 1) << 16, H1 = (int64_t) (sh - 1) << 16;
    for (int y = 0; y < dh; y++) {
        int64_t ry = ry0 + y * ystep;
        int64_t sx, sy, dx, dy; // source position of x = 0 and its step per x
        switch (rot) {
            case 90: sx = ry; sy = H1 - rx0; dx = 0; dy = -xstep; break;
            case 180: sx = W1 - rx0; sy = H1 - ry; dx = -xstep; dy = 0; break;
            case 270: sx = W1 - ry; sy = rx0; dx = 0; dy = xstep; break;
            default: sx = rx0; sy = ry; dx = xstep; dy = 0; break;
        }
        uint8_t *d = dst + (size_t) y * dw;
        for (int x = 0; x < dw; x++, sx += dx, sy += dy) d[x] = sample(src, rs, ps, sw, sh, sx, sy);
    }
}

static int plane_ok(JNIEnv *env, jobject buf, int rs, int ps, int w, int h, const uint8_t **out) {
    *out = (*env)->GetDirectBufferAddress(env, buf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, buf);
    return *out && rs > 0 && ps > 0 && (int64_t) rs * (h - 1) + (int64_t) ps * (w - 1) < cap;
}

// Writes one camera image into slot `slot` of the ring as I420 width x height of the header,
// under the slot's seqlock. Returns 0 or -errno.
JNIEXPORT jint JNICALL
Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeWriteFrame(
        JNIEnv *env, jclass cls, jlong address, jint slot, jlong seq, jlong timestamp, jobject ybuf, jobject ubuf,
        jobject vbuf, jint y_row_stride, jint uv_row_stride, jint uv_pixel_stride, jint width, jint height,
        jint rotation, jboolean mirror) {
    (void) cls;
    struct ddcam_header *h = (struct ddcam_header *) (uintptr_t) address;
    if (!h || h->magic != DDCAM_MAGIC || slot < 0 || (uint32_t) slot >= h->slots || seq <= 0) return -EINVAL;
    if (width < 2 || height < 2 || (width | height) & 1) return -EINVAL;
    int rot = ((rotation % 360) + 360) % 360;
    if (rot % 90) return -EINVAL;
    const uint8_t *Y, *U, *V;
    if (!plane_ok(env, ybuf, y_row_stride, 1, width, height, &Y) ||
        !plane_ok(env, ubuf, uv_row_stride, uv_pixel_stride, width / 2, height / 2, &U) ||
        !plane_ok(env, vbuf, uv_row_stride, uv_pixel_stride, width / 2, height / 2, &V))
        return -EFAULT;
    int dw = (int) h->width, dh = (int) h->height;
    uint8_t *dst = (uint8_t *) h + h->data_offset + (size_t) slot * h->slot_stride;

    __atomic_store_n(&h->slot_seq[slot], 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    map_plane(Y, y_row_stride, 1, width, height, dst, dw, dh, rot, mirror);
    uint8_t *du = dst + (size_t) dw * dh, *dv = du + (size_t) (dw / 2) * (dh / 2);
    map_plane(U, uv_row_stride, uv_pixel_stride, width / 2, height / 2, du, dw / 2, dh / 2, rot, mirror);
    map_plane(V, uv_row_stride, uv_pixel_stride, width / 2, height / 2, dv, dw / 2, dh / 2, rot, mirror);
    h->slot_ts[slot] = timestamp;
    __atomic_store_n(&h->slot_seq[slot], (uint64_t) seq, __ATOMIC_RELEASE);
    return 0;
}
