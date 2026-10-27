/*
 * v4l2_capture_test — replays the V4L2 call sequence of Firefox's libwebrtc capture module
 * (third_party/libwebrtc/modules/video_capture/linux/{device_info,video_capture}_v4l2.cc,
 * Firefox 157) against /dev/video0, then checks the frames.
 *
 *   v4l2_capture_test [seconds-per-run] [ppm-prefix]
 *
 * 1. Enumeration like DeviceInfoV4l2: open("/dev/videoN", O_RDONLY) for N = 0..63, QUERYCAP,
 *    card/bus_info; FillCapabilities: TRY_FMT over libwebrtc's 15 formats x 13 sizes.
 * 2. Capture like VideoCaptureModuleV4L2::StartCapture/CaptureProcess/StopCapture for three
 *    modes: open(O_RDWR|O_NONBLOCK), ENUM_FMT + preference, S_FMT, G_PARM/S_PARM, REQBUFS 4,
 *    QUERYBUF + mmap + QBUF, STREAMON, poll(POLLIN, 1000) + DQBUF + QBUF, munmap, STREAMOFF,
 *    close. Checks bytesused, the frame counter in rows 0-1 and the frame rate. With
 *    V4L2_TEST_SOURCE=bridge (frames from fake_camera_bridge) the counter only has to rise.
 * 3. fd behaviour: dup, fcntl(F_DUPFD_CLOEXEC/F_GETFL), fstat, close order, and that other
 *    fds (/dev/null) pass through untouched.
 *
 * Exit code 0 = all checks passed. Builds with the NDK (bionic) and gcc (glibc).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int bridge_mode;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            failures++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf(" (errno %d %s)\n", errno, strerror(errno)); \
        }                                                    \
    } while (0)

static const char *fcc(uint32_t f) {
    static char s[4][5];
    static int k;
    char *o = s[k++ & 3];
    for (int i = 0; i < 4; i++) o[i] = (char)((f >> (8 * i)) & 0xff);
    o[4] = 0;
    return o;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int is_capture(struct v4l2_capability *cap) {
    if (cap->capabilities & V4L2_CAP_DEVICE_CAPS) return !!(cap->device_caps & V4L2_CAP_VIDEO_CAPTURE);
    return !!(cap->capabilities & V4L2_CAP_VIDEO_CAPTURE);
}

/* DeviceInfoV4l2::NumberOfDevices + GetDeviceName + FillCapabilities */
static int enumerate(void) {
    int count = 0;
    for (int n = 0; n < 64; n++) {
        char dev[20];
        snprintf(dev, sizeof(dev), "/dev/video%d", n);
        int fd = open(dev, O_RDONLY);
        if (fd == -1) continue;
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 || !is_capture(&cap)) {
            close(fd);
            continue;
        }
        count++;
        printf("device %s: card '%s' driver '%s' bus_info '%s' caps 0x%08x device_caps 0x%08x\n", dev,
               cap.card, cap.driver, cap.bus_info, cap.capabilities, cap.device_caps);
        static char prev_bus[32];
        CHECK(cap.bus_info[0] != 0 && strcmp((char *)cap.bus_info, prev_bus) != 0,
              "bus_info must be set and unique (libwebrtc matches devices by it)");
        snprintf(prev_bus, sizeof(prev_bus), "%s", (char *)cap.bus_info);
        CHECK(cap.device_caps & V4L2_CAP_STREAMING, "STREAMING capability");

        static const uint32_t fmts[] = {
            V4L2_PIX_FMT_MJPEG,  V4L2_PIX_FMT_JPEG,   V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420,
            V4L2_PIX_FMT_YUYV,   V4L2_PIX_FMT_UYVY,   V4L2_PIX_FMT_NV12,   V4L2_PIX_FMT_BGR24,
            V4L2_PIX_FMT_RGB24,  V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_ARGB32,
            V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_BGR32,  V4L2_PIX_FMT_RGB32,
        };
        static const unsigned sz[][2] = {{128, 96},  {160, 120},  {176, 144},  {320, 240},  {352, 288},
                                         {640, 480}, {704, 576},  {800, 600},  {960, 720},  {1280, 720},
                                         {1024, 768}, {1440, 1080}, {1920, 1080}};
        struct v4l2_format f;
        memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int caps = 0;
        printf("  capabilities (TRY_FMT, as libwebrtc FillCapabilities):");
        for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
            for (unsigned s = 0; s < sizeof(sz) / sizeof(sz[0]); s++) {
                f.fmt.pix.pixelformat = fmts[i];
                f.fmt.pix.width = sz[s][0];
                f.fmt.pix.height = sz[s][1];
                if (ioctl(fd, VIDIOC_TRY_FMT, &f) >= 0 && f.fmt.pix.width == sz[s][0] &&
                    f.fmt.pix.height == sz[s][1]) {
                    printf(" %s/%ux%u@%u", fcc(fmts[i]), sz[s][0], sz[s][1],
                           sz[s][0] >= 800 && fmts[i] != V4L2_PIX_FMT_MJPEG ? 15 : 30);
                    caps++;
                }
            }
        printf("\n");
        CHECK(caps == 6, "expected 6 capabilities (YU12/NV12/YUYV x 1280x720/640x480), got %d", caps);

        struct v4l2_fmtdesc d;
        memset(&d, 0, sizeof(d));
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (d.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++) {
            printf("  ENUM_FMT %u: %s '%s' sizes:", d.index, fcc(d.pixelformat), d.description);
            struct v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof(fs));
            fs.pixel_format = d.pixelformat;
            for (fs.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
                printf(" %ux%u(", fs.discrete.width, fs.discrete.height);
                struct v4l2_frmivalenum iv;
                memset(&iv, 0, sizeof(iv));
                iv.pixel_format = d.pixelformat;
                iv.width = fs.discrete.width;
                iv.height = fs.discrete.height;
                for (iv.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; iv.index++)
                    printf("%s%u/%u", iv.index ? "," : "", iv.discrete.numerator, iv.discrete.denominator);
                printf(")");
            }
            printf("\n");
        }
        close(fd);
    }
    return count;
}

