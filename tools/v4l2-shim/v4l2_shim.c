/*
 * libdroiddesk_v4l2.so — LD_PRELOAD shim that makes /dev/video0 (front camera) and /dev/video1
 * (back camera) V4L2 capture devices in user space, without a kernel node, root or v4l2loopback. The frames come from the
 * DroidDesk app's camera (frame_source_bridge.c, protocol in camera_bridge_protocol.h), or
 * from a built-in moving test pattern (frame_source_pattern.c) when the app's camera is not
 * available or DROIDDESK_V4L2_SOURCE=pattern. Both sit behind frame_source.h.
 *
 * Modelled on PipeWire's pipewire-v4l2 (MIT): the device fd is a real eventfd, so poll,
 * select, epoll, fcntl, dup and close work on it without hooks. The eventfd runs in
 * semaphore mode and its counter always equals the number of filled buffers waiting for
 * DQBUF, so POLLIN means "a frame is ready".
 *
 * Hooked: open/open64/openat/openat64 (+ fortify __open_2 variants), close, dup/dup2/dup3,
 * fcntl (F_DUPFD*), ioctl, mmap/mmap64, stat/lstat/fstat/fstatat, access/faccessat, and
 * opendir/readdir/rewinddir/closedir of /dev (the device names are added to the listing). Every
 * other path and fd goes straight to the next library (socket_hook or libc).
 *
 * Builds for bionic (Termux, NDK) and glibc (Linux), LP64 only.
 *
 * Environment:
 *   DROIDDESK_V4L2_LOG    log file (default $TMPDIR/droiddesk-v4l2.log, else /tmp/...)
 *   DROIDDESK_V4L2_DEBUG  1 = log every ioctl on the fake device, 0 = only the important ones
 *   DROIDDESK_V4L2_OFF    1 = pass everything through (the library stays loaded but inert)
 *   DROIDDESK_V4L2_SOURCE camera (default) or pattern
 *   DROIDDESK_CAMERA_FACING front or back for both devices (default: video0 front, video1
 *                         back); DROIDDESK_CAMERA_ROTATE extra degrees
 *                         (0/90/180/270); DROIDDESK_CAMERA_MIRROR 1 = flip horizontally;
 *                         DROIDDESK_CAMERA_SOCKET abstract socket name (tests)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "frame_source.h"

#ifndef __LP64__
#error "LP64 only (arm64 bionic, x86_64/aarch64 glibc)"
#endif

#define MAX_FD 4096
#define MAX_BUFS 8

/* One device per phone camera, so a browser's own camera picker switches between them. The
 * index is the minor number and goes to the frame source, which maps it to the facing. */
static const struct { const char *path, *card, *bus_info; } devices[] = {
    {"/dev/video0", "DroidDesk – přední kamera", "platform:droiddesk-camera-0"},
    {"/dev/video1", "DroidDesk – zadní kamera", "platform:droiddesk-camera-1"},
};
#define NDEVICES (int)(sizeof(devices) / sizeof(devices[0]))

#ifdef __BIONIC__
typedef int ioctl_req_t; /* bionic: int ioctl(int, int, ...) */
#else
typedef unsigned long ioctl_req_t; /* glibc: int ioctl(int, unsigned long, ...) */
#endif

