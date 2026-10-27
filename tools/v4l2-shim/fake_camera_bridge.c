/*
 * fake_camera_bridge — test server for the shim's camera source: speaks the CameraBridge
 * protocol (camera_bridge_protocol.h) on the abstract socket like the DroidDesk app, but the
 * frames are the moving test pattern (frame_source_pattern.c) with the bridge's frame seq in
 * the machine-readable counter (rows 0-1). Lets test-bionic.sh / test-glibc.sh exercise
 * frame_source_bridge.c without Android.
 *
 *   fake_camera_bridge [socket-name]          default droiddesk.camera
 *
 * One stream at a time; a second "open" gets "err busy". FAKE_CAMERA_ANSWER=<code> makes
 * every open fail with "err <code> test" (fallback test). Builds with the NDK and gcc.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "camera_bridge_protocol.h"
#include "frame_source.h"

#define SLOTS 3

static volatile int busy;

void dd_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int say(int fd, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return send(fd, line, (size_t)n, MSG_NOSIGNAL) == n ? 0 : -1;
}

static int say_with_fd(int sock, int fd, const char *line) {
    struct iovec iov = {(void *)line, strlen(line)};
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } cm;
    memset(&cm, 0, sizeof(cm));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cm.buf;
    msg.msg_controllen = sizeof(cm.buf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, MSG_NOSIGNAL) == (ssize_t)iov.iov_len ? 0 : -1;
}

static unsigned long field(const char *line, const char *key) {
    char k[32];
    snprintf(k, sizeof(k), " %s=", key);
    const char *p = strstr(line, k);
    return p ? strtoul(p + strlen(k), NULL, 10) : 0;
}

/* Reads one line (blocking, 5 s). */
static int read_line(int fd, char *buf, size_t size) {
    size_t n = 0;
    while (n < size - 1) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 5000) <= 0) return -1;
        char ch;
        if (recv(fd, &ch, 1, 0) != 1) return -1;
        if (ch == '\n') break;
        buf[n++] = ch;
    }
    buf[n] = 0;
    return 0;
}

static void stream(int c, const char *open_line) {
    unsigned w = (unsigned)field(open_line, "width"), h = (unsigned)field(open_line, "height");
    unsigned fps = (unsigned)field(open_line, "fps");
    if (!strstr(open_line, " format=I420") || !w || !h || (w | h) & 1 || w > 4096 || h > 4096 || !fps) {
        say(c, "err bad-request %s\n", open_line);
        return;
    }
    long page = sysconf(_SC_PAGESIZE);
    size_t frame = (size_t)w * h * 3 / 2;
    size_t stride = (frame + (size_t)page - 1) & ~((size_t)page - 1);
    size_t size = DDCAM_DATA_OFFSET + SLOTS * stride;
    int mfd = (int)syscall(SYS_memfd_create, "fake-camera", 1u /* MFD_CLOEXEC */);
    if (mfd < 0 || ftruncate(mfd, (off_t)size) < 0) {
        say(c, "err camera-error memfd: %s\n", strerror(errno));
        if (mfd >= 0) close(mfd);
        return;
    }
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (mem == MAP_FAILED) {
        say(c, "err camera-error mmap\n");
        close(mfd);
        return;
    }
    struct ddcam_header *hd = (void *)mem;
    hd->magic = DDCAM_MAGIC;
    hd->version = DDCAM_VERSION;
    hd->width = w;
    hd->height = h;
    hd->fourcc = V4L2_PIX_FMT_YUV420;
    hd->frame_size = (uint32_t)frame;
    hd->slots = SLOTS;
    hd->slot_stride = (uint32_t)stride;
    hd->data_offset = DDCAM_DATA_OFFSET;
    const char *facing = strstr(open_line, " facing=back") ? "back" : "front";
    char ok[256];
    snprintf(ok, sizeof(ok),
             "ok v=1 width=%u height=%u fps=%u format=I420 slots=%d slot_stride=%zu data_offset=%u "
             "size=%zu camera=fake facing=%s sensor=%ux%u rotation=0\n",
             w, h, fps, SLOTS, stride, DDCAM_DATA_OFFSET, size, facing, w, h);
    int sent = say_with_fd(c, mfd, ok);
    close(mfd); /* the client has its own copy now */
    if (sent < 0) {
        munmap(mem, size);
        return;
    }
    fprintf(stderr, "fake_camera_bridge: streaming %ux%u@%u\n", w, h, fps);

    struct dd_format f = {V4L2_PIX_FMT_YUV420, w, h, w, (uint32_t)frame, fps};
    void *pattern = dd_pattern_source.start(&f, 0);
    uint64_t seq = 0;
    for (;;) {
        struct pollfd p = {c, POLLIN, 0};
        if (poll(&p, 1, 0) > 0) {
            char buf[256];
            ssize_t n = recv(c, buf, sizeof(buf) - 1, MSG_DONTWAIT);
            if (n <= 0) break;
            buf[n] = 0;
            if (strstr(buf, "close")) break;
        }
        unsigned slot = (unsigned)(seq % SLOTS);
        seq++;
        __atomic_store_n(&hd->slot_seq[slot], 0, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        struct timespec ts;
        dd_pattern_source.next_frame(pattern, mem + DDCAM_DATA_OFFSET + slot * stride, (uint32_t)seq, &ts);
        hd->slot_ts[slot] = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        __atomic_store_n(&hd->slot_seq[slot], seq, __ATOMIC_RELEASE);
        if (say(c, "frame %u %llu %lld\n", slot, (unsigned long long)seq, (long long)hd->slot_ts[slot]) < 0) break;
    }
    dd_pattern_source.stop(pattern);
    munmap(mem, size);
    fprintf(stderr, "fake_camera_bridge: stream ended after %llu frames\n", (unsigned long long)seq);
}

/* Like the app: a stream that is just ending (STREAMOFF + STREAMON for a new size) gets
 * 1.5 s to go before a new open is turned away as busy. */
static int claim(void) {
    for (int i = 0; i < 30; i++) {
        if (!__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) return 1;
        struct timespec t = {0, 50000000L};
        nanosleep(&t, NULL);
    }
    return 0;
}

static void *client(void *arg) {
    int c = (int)(intptr_t)arg;
    char line[512];
    if (read_line(c, line, sizeof(line)) == 0) {
        const char *fail = getenv("FAKE_CAMERA_ANSWER");
        if (strncmp(line, "open ", 5) != 0) {
            say(c, strcmp(line, "status") == 0 ? "ok %s\n" : "err bad-request %s\n",
                strcmp(line, "status") == 0 ? (busy ? "streaming" : "idle") : line);
        } else if (fail && *fail) {
            say(c, "err %s test\n", fail);
        } else if (!claim()) {
            say(c, "err busy another program is using the camera\n");
        } else {
            stream(c, line);
            __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        }
    }
    close(c);
    return NULL;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : DDCAM_SOCKET_NAME;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    size_t n = strlen(name);
    memcpy(a.sun_path + 1, name, n);
    if (s < 0 || bind(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) < 0 ||
        listen(s, 4) < 0) {
        perror("fake_camera_bridge: listen");
        return 1;
    }
    fprintf(stderr, "fake_camera_bridge: listening on @%s\n", name);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        pthread_t t;
        if (pthread_create(&t, NULL, client, (void *)(intptr_t)c) == 0) pthread_detach(t);
        else close(c);
    }
}
