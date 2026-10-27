/*
 * Camera frame source for libdroiddesk_v4l2.so: frames from the DroidDesk app's CameraBridge
 * (Camera2) through the abstract socket @droiddesk.camera and a shared-memory ring.
 * Protocol and memory layout: camera_bridge_protocol.h.
 *
 * start() only connects and sends "open" (it runs under the shim's lock, so it must not
 * block); the answer is awaited in next_frame(), which runs on the producer thread in
 * slices of at most 100 ms. When the app is not reachable, answers with an error (no
 * permission, camera busy, no camera) or does not answer within 35 s (the permission
 * dialog), the source falls back to the test pattern for this stream and logs why. A stream
 * that breaks later (app restarted) keeps the last picture and reconnects every 2 s.
 *
 * The app always sends tightly packed I420 at the size the shim asked for; NV12 and YUYV
 * are converted here while copying.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "camera_bridge_protocol.h"
#include "frame_source.h"

#define ANSWER_TIMEOUT_MS 35000 /* the app may be showing the permission dialog (30 s) */
#define RETRY_MS 2000
#define SLICE_MS 100

enum state { WAIT_OK, STREAM, LOST, FALLBACK };

struct bridge {
    struct dd_format f;
    int device;
    enum state state;
    int sock;
    char rbuf[4096];
    size_t rlen;
    int shm_fd;
    uint8_t *mem;
    size_t mem_size;
    struct timespec deadline; /* WAIT_OK: give up; LOST: next reconnect */
    int have_pending;
    uint32_t pending_slot;
    uint64_t pending_seq, last_seq;
    long period_ns;
    struct timespec last_out;
    int have_out;
    void *pattern;
    uint8_t *tmp; /* I420 copy when the V4L2 format is NV12 or YUYV */
    uint64_t frames, torn, skipped;
};

static long now_ms_diff(const struct timespec *a, const struct timespec *b) {
    return (long)(a->tv_sec - b->tv_sec) * 1000L + (a->tv_nsec - b->tv_nsec) / 1000000L;
}