/* ---- real functions ---------------------------------------------------------------- */

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_openat64)(int, const char *, int, ...);
static int (*real_close)(int);
static int (*real_dup)(int);
static int (*real_dup2)(int, int);
static int (*real_dup3)(int, int, int);
static int (*real_fcntl)(int, int, ...);
static int (*real_ioctl)(int, ioctl_req_t, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
static int (*real_stat)(const char *, struct stat *);
static int (*real_lstat)(const char *, struct stat *);
static int (*real_fstat)(int, struct stat *);
static int (*real_fstatat)(int, const char *, struct stat *, int);
static int (*real_access)(const char *, int);
static int (*real_faccessat)(int, const char *, int, int);
static DIR *(*real_opendir)(const char *);
static struct dirent *(*real_readdir)(DIR *);
static void (*real_rewinddir)(DIR *);
static int (*real_closedir)(DIR *);

static int disabled;
static int debug;
static int log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

#define RESOLVE(name) real_##name = dlsym(RTLD_NEXT, #name)

static void resolve(void) {
    RESOLVE(open); RESOLVE(open64); RESOLVE(openat); RESOLVE(openat64);
    RESOLVE(close); RESOLVE(dup); RESOLVE(dup2); RESOLVE(dup3); RESOLVE(fcntl);
    RESOLVE(ioctl); RESOLVE(mmap); RESOLVE(mmap64);
    RESOLVE(stat); RESOLVE(lstat); RESOLVE(fstat); RESOLVE(fstatat);
    RESOLVE(access); RESOLVE(faccessat);
    RESOLVE(opendir); RESOLVE(readdir); RESOLVE(rewinddir); RESOLVE(closedir);
    if (!real_open64) real_open64 = real_open;
    if (!real_openat64) real_openat64 = real_openat;
    if (!real_mmap64) real_mmap64 = (void *(*)(void *, size_t, int, int, int, off64_t))real_mmap;
}

/* ---- logging ---------------------------------------------------------------------- */

static void open_log(void) {
    char path[512];
    const char *p = getenv("DROIDDESK_V4L2_LOG");
    if (!p || !*p) {
        const char *tmp = getenv("TMPDIR");
        snprintf(path, sizeof(path), "%s/droiddesk-v4l2.log", tmp && *tmp ? tmp : "/tmp");
        p = path;
    }
    log_fd = (int)syscall(SYS_openat, AT_FDCWD, p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

__attribute__((format(printf, 1, 2))) static void logmsg(const char *fmt, ...) {
    if (log_fd < 0) return;
    char buf[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03ld [%d/%d] ", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, ts.tv_nsec / 1000000, (int)getpid(), (int)syscall(SYS_gettid));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    pthread_mutex_lock(&log_lock);
    ssize_t w = write(log_fd, buf, (size_t)n);
    (void)w;
    pthread_mutex_unlock(&log_lock);
}

void dd_log(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logmsg("%s", buf);
}

/* ---- device state ----------------------------------------------------------------- */

enum { BUF_DEQUEUED, BUF_QUEUED, BUF_FILLING, BUF_DONE };

struct formatdesc { uint32_t fourcc; const char *name; };
static const struct formatdesc formats[] = {
    {V4L2_PIX_FMT_YUV420, "Planar YUV 4:2:0 (I420)"},
    {V4L2_PIX_FMT_NV12, "Y/UV 4:2:0 (NV12)"},
    {V4L2_PIX_FMT_YUYV, "YUYV 4:2:2"},
};
#define NFORMATS (sizeof(formats) / sizeof(formats[0]))
static const struct { uint32_t w, h; } sizes[] = {{1280, 720}, {640, 480}};
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))
static const uint32_t intervals[] = {30, 15}; /* fps, discrete */
#define NINTERVALS (sizeof(intervals) / sizeof(intervals[0]))

struct cam {
    int device;        /* index into devices[] */
    int refs;          /* fds (dups) pointing at this open file */
    int notify_fd;     /* internal dup of the eventfd, survives the app closing its fd */
    struct dd_format fmt;
    /* buffers: one memfd, buffer i at offset i * stride */
    int memfd;
    uint8_t *mem;
    size_t stride;
    unsigned nbufs;
    struct {
        int state;
        uint32_t bytesused, sequence;
        struct timeval ts;
    } buf[MAX_BUFS];
    unsigned queued[MAX_BUFS], nqueued; /* FIFO of buffers for the producer */
    unsigned done[MAX_BUFS], ndone;     /* FIFO of filled buffers for DQBUF */
    /* streaming */
    int streaming, stop, thread_valid;
    int stopping;      /* a stream_off is joining the producer (lock dropped) */
    pthread_t thread;
    pthread_cond_t cond;
    const struct dd_frame_source *source;
    void *source_ctx;
    uint32_t sequence;
    uint64_t produced, dropped, dequeued;
    struct timespec stat_since;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct cam *fdmap[MAX_FD];

static struct cam *lookup(int fd) {
    if (fd < 0 || fd >= MAX_FD) return NULL;
    return __atomic_load_n(&fdmap[fd], __ATOMIC_ACQUIRE);
}

static void set_fd(int fd, struct cam *c) {
    __atomic_store_n(&fdmap[fd], c, __ATOMIC_RELEASE);
}

/* Index of the fake device at path, or -1. */
static int device_index(const char *path) {
    if (disabled || !path) return -1;
    for (int i = 0; i < NDEVICES; i++)
        if (strcmp(path, devices[i].path) == 0) return i;
    return -1;
}

static void compute_format(struct dd_format *f, uint32_t fourcc, uint32_t w, uint32_t h) {
    f->fourcc = fourcc;
    f->width = w;
    f->height = h;
    if (fourcc == V4L2_PIX_FMT_YUYV) {
        f->bytesperline = w * 2;
        f->sizeimage = w * h * 2;
    } else {
        f->bytesperline = w;
        f->sizeimage = w * h * 3 / 2;
    }
}

static int format_supported(uint32_t fourcc) {
    for (unsigned i = 0; i < NFORMATS; i++)
        if (formats[i].fourcc == fourcc) return 1;
    return 0;
}

static void nearest_size(uint32_t *w, uint32_t *h) {
    unsigned best = 0;
    uint64_t best_d = UINT64_MAX;
    for (unsigned i = 0; i < NSIZES; i++) {
        int64_t dw = (int64_t)sizes[i].w - *w, dh = (int64_t)sizes[i].h - *h;
        uint64_t d = (uint64_t)(dw * dw + dh * dh);
        if (d < best_d) { best_d = d; best = i; }
    }
    *w = sizes[best].w;
    *h = sizes[best].h;
}

static const char *fourcc_str(uint32_t f, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = c >= 32 && c < 127 ? c : '?';
    }
    out[4] = 0;
    return out;
}

/* ---- buffers ---------------------------------------------------------------------- */

static int make_memfd(size_t size) {
    int fd = -1;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "droiddesk-v4l2", 1u /* MFD_CLOEXEC */);
#endif
    if (fd < 0) { /* fallback: unlinked temp file */
        const char *tmp = getenv("TMPDIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/.droiddesk-v4l2-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        fd = mkostemp(path, O_CLOEXEC);
        if (fd >= 0) unlink(path);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        real_close(fd);
        return -1;
    }
    return fd;
}

static void free_buffers(struct cam *c) {
    if (c->mem) munmap(c->mem, c->stride * c->nbufs);
    if (c->memfd >= 0) real_close(c->memfd);
    c->mem = NULL;
    c->memfd = -1;
    c->nbufs = 0;
    c->nqueued = c->ndone = 0;
}

static int alloc_buffers(struct cam *c, unsigned n) {
    long page = sysconf(_SC_PAGESIZE);
    size_t stride = ((size_t)c->fmt.sizeimage + (size_t)page - 1) & ~((size_t)page - 1);
    int fd = make_memfd(stride * n);
    if (fd < 0) return -1;
    void *mem = real_mmap(NULL, stride * n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        real_close(fd);
        return -1;
    }
    c->memfd = fd;
    c->mem = mem;
    c->stride = stride;
    c->nbufs = n;
    c->nqueued = c->ndone = 0;
    for (unsigned i = 0; i < n; i++) memset(&c->buf[i], 0, sizeof(c->buf[i]));
    return 0;
}

/* ---- producer thread ---------------------------------------------------------------- */

static void notify_one(struct cam *c) {
    uint64_t one = 1;
    ssize_t r = write(c->notify_fd, &one, sizeof(one));
    (void)r;
}

static void consume_one(struct cam *c) {
    uint64_t v;
    ssize_t r = read(c->notify_fd, &v, sizeof(v)); /* semaphore mode: decrements by 1 */
    (void)r;
}

static void *producer(void *arg) {
    struct cam *c = arg;
    pthread_mutex_lock(&lock);
    while (!c->stop) {
        int idx = -1;
        if (c->nqueued) {
            idx = (int)c->queued[0];
            memmove(c->queued, c->queued + 1, --c->nqueued * sizeof(c->queued[0]));
            c->buf[idx].state = BUF_FILLING;
        }
        uint32_t seq = c->sequence;
        uint8_t *dst = idx >= 0 ? c->mem + (size_t)idx * c->stride : NULL;
        const struct dd_frame_source *src = c->source;
        void *ctx = c->source_ctx;
        pthread_mutex_unlock(&lock);

        struct timespec ts = {0, 0};
        int r = src->next_frame(ctx, dst, seq, &ts);
        int ok = r == 0;

        pthread_mutex_lock(&lock);
        if (r >= 0 && c->sequence == seq) c->sequence++; /* a frame was due (sent or dropped) */
        if (idx < 0) {
            if (r >= 0) c->dropped++;
        } else if (c->buf[idx].state == BUF_FILLING) {
            if (ok && !c->stop) {
                c->buf[idx].state = BUF_DONE;
                c->buf[idx].bytesused = c->fmt.sizeimage;
                c->buf[idx].sequence = seq;
                c->buf[idx].ts.tv_sec = ts.tv_sec;
                c->buf[idx].ts.tv_usec = ts.tv_nsec / 1000;
                c->done[c->ndone++] = (unsigned)idx;
                c->produced++;
                notify_one(c);
                pthread_cond_broadcast(&c->cond);
            } else { /* no frame: give the buffer back to the queue */
                c->buf[idx].state = BUF_QUEUED;
                memmove(c->queued + 1, c->queued, c->nqueued++ * sizeof(c->queued[0]));
                c->queued[0] = (unsigned)idx;
            }
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (double)(now.tv_sec - c->stat_since.tv_sec) +
                      (double)(now.tv_nsec - c->stat_since.tv_nsec) / 1e9;
        if (secs >= 10.0) {
            logmsg("stats: %.1f s, %llu frames filled (%.1f fps), %llu dequeued, %llu dropped "
                   "(no buffer queued)", secs, (unsigned long long)c->produced,
                   (double)c->produced / secs, (unsigned long long)c->dequeued,
                   (unsigned long long)c->dropped);
            c->produced = c->dropped = c->dequeued = 0;
            c->stat_since = now;
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* Called with lock held; drops it while joining the producer. A second caller (STREAMOFF and
 * the last close on two threads) waits until the first one has finished the teardown. */
static void stream_off(struct cam *c) {
    while (c->stopping) pthread_cond_wait(&c->cond, &lock);
    if (!c->streaming) return;
    c->stop = 1;
    pthread_cond_broadcast(&c->cond);
    if (c->thread_valid) {
        pthread_t t = c->thread;
        c->thread_valid = 0;
        c->stopping = 1;
        pthread_mutex_unlock(&lock);
        pthread_join(t, NULL);
        pthread_mutex_lock(&lock);
        c->stopping = 0;
        pthread_cond_broadcast(&c->cond);
    }
    if (c->source && c->source_ctx) c->source->stop(c->source_ctx);
    c->source_ctx = NULL;
    while (c->ndone) { /* reset the eventfd counter to 0 */
        consume_one(c);
        c->ndone--;
    }
    c->nqueued = 0;
    for (unsigned i = 0; i < c->nbufs; i++) c->buf[i].state = BUF_DEQUEUED;
    c->streaming = 0;
    logmsg("STREAMOFF");
}

static int stream_on(struct cam *c) {
    if (c->streaming) return 0;
    if (!c->nbufs) return EINVAL;
    /* The bridge source falls back to the pattern itself (without blocking here, under the
     * lock) when the app's camera is not there. */
    const char *want = getenv("DROIDDESK_V4L2_SOURCE");
    c->source = want && strcmp(want, "pattern") == 0 ? &dd_pattern_source : &dd_bridge_source;
    c->source_ctx = c->source->start(&c->fmt, c->device);
    if (!c->source_ctx) return EIO;
    c->stop = 0;
    c->sequence = 0;
    c->produced = c->dropped = c->dequeued = 0;
    clock_gettime(CLOCK_MONOTONIC, &c->stat_since);
    if (pthread_create(&c->thread, NULL, producer, c) != 0) {
        c->source->stop(c->source_ctx);
        c->source_ctx = NULL;
        return EIO;
    }
    c->thread_valid = 1;
    c->streaming = 1;
    char fc[5];
    logmsg("STREAMON %s %ux%u @%u fps, %u buffers of %u bytes, source %s",
           fourcc_str(c->fmt.fourcc, fc), c->fmt.width, c->fmt.height, c->fmt.fps, c->nbufs,
           c->fmt.sizeimage, c->source->name);
    return 0;
}

/* ---- open / close / dup ------------------------------------------------------------- */

static int cam_open(int device, int flags) {
    int efd_flags = EFD_SEMAPHORE;
    if (flags & O_NONBLOCK) efd_flags |= EFD_NONBLOCK;
    if (flags & O_CLOEXEC) efd_flags |= EFD_CLOEXEC;
    int fd = eventfd(0, efd_flags);
    if (fd < 0) return -1;
    if (fd >= MAX_FD) {
        real_close(fd);
        errno = EMFILE;
        return -1;
    }
    struct cam *c = calloc(1, sizeof(*c));
    int nfd = real_fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (!c || nfd < 0) {
        free(c);
        if (nfd >= 0) real_close(nfd);
        real_close(fd);
        errno = ENOMEM;
        return -1;
    }
    c->device = device;
    c->refs = 1;
    c->notify_fd = nfd;
    c->memfd = -1;
    pthread_cond_init(&c->cond, NULL);
    compute_format(&c->fmt, V4L2_PIX_FMT_YUV420, sizes[0].w, sizes[0].h);
    c->fmt.fps = intervals[0];
    pthread_mutex_lock(&lock);
    set_fd(fd, c);
    pthread_mutex_unlock(&lock);
    logmsg("open %s flags=0x%x -> fd %d", devices[device].path, flags, fd);
    return fd;
}

/* lock held */
static void release_fd(int fd) {
    struct cam *c = lookup(fd);
    if (!c) return;
    set_fd(fd, NULL);
    if (--c->refs > 0) {
        logmsg("close fd %d (%d more refs)", fd, c->refs);
        return;
    }
    stream_off(c);
    free_buffers(c);
    real_close(c->notify_fd);
    pthread_cond_destroy(&c->cond);
    free(c);
    logmsg("close fd %d (device released)", fd);
}

static void track_dup(int oldfd, int newfd) {
    if (newfd < 0) return;
    pthread_mutex_lock(&lock);
    struct cam *c = lookup(oldfd);
    if (c && newfd < MAX_FD) {
        c->refs++;
        set_fd(newfd, c);
        logmsg("dup fd %d -> %d", oldfd, newfd);
    }
    pthread_mutex_unlock(&lock);
}

static void prefork(void) {
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&log_lock);
}
static void postfork_parent(void) {
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&lock);
}
/* The child forgets the inherited camera fds (they become plain eventfds): cleaning them up
 * would send "close" over the bridge connection it shares with the parent and stop the
 * parent's camera. The objects leak in the child, which normally execs right away. */
static void postfork_child(void) {
    for (int fd = 0; fd < MAX_FD; fd++) fdmap[fd] = NULL;
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&lock);
}

__attribute__((constructor)) static void init(void) {
    resolve();
    const char *off = getenv("DROIDDESK_V4L2_OFF");
    disabled = off && *off == '1';
    const char *dbg = getenv("DROIDDESK_V4L2_DEBUG");
    debug = dbg && *dbg == '1';
    open_log();
    pthread_atfork(prefork, postfork_parent, postfork_child);
    char exe[256] = "?";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) exe[n] = 0;
    logmsg("loaded into %s%s", exe, disabled ? " (DROIDDESK_V4L2_OFF=1, inert)" : "");
}

/* ---- ioctl -------------------------------------------------------------------------- */

static const char *ioctl_name(unsigned int req) {
    switch (req) {
#define N(x) case (unsigned int)x: return #x;
        N(VIDIOC_QUERYCAP) N(VIDIOC_ENUM_FMT) N(VIDIOC_G_FMT) N(VIDIOC_S_FMT) N(VIDIOC_TRY_FMT)
        N(VIDIOC_REQBUFS) N(VIDIOC_QUERYBUF) N(VIDIOC_QBUF) N(VIDIOC_DQBUF) N(VIDIOC_STREAMON)
        N(VIDIOC_STREAMOFF) N(VIDIOC_G_PARM) N(VIDIOC_S_PARM) N(VIDIOC_ENUM_FRAMESIZES)
        N(VIDIOC_ENUM_FRAMEINTERVALS) N(VIDIOC_ENUMINPUT) N(VIDIOC_G_INPUT) N(VIDIOC_S_INPUT)
        N(VIDIOC_QUERYCTRL) N(VIDIOC_QUERY_EXT_CTRL) N(VIDIOC_G_CTRL) N(VIDIOC_S_CTRL)
        N(VIDIOC_G_EXT_CTRLS) N(VIDIOC_S_EXT_CTRLS) N(VIDIOC_G_STD) N(VIDIOC_S_STD)
        N(VIDIOC_ENUMSTD) N(VIDIOC_QUERYSTD) N(VIDIOC_CROPCAP) N(VIDIOC_G_SELECTION)
        N(VIDIOC_S_SELECTION) N(VIDIOC_SUBSCRIBE_EVENT) N(VIDIOC_DQEVENT) N(VIDIOC_EXPBUF)
        N(VIDIOC_CREATE_BUFS) N(VIDIOC_G_PRIORITY) N(VIDIOC_S_PRIORITY) N(VIDIOC_ENUMAUDIO)
        N(VIDIOC_G_AUDIO) N(VIDIOC_G_TUNER) N(VIDIOC_LOG_STATUS) N(VIDIOC_G_DV_TIMINGS)
        N(VIDIOC_QUERYMENU)
#undef N
    default: return NULL;
    }
}

static int do_querycap(const struct cam *c, struct v4l2_capability *cap) {
    memset(cap, 0, sizeof(*cap));
    snprintf((char *)cap->driver, sizeof(cap->driver), "droiddesk");
    snprintf((char *)cap->card, sizeof(cap->card), "%s", devices[c->device].card);
    snprintf((char *)cap->bus_info, sizeof(cap->bus_info), "%s", devices[c->device].bus_info);
    cap->version = (6u << 16) | (1u << 8);
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
}

static void fill_pix(struct v4l2_pix_format *pix, const struct dd_format *f) {
    memset(pix, 0, sizeof(*pix));
    pix->width = f->width;
    pix->height = f->height;
    pix->pixelformat = f->fourcc;
    pix->field = V4L2_FIELD_NONE;
    pix->bytesperline = f->bytesperline;
    pix->sizeimage = f->sizeimage;
    pix->colorspace = V4L2_COLORSPACE_SRGB;
}

/*
 * TRY_FMT/S_FMT. An unknown pixel format fails TRY_FMT with EINVAL: libwebrtc's
 * FillCapabilities records the format it ASKED for whenever TRY_FMT succeeds with the same
 * size, so substituting I420 (what the spec suggests) would make Firefox list MJPEG/RGB
 * modes we do not have. S_FMT substitutes I420, as drivers do. Sizes snap to the nearest.
 */
static int do_fmt(struct cam *c, struct v4l2_format *f, int set) {
    if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return EINVAL;
    uint32_t fourcc = f->fmt.pix.pixelformat;
    if (!format_supported(fourcc)) {
        if (!set) return EINVAL;
        fourcc = V4L2_PIX_FMT_YUV420;
    }
    uint32_t w = f->fmt.pix.width, h = f->fmt.pix.height;
    nearest_size(&w, &h);
    struct dd_format nf = c->fmt;
    compute_format(&nf, fourcc, w, h);
    if (set) {
        if (c->streaming || c->nbufs) {
            if (memcmp(&nf, &c->fmt, sizeof(nf)) != 0) return EBUSY;
        }
        c->fmt = nf;
        char fc[5];
        logmsg("S_FMT %s %ux%u", fourcc_str(fourcc, fc), w, h);
    }
    fill_pix(&f->fmt.pix, &nf);
    return 0;
}

static int do_ioctl(struct cam *c, unsigned int req, void *arg) {
    switch (req) {
    case (unsigned int)VIDIOC_QUERYCAP:
        return do_querycap(c, arg);

    case (unsigned int)VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *d = arg;
        if (d->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || d->index >= NFORMATS) return EINVAL;
        uint32_t index = d->index, type = d->type;
        memset(d, 0, sizeof(*d));
        d->index = index;
        d->type = type;
        d->pixelformat = formats[index].fourcc;
        snprintf((char *)d->description, sizeof(d->description), "%s", formats[index].name);
        return 0;
    }

    case (unsigned int)VIDIOC_G_FMT: {
        struct v4l2_format *f = arg;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return EINVAL;
        fill_pix(&f->fmt.pix, &c->fmt);
        return 0;
    }
    case (unsigned int)VIDIOC_S_FMT:
        return do_fmt(c, arg, 1);
    case (unsigned int)VIDIOC_TRY_FMT:
        return do_fmt(c, arg, 0);

    case (unsigned int)VIDIOC_ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum *s = arg;
        if (!format_supported(s->pixel_format) || s->index >= NSIZES) return EINVAL;
        s->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        s->discrete.width = sizes[s->index].w;
        s->discrete.height = sizes[s->index].h;
        return 0;
    }
    case (unsigned int)VIDIOC_ENUM_FRAMEINTERVALS: {
        struct v4l2_frmivalenum *iv = arg;
        int size_ok = 0;
        for (unsigned i = 0; i < NSIZES; i++)
            if (sizes[i].w == iv->width && sizes[i].h == iv->height) size_ok = 1;
        if (!format_supported(iv->pixel_format) || !size_ok || iv->index >= NINTERVALS)
            return EINVAL;
        iv->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        iv->discrete.numerator = 1;
        iv->discrete.denominator = intervals[iv->index];
        return 0;
    }

    case (unsigned int)VIDIOC_G_PARM:
    case (unsigned int)VIDIOC_S_PARM: {
        struct v4l2_streamparm *p = arg;
        if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return EINVAL;
        if (req == (unsigned int)VIDIOC_S_PARM) {
            struct v4l2_fract t = p->parm.capture.timeperframe;
            if (t.numerator && t.denominator) {
                uint32_t fps = (t.denominator + t.numerator / 2) / t.numerator;
                if (fps < 1) fps = 1;
                if (fps > intervals[0]) fps = intervals[0];
                if (fps != c->fmt.fps && c->streaming) return EBUSY;
                c->fmt.fps = fps;
                logmsg("S_PARM %u fps", fps);
            }
        }
        memset(&p->parm, 0, sizeof(p->parm));
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = 1;
        p->parm.capture.timeperframe.denominator = c->fmt.fps;
        return 0;
    }

    case (unsigned int)VIDIOC_ENUMINPUT: {
        struct v4l2_input *in = arg;
        if (in->index != 0) return EINVAL;
        memset(in, 0, sizeof(*in));
        snprintf((char *)in->name, sizeof(in->name), "Camera");
        in->type = V4L2_INPUT_TYPE_CAMERA;
        return 0;
    }
    case (unsigned int)VIDIOC_G_INPUT:
        *(int *)arg = 0;
        return 0;
    case (unsigned int)VIDIOC_S_INPUT:
        return *(int *)arg == 0 ? 0 : EINVAL;

    case (unsigned int)VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *rb = arg;
        if (rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || rb->memory != V4L2_MEMORY_MMAP) return EINVAL;
        if (c->streaming) return EBUSY;
        free_buffers(c);
        unsigned asked = rb->count, n = asked;
        if (n > MAX_BUFS) n = MAX_BUFS;
        if (n > 0 && n < 2) n = 2;
        if (n && alloc_buffers(c, n) < 0) return ENOMEM;
        rb->count = n;
#ifdef V4L2_BUF_CAP_SUPPORTS_MMAP
        rb->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
#endif
        logmsg("REQBUFS %u -> %u", asked, n);
        return 0;
    }

    case (unsigned int)VIDIOC_QUERYBUF:
    case (unsigned int)VIDIOC_QBUF:
    case (unsigned int)VIDIOC_DQBUF: {
        struct v4l2_buffer *b = arg;
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return EINVAL;
        if (req == (unsigned int)VIDIOC_DQBUF) {
            if (b->memory != V4L2_MEMORY_MMAP) return EINVAL;
            if (!c->streaming) return EINVAL;
            while (!c->ndone) {
                int fl = real_fcntl(c->notify_fd, F_GETFL);
                if (fl >= 0 && (fl & O_NONBLOCK)) return EAGAIN;
                pthread_cond_wait(&c->cond, &lock);
                if (!c->streaming || c->stop) return EINVAL;
            }
            unsigned idx = c->done[0];
            memmove(c->done, c->done + 1, --c->ndone * sizeof(c->done[0]));
            consume_one(c);
            c->buf[idx].state = BUF_DEQUEUED;
            c->dequeued++;
            b->index = idx;
        } else {
            if (b->index >= c->nbufs) return EINVAL;
            if (req == (unsigned int)VIDIOC_QBUF) {
                if (b->memory != V4L2_MEMORY_MMAP) return EINVAL;
                if (c->buf[b->index].state != BUF_DEQUEUED) return EINVAL;
                c->buf[b->index].state = BUF_QUEUED;
                c->queued[c->nqueued++] = b->index;
            }
        }
        unsigned i = b->index;
        int st = c->buf[i].state;
        b->memory = V4L2_MEMORY_MMAP;
        b->m.offset = (uint32_t)(i * c->stride);
        b->length = c->fmt.sizeimage;
        b->field = V4L2_FIELD_NONE;
        b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        if (st == BUF_QUEUED || st == BUF_FILLING) b->flags |= V4L2_BUF_FLAG_QUEUED;
        if (st == BUF_DONE) b->flags |= V4L2_BUF_FLAG_DONE;
        b->bytesused = c->buf[i].bytesused;
        b->sequence = c->buf[i].sequence;
        b->timestamp = c->buf[i].ts;
        return 0;
    }

    case (unsigned int)VIDIOC_STREAMON:
    case (unsigned int)VIDIOC_STREAMOFF: {
        if (*(int *)arg != V4L2_BUF_TYPE_VIDEO_CAPTURE) return EINVAL;
        if (req == (unsigned int)VIDIOC_STREAMON) return stream_on(c);
        stream_off(c);
        return 0;
    }

    case (unsigned int)VIDIOC_QUERYCTRL:
    case (unsigned int)VIDIOC_QUERY_EXT_CTRL:
    case (unsigned int)VIDIOC_G_CTRL:
    case (unsigned int)VIDIOC_S_CTRL:
    case (unsigned int)VIDIOC_G_EXT_CTRLS:
    case (unsigned int)VIDIOC_S_EXT_CTRLS:
    case (unsigned int)VIDIOC_QUERYMENU:
    case (unsigned int)VIDIOC_ENUMSTD:
    case (unsigned int)VIDIOC_ENUMAUDIO:
        return EINVAL; /* no controls, no standards, no audio */
    case (unsigned int)VIDIOC_G_STD:
    case (unsigned int)VIDIOC_QUERYSTD:
        return ENODATA;
    default:
        return ENOTTY;
    }
}