static uint32_t decode_counter(const uint8_t *frame, uint32_t fourcc) {
    uint32_t v = 0;
    for (int b = 0; b < 32; b++) {
        int x = b * 8 + 4;
        uint8_t y = fourcc == V4L2_PIX_FMT_YUYV ? frame[x * 2] : frame[x];
        v = (v << 1) | (y > 128);
    }
    return v;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void write_ppm(const char *path, const uint8_t *fr, uint32_t fourcc, unsigned W, unsigned H) {
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u %u\n255\n", W, H);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            int Y, U, V;
            if (fourcc == V4L2_PIX_FMT_YUYV) {
                const uint8_t *p = fr + (size_t)y * W * 2 + (x & ~1u) * 2;
                Y = p[(x & 1) * 2]; U = p[1]; V = p[3];
            } else if (fourcc == V4L2_PIX_FMT_NV12) {
                const uint8_t *c = fr + (size_t)W * H + (size_t)(y / 2) * W + (x & ~1u);
                Y = fr[(size_t)y * W + x]; U = c[0]; V = c[1];
            } else {
                Y = fr[(size_t)y * W + x];
                U = fr[(size_t)W * H + (size_t)(y / 2) * (W / 2) + x / 2];
                V = fr[(size_t)W * H + (size_t)(W / 2) * (H / 2) + (size_t)(y / 2) * (W / 2) + x / 2];
            }
            int c = Y - 16, d = U - 128, e = V - 128;
            uint8_t rgb[3] = {clamp8((298 * c + 409 * e + 128) >> 8),
                              clamp8((298 * c - 100 * d - 208 * e + 128) >> 8),
                              clamp8((298 * c + 516 * d + 128) >> 8)};
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    printf("  wrote %s\n", path);
}

/* VideoCaptureModuleV4L2::StartCapture + CaptureProcess loop + StopCapture */
static void capture(unsigned W, unsigned H, unsigned fps, uint32_t force, double secs, const char *ppm) {
    printf("capture %ux%u@%u%s%s\n", W, H, fps, force ? " forced " : "", force ? fcc(force) : "");
    int fd = open("/dev/video0", O_RDWR | O_NONBLOCK, 0);
    CHECK(fd >= 0, "open O_RDWR|O_NONBLOCK");
    if (fd < 0) return;

    static const uint32_t hd[] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420,
                                  V4L2_PIX_FMT_YUYV,  V4L2_PIX_FMT_UYVY,   V4L2_PIX_FMT_NV12};
    static const uint32_t sd[] = {V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420, V4L2_PIX_FMT_YUYV,
                                  V4L2_PIX_FMT_UYVY,   V4L2_PIX_FMT_NV12,   V4L2_PIX_FMT_MJPEG};
    const uint32_t *pref = W > 640 || H > 480 ? hd : sd;
    int best = 6;
    struct v4l2_fmtdesc d;
    memset(&d, 0, sizeof(d));
    d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0) {
        for (int i = 0; i < 6; i++)
            if (d.pixelformat == pref[i] && i < best) best = i;
        d.index++;
    }
    CHECK(best < 6, "no preferred format");
    uint32_t fourcc = force ? force : pref[best < 6 ? best : 0];

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = W;
    f.fmt.pix.height = H;
    f.fmt.pix.pixelformat = fourcc;
    CHECK(ioctl(fd, VIDIOC_S_FMT, &f) == 0, "S_FMT");
    CHECK(f.fmt.pix.width == W && f.fmt.pix.height == H && f.fmt.pix.pixelformat == fourcc,
          "S_FMT returned %ux%u %s", f.fmt.pix.width, f.fmt.pix.height, fcc(f.fmt.pix.pixelformat));
    uint32_t sizeimage = f.fmt.pix.sizeimage;

    struct v4l2_streamparm sp;
    memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK(ioctl(fd, VIDIOC_G_PARM, &sp) == 0, "G_PARM");
    CHECK(sp.parm.capture.capability & V4L2_CAP_TIMEPERFRAME, "TIMEPERFRAME");
    memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = fps;
    CHECK(ioctl(fd, VIDIOC_S_PARM, &sp) == 0, "S_PARM");

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    rb.count = 4;
    CHECK(ioctl(fd, VIDIOC_REQBUFS, &rb) == 0 && rb.count == 4, "REQBUFS");
    void *start[8] = {0};
    size_t len[8] = {0};
    for (unsigned i = 0; i < rb.count && i < 8; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        CHECK(ioctl(fd, VIDIOC_QUERYBUF, &b) == 0, "QUERYBUF %u", i);
        start[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        CHECK(start[i] != MAP_FAILED, "mmap buffer %u", i);
        len[i] = b.length;
        CHECK(ioctl(fd, VIDIOC_QBUF, &b) == 0, "QBUF %u", i);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CHECK(ioctl(fd, VIDIOC_STREAMON, &type) == 0, "STREAMON");

    /* fd behaviour while streaming */
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "fstat is a char device");
    int fl = fcntl(fd, F_GETFL);
    CHECK(fl >= 0 && (fl & O_NONBLOCK), "F_GETFL has O_NONBLOCK");
    int d1 = dup(fd);
    struct v4l2_capability cap;
    CHECK(d1 >= 0 && ioctl(d1, VIDIOC_QUERYCAP, &cap) == 0, "QUERYCAP on dup");
    int d2 = fcntl(fd, F_DUPFD_CLOEXEC, 100);
    CHECK(d2 >= 100 && ioctl(d2, VIDIOC_QUERYCAP, &cap) == 0, "QUERYCAP on F_DUPFD_CLOEXEC");
    close(d1);
    close(d2);

    double t0 = now_s(), first = 0, last = 0;
    unsigned frames = 0, polls = 0, timeouts = 0, bad_counter = 0, skipped = 0;
    uint32_t prev = 0, first_seq = 0, last_seq = 0;
    int have_prev = 0;
    uint8_t *saved = NULL;
    while (now_s() - t0 < secs) {
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, 1000);
        polls++;
        if (r == 0) { timeouts++; continue; }
        if (r < 0 || !(p.revents & POLLIN)) continue;
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            CHECK(errno == EAGAIN || errno == EINTR, "DQBUF");
            continue;
        }
        double t = now_s();
        if (!frames) { first = t; first_seq = b.sequence; }
        last = t;
        last_seq = b.sequence;
        frames++;
        CHECK(b.bytesused == sizeimage, "bytesused %u != %u", b.bytesused, sizeimage);
        uint32_t ctr = decode_counter(start[b.index], fourcc);
        /* pattern: the image counter is the V4L2 sequence; camera bridge (V4L2_TEST_SOURCE=bridge,
         * fake_camera_bridge): the bridge's frame seq, which only has to rise */
        if (bridge_mode ? have_prev && ctr <= prev : ctr != b.sequence) bad_counter++;
        if (have_prev && ctr != prev + 1) skipped += ctr - prev - 1;
        prev = ctr;
        have_prev = 1;
        if (ppm && !saved && frames == 20 && (saved = malloc(sizeimage)))
            memcpy(saved, start[b.index], sizeimage);
        CHECK(ioctl(fd, VIDIOC_QBUF, &b) == 0, "re-QBUF");
    }
    /* source rate from the sequence numbers (counts frames dropped because the reader was
     * slow, e.g. under qemu); delivered rate from what this reader dequeued */
    double span = last - first;
    double source_rate = frames > 1 && span > 0 ? (last_seq - first_seq) / span : 0;
    double delivered = frames > 1 && span > 0 ? (frames - 1) / span : 0;
    printf("  %s %ux%u: %u frames in %.2f s, source %.1f fps, delivered %.1f fps (asked %u), "
           "%u polls, %u timeouts, %u counter mismatches, %u dropped\n", fcc(fourcc), W, H, frames,
           span, source_rate, delivered, fps, polls, timeouts, bad_counter, skipped);
    CHECK(frames > 0, "no frames");
    CHECK(source_rate > fps * 0.9 && source_rate < fps * 1.1, "source rate %.1f not within 10 %% of %u",
          source_rate, fps);
    CHECK(delivered > fps * 0.5, "delivered rate %.1f below half of %u", delivered, fps);
    CHECK(bad_counter == 0, bridge_mode ? "bridge frame counter in the image did not rise"
                                        : "frame counter in the image != v4l2_buffer.sequence");
    if (saved) {
        char path[256];
        snprintf(path, sizeof(path), "%s-%ux%u-%s.ppm", ppm, W, H, fcc(fourcc));
        write_ppm(path, saved, fourcc, W, H);
        free(saved);
    }

    for (unsigned i = 0; i < rb.count && i < 8; i++)
        if (start[i] != MAP_FAILED) munmap(start[i], len[i]);
    CHECK(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF");
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 0) == 0, "no POLLIN after STREAMOFF");
    CHECK(close(fd) == 0, "close");
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 3.0;
    const char *ppm = argc > 2 ? argv[2] : NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *src = getenv("V4L2_TEST_SOURCE");
    bridge_mode = src && strcmp(src, "bridge") == 0;

    /* passthrough sanity: a normal fd is untouched */
    int nul = open("/dev/null", O_RDWR);
    struct v4l2_capability cap;
    CHECK(nul >= 0, "open /dev/null");
    CHECK(ioctl(nul, VIDIOC_QUERYCAP, &cap) < 0 && errno == ENOTTY, "/dev/null QUERYCAP must be ENOTTY");
    CHECK(write(nul, "x", 1) == 1, "write /dev/null");
    close(nul);
    struct stat st;
    CHECK(stat("/dev/video0", &st) == 0 && S_ISCHR(st.st_mode), "stat /dev/video0");
    CHECK(access("/dev/video0", R_OK | W_OK) == 0, "access /dev/video0");
    CHECK(stat("/dev/video1", &st) == 0 && minor(st.st_rdev) == 1, "stat /dev/video1 (back camera)");
    CHECK(open("/dev/video2", O_RDONLY) < 0, "/dev/video2 must not exist");

    int n = enumerate();
    CHECK(n == 2, "expected 2 capture devices (front, back), found %d", n);
    if (n) {
        capture(1280, 720, 15, 0, secs, ppm); /* Firefox: HD capability, fps guessed 15 */
        capture(640, 480, 30, 0, secs, ppm);  /* Firefox: SD, I420, 30 fps */
        capture(640, 480, 30, V4L2_PIX_FMT_YUYV, secs, ppm);
        capture(1280, 720, 30, V4L2_PIX_FMT_NV12, secs, ppm);
    }
    printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: PASS\n", failures);
    return failures ? 1 : 0;
}