static long ns_diff(const struct timespec *a, const struct timespec *b) {
    return (long)(a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

static void in_ms(struct timespec *t, long ms) {
    clock_gettime(CLOCK_MONOTONIC, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
}

static const char *env_or(const char *name, const char *def) {
    const char *v = getenv(name);
    return v && *v ? v : def;
}

static void unmap(struct bridge *b) {
    if (b->mem) munmap(b->mem, b->mem_size);
    if (b->shm_fd >= 0) close(b->shm_fd);
    b->mem = NULL;
    b->mem_size = 0;
    b->shm_fd = -1;
}

static void disconnect(struct bridge *b) {
    if (b->sock >= 0) {
        static const char bye[] = "close\n";
        ssize_t w = send(b->sock, bye, sizeof(bye) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        (void)w;
        close(b->sock);
    }
    b->sock = -1;
    b->rlen = 0;
    b->have_pending = 0;
    unmap(b);
}

static void fall_back(struct bridge *b, const char *why) {
    disconnect(b);
    dd_log("camera bridge: %s -> test pattern for this stream", why);
    b->pattern = dd_pattern_source.start(&b->f, b->device);
    b->state = FALLBACK;
}

/* Connects and sends "open". 0 = waiting for the answer, -1 = not reachable (errno). */
static int connect_and_open(struct bridge *b) {
    const char *name = env_or("DROIDDESK_CAMERA_SOCKET", DDCAM_SOCKET_NAME);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n > sizeof(a.sun_path) - 1) n = sizeof(a.sun_path) - 1;
    memcpy(a.sun_path + 1, name, n); /* abstract: leading NUL */
    if (connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    /* Without DROIDDESK_CAMERA_FACING the app picks the camera last chosen with
     * "droiddesk-camera front|back" (front at first). */
    const char *facing = getenv("DROIDDESK_CAMERA_FACING");
    char facing_field[16] = "";
    if (facing && *facing)
        snprintf(facing_field, sizeof(facing_field), " facing=%s", strcmp(facing, "back") == 0 ? "back" : "front");
    char line[256];
    int len = snprintf(line, sizeof(line),
                       "open v=%d width=%u height=%u fps=%u format=I420%s rotate=%d mirror=%d\n",
                       DDCAM_VERSION, b->f.width, b->f.height, b->f.fps ? b->f.fps : 30, facing_field,
                       atoi(env_or("DROIDDESK_CAMERA_ROTATE", "0")),
                       strcmp(env_or("DROIDDESK_CAMERA_MIRROR", "0"), "1") == 0);
    if (send(s, line, (size_t)len, MSG_NOSIGNAL) != len) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    b->sock = s;
    b->rlen = 0;
    b->state = WAIT_OK;
    in_ms(&b->deadline, ANSWER_TIMEOUT_MS);
    return 0;
}

/* "ok ..." with the shared memory fd: map it and check the layout. NULL = fine, else why not. */
static const char *map_ring(struct bridge *b, const char *line) {
    if (b->shm_fd < 0) return "answer without the shared memory fd";
    struct stat st;
    if (fstat(b->shm_fd, &st) < 0 || st.st_size < (off_t)sizeof(struct ddcam_header))
        return "shared memory too small";
    b->mem_size = (size_t)st.st_size;
    void *m = mmap(NULL, b->mem_size, PROT_READ, MAP_SHARED, b->shm_fd, 0);
    if (m == MAP_FAILED) {
        b->mem_size = 0;
        return "mmap of the shared memory failed";
    }
    b->mem = m;
    const struct ddcam_header *h = (const void *)b->mem;
    if (h->magic != DDCAM_MAGIC || h->version != DDCAM_VERSION) return "wrong shared memory magic/version";
    if (h->width != b->f.width || h->height != b->f.height || h->fourcc != V4L2_PIX_FMT_YUV420 ||
        h->frame_size != b->f.width * b->f.height * 3 / 2)
        return "app sends another size or format";
    if (!h->slots || h->slots > DDCAM_MAX_SLOTS ||
        (uint64_t)h->data_offset + (uint64_t)h->slots * h->slot_stride > b->mem_size ||
        h->slot_stride < h->frame_size)
        return "bad slot layout";
    dd_log("camera bridge: %s", line);
    return NULL;
}

static void handle_line(struct bridge *b, char *line) {
    if (strncmp(line, "frame ", 6) == 0) {
        unsigned slot;
        unsigned long long seq;
        if (b->state == STREAM && sscanf(line + 6, "%u %llu", &slot, &seq) == 2 &&
            slot < ((const struct ddcam_header *)(const void *)b->mem)->slots) {
            b->pending_slot = slot;
            b->pending_seq = seq;
            b->have_pending = 1;
        }
    } else if (strncmp(line, "ok", 2) == 0 && b->state == WAIT_OK) {
        const char *why = map_ring(b, line);
        if (why) fall_back(b, why);
        else {
            b->state = STREAM;
            b->last_seq = 0;
            b->have_out = 0;
        }
    } else if (strncmp(line, "err", 3) == 0 && b->state == WAIT_OK) {
        char why[300];
        snprintf(why, sizeof(why), "app answered \"%s\"", line);
        fall_back(b, why);
    } else if (strncmp(line, "state ", 6) == 0) {
        dd_log("camera bridge: %s", line);
    } else if (line[0]) {
        dd_log("camera bridge: unexpected \"%s\"", line);
    }
}

/* Reads what is there (fds included) and handles complete lines. -1 = connection closed. */
static int pump(struct bridge *b) {
    for (;;) {
        if (b->rlen >= sizeof(b->rbuf) - 1) b->rlen = 0; /* runaway line: drop it */
        struct iovec iov = {b->rbuf + b->rlen, sizeof(b->rbuf) - 1 - b->rlen};
        union {
            struct cmsghdr h;
            char buf[CMSG_SPACE(sizeof(int) * 4)];
        } cm;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cm.buf;
        msg.msg_controllen = sizeof(cm.buf);
        ssize_t n = recvmsg(b->sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fds = (int *)(void *)CMSG_DATA(c);
            for (int i = 0; i < count; i++) {
                if (b->shm_fd < 0 && b->state == WAIT_OK) b->shm_fd = fds[i];
                else close(fds[i]);
            }
        }
        if (n == 0) return -1;
        b->rlen += (size_t)n;
        char *start = b->rbuf, *nl;
        while (b->sock >= 0 && (nl = memchr(start, '\n', b->rlen - (size_t)(start - b->rbuf)))) {
            *nl = 0;
            handle_line(b, start);
            start = nl + 1;
        }
        if (b->sock < 0) return 0; /* fell back while handling */
        b->rlen -= (size_t)(start - b->rbuf);
        memmove(b->rbuf, start, b->rlen);
    }
}

static void *bridge_start(const struct dd_format *format, int device) {
    struct bridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->f = *format;
    b->device = device;
    b->sock = -1;
    b->shm_fd = -1;
    b->period_ns = 1000000000L / (format->fps ? format->fps : 30);
    if (format->fourcc != V4L2_PIX_FMT_YUV420 &&
        !(b->tmp = malloc((size_t)format->width * format->height * 3 / 2))) {
        free(b);
        return NULL;
    }
    if (connect_and_open(b) < 0) {
        char why[160];
        snprintf(why, sizeof(why), "@%s not reachable (%s; is DroidDesk running?)",
                 env_or("DROIDDESK_CAMERA_SOCKET", DDCAM_SOCKET_NAME), strerror(errno));
        fall_back(b, why);
        if (!b->pattern) {
            free(b->tmp);
            free(b);
            return NULL;
        }
    }
    return b;
}

static void i420_to(const struct dd_format *f, const uint8_t *src, uint8_t *dst) {
    uint32_t W = f->width, H = f->height;
    const uint8_t *Y = src, *U = src + (size_t)W * H, *V = U + (size_t)(W / 2) * (H / 2);
    if (f->fourcc == V4L2_PIX_FMT_NV12) {
        memcpy(dst, Y, (size_t)W * H);
        uint8_t *uv = dst + (size_t)W * H;
        for (size_t i = 0, n = (size_t)(W / 2) * (H / 2); i < n; i++) {
            uv[2 * i] = U[i];
            uv[2 * i + 1] = V[i];
        }
        return;
    }
    /* YUYV */
    for (uint32_t y = 0; y < H; y++) {
        const uint8_t *yr = Y + (size_t)y * W, *ur = U + (size_t)(y / 2) * (W / 2), *vr = V + (size_t)(y / 2) * (W / 2);
        uint8_t *d = dst + (size_t)y * f->bytesperline;
        for (uint32_t x = 0; x < W; x += 2, d += 4) {
            d[0] = yr[x];
            d[1] = ur[x / 2];
            d[2] = yr[x + 1];
            d[3] = vr[x / 2];
        }
    }
}

/* Seqlock copy of the pending slot. 0 = copied, -1 = overwritten meanwhile. */
static int copy_slot(struct bridge *b, uint8_t *out) {
    const struct ddcam_header *h = (const void *)b->mem;
    uint32_t s = b->pending_slot;
    uint64_t s1 = __atomic_load_n(&h->slot_seq[s], __ATOMIC_ACQUIRE);
    if (s1 == 0 || s1 != b->pending_seq) return -1;
    memcpy(out, b->mem + h->data_offset + (size_t)s * h->slot_stride, h->frame_size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t s2 = __atomic_load_n(&h->slot_seq[s], __ATOMIC_RELAXED);
    return s1 == s2 ? 0 : -1;
}

static int bridge_next_frame(void *ctx, uint8_t *dst, uint32_t sequence, struct timespec *timestamp) {
    struct bridge *b = ctx;
    if (b->state == FALLBACK) {
        if (b->pattern) return dd_pattern_source.next_frame(b->pattern, dst, sequence, timestamp);
        sleep_ms(SLICE_MS);
        return -1;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (b->state == LOST) {
        if (now_ms_diff(&now, &b->deadline) < 0) {
            sleep_ms(SLICE_MS);
            return -1;
        }
        if (connect_and_open(b) < 0) {
            b->state = LOST;
            in_ms(&b->deadline, RETRY_MS);
            sleep_ms(SLICE_MS);
            return -1;
        }
        dd_log("camera bridge: reconnected, waiting for the camera");
    }

    int fresh = b->have_pending && b->pending_seq > b->last_seq;
    struct pollfd p = {b->sock, POLLIN, 0};
    int r = poll(&p, 1, fresh ? 0 : SLICE_MS);
    if (r > 0 && pump(b) < 0) {
        if (b->state == WAIT_OK && b->last_seq == 0 && !b->frames) {
            fall_back(b, "app closed the connection without an answer");
        } else {
            dd_log("camera bridge: connection lost after %llu frames, reconnecting every %d s",
                   (unsigned long long)b->frames, RETRY_MS / 1000);
            disconnect(b);
            b->state = LOST;
            in_ms(&b->deadline, RETRY_MS);
        }
        return -1;
    }
    if (b->state == FALLBACK) return -1; /* fell back just now; the pattern starts next call */
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (b->state == WAIT_OK) {
        if (now_ms_diff(&now, &b->deadline) >= 0) fall_back(b, "no answer from the app within 35 s");
        return -1;
    }
    if (b->state != STREAM || !b->have_pending || b->pending_seq <= b->last_seq) return -1;

    /* The app paces to the asked fps; this only drops extra frames if it runs faster. */
    if (b->have_out && ns_diff(&now, &b->last_out) < b->period_ns * 3 / 4) {
        b->last_seq = b->pending_seq;
        b->skipped++;
        return -1;
    }
    b->last_seq = b->pending_seq;
    b->last_out = now;
    b->have_out = 1;
    *timestamp = now;
    if (!dst) return 1;
    if (copy_slot(b, b->tmp ? b->tmp : dst) < 0) {
        b->torn++;
        return -1;
    }
    if (b->tmp) i420_to(&b->f, b->tmp, dst);
    b->frames++;
    return 0;
}

static void bridge_stop(void *ctx) {
    struct bridge *b = ctx;
    if (!b) return;
    if (b->pattern) dd_pattern_source.stop(b->pattern);
    if (b->state != FALLBACK)
        dd_log("camera bridge: stop after %llu frames (%llu overwritten while copying, %llu above the fps)",
               (unsigned long long)b->frames, (unsigned long long)b->torn, (unsigned long long)b->skipped);
    disconnect(b);
    free(b->tmp);
    free(b);
}

const struct dd_frame_source dd_bridge_source = {
    "camera", bridge_start, bridge_next_frame, bridge_stop,
};