static int cam_ioctl(int fd, unsigned int req, void *arg) {
    pthread_mutex_lock(&lock);
    struct cam *c = lookup(fd);
    if (!c) {
        pthread_mutex_unlock(&lock);
        errno = EBADF;
        return -1;
    }
    int err = arg || req == (unsigned int)VIDIOC_LOG_STATUS ? do_ioctl(c, req, arg) : EFAULT;
    pthread_mutex_unlock(&lock);
    const char *name = ioctl_name(req);
    int hot = req == (unsigned int)VIDIOC_QBUF || req == (unsigned int)VIDIOC_DQBUF;
    /* EINVAL ends an enumeration (and TRY_FMT probing in libwebrtc): expected, not news. */
    int probe = err == EINVAL &&
                (req == (unsigned int)VIDIOC_TRY_FMT || req == (unsigned int)VIDIOC_ENUM_FMT ||
                 req == (unsigned int)VIDIOC_ENUM_FRAMESIZES || req == (unsigned int)VIDIOC_ENUM_FRAMEINTERVALS ||
                 req == (unsigned int)VIDIOC_ENUMINPUT || req == (unsigned int)VIDIOC_QUERYCTRL ||
                 req == (unsigned int)VIDIOC_QUERY_EXT_CTRL);
    if (debug ? !(hot && err == EAGAIN)
              : (!hot && !probe && (err || !name || req == (unsigned int)VIDIOC_QUERYCAP))) {
        if (name) logmsg("ioctl fd %d %s -> %s", fd, name, err ? strerror(err) : "ok");
        else logmsg("ioctl fd %d 0x%08x (unknown) -> %s", fd, req, err ? strerror(err) : "ok");
    }
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

int ioctl(int fd, ioctl_req_t request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (lookup(fd)) return cam_ioctl(fd, (unsigned int)request, arg);
    if (!real_ioctl) resolve();
    return real_ioctl(fd, request, arg);
}

/* ---- mmap ------------------------------------------------------------------------- */

static void *cam_mmap(int fd, void *addr, size_t len, int prot, int flags, off64_t off) {
    pthread_mutex_lock(&lock);
    struct cam *c = lookup(fd);
    void *r = MAP_FAILED;
    int err = EINVAL;
    if (c && c->nbufs && c->stride && off >= 0 && (size_t)off % c->stride == 0 &&
        (size_t)off / c->stride < c->nbufs && len <= c->stride) {
        r = real_mmap(addr, len, prot, flags, c->memfd, (off_t)off);
        err = r == MAP_FAILED ? errno : 0;
    }
    pthread_mutex_unlock(&lock);
    if (debug || r == MAP_FAILED)
        logmsg("mmap fd %d offset %lld len %zu -> %s", fd, (long long)off, len,
               r == MAP_FAILED ? strerror(err) : "ok");
    if (r == MAP_FAILED) errno = err;
    return r;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (fd >= 0 && lookup(fd)) return cam_mmap(fd, addr, len, prot, flags, off);
    if (!real_mmap) return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
    return real_mmap(addr, len, prot, flags, fd, off);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t off) {
    if (fd >= 0 && lookup(fd)) return cam_mmap(fd, addr, len, prot, flags, off);
    if (!real_mmap64) return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
    return real_mmap64(addr, len, prot, flags, fd, off);
}

/* ---- open family ---------------------------------------------------------------------- */

static mode_t va_mode(int flags, va_list ap) {
#ifdef O_TMPFILE
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) return (mode_t)va_arg(ap, int);
#else
    if (flags & O_CREAT) return (mode_t)va_arg(ap, int);
#endif
    return 0;
}

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_mode(flags, ap);
    va_end(ap);
    int dev = device_index(path);
    if (dev >= 0) return cam_open(dev, flags);
    if (!real_open) resolve();
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_mode(flags, ap);
    va_end(ap);
    int dev = device_index(path);
    if (dev >= 0) return cam_open(dev, flags);
    if (!real_open64) resolve();
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_mode(flags, ap);
    va_end(ap);
    int dev = device_index(path);
    if (dev >= 0) return cam_open(dev, flags); /* absolute: dirfd is ignored */
    if (!real_openat) resolve();
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_mode(flags, ap);
    va_end(ap);
    int dev = device_index(path);
    if (dev >= 0) return cam_open(dev, flags);
    if (!real_openat64) resolve();
    return real_openat64(dirfd, path, flags, mode);
}

/* _FORTIFY_SOURCE entry points (open without O_CREAT). */
int __open_2(const char *path, int flags) { return open(path, flags); }
int __openat_2(int dirfd, const char *path, int flags) { return openat(dirfd, path, flags); }
#ifndef __BIONIC__
int __open64_2(const char *path, int flags) { return open64(path, flags); }
int __openat64_2(int dirfd, const char *path, int flags) { return openat64(dirfd, path, flags); }
#endif

int close(int fd) {
    if (lookup(fd)) {
        pthread_mutex_lock(&lock);
        release_fd(fd);
        pthread_mutex_unlock(&lock);
    }
    if (!real_close) resolve();
    return real_close(fd);
}

int dup(int fd) {
    if (!real_dup) resolve();
    int r = real_dup(fd);
    if (r >= 0 && lookup(fd)) track_dup(fd, r);
    return r;
}

/* After a successful dup2/dup3: newfd no longer is what it was, and may now be the camera.
 * Failed calls leave the tracking alone; dup2(fd, fd) changes nothing. */
static int dup_onto(int oldfd, int newfd, int r) {
    if (r < 0 || oldfd == newfd) return r;
    if (lookup(newfd)) {
        pthread_mutex_lock(&lock);
        release_fd(newfd);
        pthread_mutex_unlock(&lock);
    }
    if (lookup(oldfd)) track_dup(oldfd, r);
    return r;
}

int dup2(int oldfd, int newfd) {
    if (!real_dup2) resolve();
    return dup_onto(oldfd, newfd, real_dup2(oldfd, newfd));
}

int dup3(int oldfd, int newfd, int flags) {
    if (!real_dup3) resolve();
    return dup_onto(oldfd, newfd, real_dup3(oldfd, newfd, flags));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_fcntl) resolve();
    int r = real_fcntl(fd, cmd, arg);
    if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && r >= 0 && lookup(fd)) track_dup(fd, r);
    return r;
}

#ifndef __BIONIC__
int fcntl64(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}
#endif

/* ---- stat / access ---------------------------------------------------------------- */

static void fake_stat(int device, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0660;
    st->st_rdev = makedev(81, device);
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_blksize = 4096;
    st->st_ino = 0xd201d35cu + (unsigned)device;
}

int stat(const char *path, struct stat *st) {
    int dev = device_index(path);
    if (dev >= 0) { fake_stat(dev, st); return 0; }
    if (!real_stat) resolve();
    return real_stat(path, st);
}

int lstat(const char *path, struct stat *st) {
    int dev = device_index(path);
    if (dev >= 0) { fake_stat(dev, st); return 0; }
    if (!real_lstat) resolve();
    return real_lstat(path, st);
}

int fstat(int fd, struct stat *st) {
    struct cam *c = lookup(fd);
    if (c) { fake_stat(c->device, st); return 0; }
    if (!real_fstat) resolve();
    return real_fstat(fd, st);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    int dev = device_index(path);
    if (dev >= 0) { fake_stat(dev, st); return 0; }
    struct cam *c = (flags & AT_EMPTY_PATH) && path && !*path ? lookup(dirfd) : NULL;
    if (c) { fake_stat(c->device, st); return 0; }
    if (!real_fstatat) resolve();
    return real_fstatat(dirfd, path, st, flags);
}

/* LP64 bionic and glibc (2.33+ exports these, v4l2-ctl calls stat64/fstat64): struct
 * stat64 has the same layout as struct stat. */
int stat64(const char *path, struct stat64 *st) { return stat(path, (struct stat *)st); }
int lstat64(const char *path, struct stat64 *st) { return lstat(path, (struct stat *)st); }
int fstat64(int fd, struct stat64 *st) { return fstat(fd, (struct stat *)st); }
int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) {
    return fstatat(dirfd, path, (struct stat *)st, flags);
}

int access(const char *path, int mode) {
    if (device_index(path) >= 0) return (mode & X_OK) ? (errno = EACCES, -1) : 0;
    if (!real_access) resolve();
    return real_access(path, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    if (device_index(path) >= 0) return (mode & X_OK) ? (errno = EACCES, -1) : 0;
    if (!real_faccessat) resolve();
    return real_faccessat(dirfd, path, mode, flags);
}

/* ---- /dev listing ------------------------------------------------------------------ */

/*
 * Chromium finds V4L2 devices by listing /dev for "video*" (FileEnumerator), Firefox by
 * opening /dev/video0..63. Where the phone has /dev/video0 and video1 nodes of its own
 * (the Fold 7), Chromium found the names and the shim took over the open; the Tab S10 FE
 * has only inaccessible video6..video242 and Chromium found no camera. A listing of /dev
 * gets our device names after the real entries when they are missing.
 */
#define MAX_DEVDIRS 16
static struct devdir {
    DIR *dir;
    int real_done;
    unsigned next;          /* next fake device to return */
    int seen[NDEVICES];     /* a real entry of that name exists */
    struct dirent ent;
} devdirs[MAX_DEVDIRS];
static pthread_mutex_t devdir_lock = PTHREAD_MUTEX_INITIALIZER;

static struct devdir *find_devdir(DIR *d) {
    for (int i = 0; i < MAX_DEVDIRS; i++)
        if (devdirs[i].dir == d) return &devdirs[i];
    return NULL;
}

static int is_dev_dir(const char *path) {
    return !disabled && path && (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0);
}

DIR *opendir(const char *path) {
    if (!real_opendir) resolve();
    DIR *d = real_opendir(path);
    if (d && is_dev_dir(path)) {
        pthread_mutex_lock(&devdir_lock);
        struct devdir *slot = find_devdir(NULL);
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->dir = d;
        }
        pthread_mutex_unlock(&devdir_lock);
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!real_readdir) resolve();
    pthread_mutex_lock(&devdir_lock);
    struct devdir *slot = find_devdir(d);
    pthread_mutex_unlock(&devdir_lock);
    if (!slot) return real_readdir(d);
    if (!slot->real_done) {
        int saved = errno;
        struct dirent *e = real_readdir(d);
        if (e) {
            for (int i = 0; i < NDEVICES; i++)
                if (strcmp(e->d_name, devices[i].path + 5) == 0) slot->seen[i] = 1;
            return e;
        }
        errno = saved;
        slot->real_done = 1;
    }
    while (slot->next < NDEVICES && slot->seen[slot->next]) slot->next++;
    if (slot->next >= NDEVICES) return NULL;
    unsigned i = slot->next++;
    memset(&slot->ent, 0, sizeof(slot->ent));
    slot->ent.d_ino = 0xd201d35cu + i;
    slot->ent.d_reclen = sizeof(slot->ent);
    slot->ent.d_type = DT_CHR;
    snprintf(slot->ent.d_name, sizeof(slot->ent.d_name), "%s", devices[i].path + 5);
    return &slot->ent;
}

/* LP64: struct dirent64 has the same layout. */
struct dirent64 *readdir64(DIR *d) { return (struct dirent64 *)readdir(d); }

void rewinddir(DIR *d) {
    if (!real_rewinddir) resolve();
    pthread_mutex_lock(&devdir_lock);
    struct devdir *slot = find_devdir(d);
    if (slot) {
        DIR *keep = slot->dir;
        memset(slot, 0, sizeof(*slot));
        slot->dir = keep;
    }
    pthread_mutex_unlock(&devdir_lock);
    real_rewinddir(d);
}

int closedir(DIR *d) {
    if (!real_closedir) resolve();
    pthread_mutex_lock(&devdir_lock);
    struct devdir *slot = find_devdir(d);
    if (slot) slot->dir = NULL;
    pthread_mutex_unlock(&devdir_lock);
    return real_closedir(d);
}
