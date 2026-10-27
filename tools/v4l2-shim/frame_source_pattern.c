/*
 * Test pattern frame source for libdroiddesk_v4l2.so: scrolling 75 % colour bars, a
 * bouncing box, the frame counter in big digits, the resolution and a one-second ticker
 * (one cell per frame, so a ticker that sweeps once per second means the fps is right).
 *
 * Row 0-1 also carry the frame counter as 32 blocks of 8 px (bright = 1, MSB first) for
 * machine checks (v4l2_capture_test decodes it).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frame_source.h"

struct yuv { uint8_t y, u, v; };

/* BT.601 limited range, 75 % bars: white yellow cyan green magenta red blue black. */
static const struct yuv bars[8] = {
    {180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
    {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128},
};
static const struct yuv BLACK = {16, 128, 128};
static const struct yuv WHITE = {235, 128, 128};
static const struct yuv GREY = {60, 128, 128};
static const struct yuv BOX_A = {210, 16, 146}; /* bright yellow-green */
static const struct yuv BOX_B = {41, 240, 110}; /* deep blue */

/* 5x7 font: '0'-'9', 'x', ' ', 'f', 'p', 's'. Each row is 5 bits, MSB = left. */
static const uint8_t font_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};
static const uint8_t font_x[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11};
static const uint8_t font_f[7] = {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08};
static const uint8_t font_p[7] = {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10};
static const uint8_t font_s[7] = {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E};

struct pattern {
    struct dd_format f;
    struct timespec next;
    long period_ns;
    uint8_t *row; /* one packed/planar row template, 2 * width bytes */
};

static void ts_add(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static long ts_diff_ns(const struct timespec *a, const struct timespec *b) {
    return (long)(a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

/* Fills a rectangle; x and w are rounded to even values (chroma is shared by 2 pixels). */
static void fill_rect(const struct dd_format *f, uint8_t *dst, int x, int y, int w, int h,
                      struct yuv c) {
    int W = (int)f->width, H = (int)f->height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    x &= ~1;
    w = (w + 1) & ~1;
    if (x + w > W) w = W - x;
    if (w <= 0 || h <= 0) return;
    if (f->fourcc == V4L2_PIX_FMT_YUYV) {
        for (int r = y; r < y + h; r++) {
            uint8_t *p = dst + (size_t)r * f->bytesperline + (size_t)x * 2;
            for (int i = 0; i < w; i += 2, p += 4) {
                p[0] = c.y; p[1] = c.u; p[2] = c.y; p[3] = c.v;
            }
        }
        return;
    }
    for (int r = y; r < y + h; r++) memset(dst + (size_t)r * W + x, c.y, (size_t)w);
    uint8_t *chroma = dst + (size_t)W * H;
    int cy0 = y / 2, cy1 = (y + h + 1) / 2;
    if (f->fourcc == V4L2_PIX_FMT_NV12) {
        for (int r = cy0; r < cy1; r++) {
            uint8_t *p = chroma + (size_t)r * W + x;
            for (int i = 0; i < w; i += 2, p += 2) { p[0] = c.u; p[1] = c.v; }
        }
    } else { /* YUV420 (I420): U plane, then V plane */
        uint8_t *u = chroma, *v = chroma + (size_t)(W / 2) * (H / 2);
        for (int r = cy0; r < cy1; r++) {
            memset(u + (size_t)r * (W / 2) + x / 2, c.u, (size_t)w / 2);
            memset(v + (size_t)r * (W / 2) + x / 2, c.v, (size_t)w / 2);
        }
    }
}

static void draw_glyph(const struct dd_format *f, uint8_t *dst, const uint8_t g[7], int x, int y,
                       int scale, struct yuv c) {
    for (int r = 0; r < 7; r++)
        for (int col = 0; col < 5; col++)
            if (g[r] & (0x10 >> col)) fill_rect(f, dst, x + col * scale, y + r * scale, scale, scale, c);
}

static int draw_text(const struct dd_format *f, uint8_t *dst, const char *s, int x, int y, int scale,
                     struct yuv c) {
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = NULL;
        if (*s >= '0' && *s <= '9') g = font_digits[*s - '0'];
        else if (*s == 'x') g = font_x;
        else if (*s == 'f') g = font_f;
        else if (*s == 'p') g = font_p;
        else if (*s == 's') g = font_s;
        if (g) draw_glyph(f, dst, g, x, y, scale, c);
    }
    return x;
}

/* Scrolling bars: build one row, copy it to every row. */
static void draw_bars(struct pattern *p, uint8_t *dst, uint32_t seq) {
    const struct dd_format *f = &p->f;
    int W = (int)f->width, H = (int)f->height;
    /* one screen width every 4 seconds */
    int shift = (int)(((uint64_t)seq * (uint64_t)W / (4u * (f->fps ? f->fps : 30))) % (uint64_t)W);
    shift &= ~1;
    uint8_t *row = p->row;
    if (f->fourcc == V4L2_PIX_FMT_YUYV) {
        for (int x = 0; x < W; x += 2) {
            struct yuv c = bars[((x + shift) % W) * 8 / W];
            row[2 * x] = c.y; row[2 * x + 1] = c.u; row[2 * x + 2] = c.y; row[2 * x + 3] = c.v;
        }
        for (int r = 0; r < H; r++) memcpy(dst + (size_t)r * f->bytesperline, row, (size_t)W * 2);
        return;
    }
    /* row[0..W) = Y, row[W..2W) = chroma row for NV12 (UV pairs) or U|V halves for I420 */
    uint8_t *cy = row, *cc = row + W;
    for (int x = 0; x < W; x += 2) {
        struct yuv c = bars[((x + shift) % W) * 8 / W];
        cy[x] = cy[x + 1] = c.y;
        if (f->fourcc == V4L2_PIX_FMT_NV12) { cc[x] = c.u; cc[x + 1] = c.v; }
        else { cc[x / 2] = c.u; cc[W / 2 + x / 2] = c.v; }
    }
    for (int r = 0; r < H; r++) memcpy(dst + (size_t)r * W, cy, (size_t)W);
    uint8_t *chroma = dst + (size_t)W * H;
    if (f->fourcc == V4L2_PIX_FMT_NV12) {
        for (int r = 0; r < H / 2; r++) memcpy(chroma + (size_t)r * W, cc, (size_t)W);
    } else {
        uint8_t *u = chroma, *v = chroma + (size_t)(W / 2) * (H / 2);
        for (int r = 0; r < H / 2; r++) {
            memcpy(u + (size_t)r * (W / 2), cc, (size_t)W / 2);
            memcpy(v + (size_t)r * (W / 2), cc + W / 2, (size_t)W / 2);
        }
    }
}

/* Triangle wave 0..range..0 */
static int bounce(uint64_t t, int range) {
    if (range <= 0) return 0;
    uint64_t m = t % (uint64_t)(2 * range);
    return (int)(m < (uint64_t)range ? m : (uint64_t)(2 * range) - m);
}

static void render(struct pattern *p, uint8_t *dst, uint32_t seq) {
    const struct dd_format *f = &p->f;
    int W = (int)f->width, H = (int)f->height;
    unsigned fps = f->fps ? f->fps : 30;
    draw_bars(p, dst, seq);

    /* bouncing box, colour flips every second */
    int S = H / 5;
    int bx = bounce((uint64_t)seq * (unsigned)(W / 2) / fps, W - S);
    int by = bounce((uint64_t)seq * (unsigned)(H / 2) / fps, H - S);
    fill_rect(f, dst, bx, by, S, S, (seq / fps) % 2 ? BOX_B : BOX_A);

    /* frame counter and format */
    int scale = H / 72;
    if (scale < 2) scale = 2;
    char line1[16], line2[32];
    snprintf(line1, sizeof(line1), "%06u", seq % 1000000u);
    snprintf(line2, sizeof(line2), "%ux%u %ufps", f->width, f->height, fps);
    int sc2 = scale / 2 > 1 ? scale / 2 : 1;
    int panel_w = 6 * scale * 6 + 2 * scale;
    int w2 = (int)strlen(line2) * 6 * sc2 + 2 * scale;
    if (w2 > panel_w) panel_w = w2;
    int panel_h = 8 * scale + 8 * sc2 + 3 * scale;
    fill_rect(f, dst, 2 * scale, 4 * scale, panel_w, panel_h, BLACK);
    draw_text(f, dst, line1, 3 * scale, 5 * scale, scale, WHITE);
    draw_text(f, dst, line2, 3 * scale, 5 * scale + 8 * scale + scale, sc2, WHITE);

    /* one-second ticker: fps cells, the current one lit */
    int th = H / 20, ty = H - th - H / 40;
    for (unsigned i = 0; i < fps; i++) {
        int x0 = (int)((uint64_t)W * i / fps), x1 = (int)((uint64_t)W * (i + 1) / fps);
        fill_rect(f, dst, x0 + 2, ty, x1 - x0 - 4, th, i == seq % fps ? WHITE : GREY);
    }

    /* machine-readable counter in rows 0-1 */
    for (int b = 0; b < 32; b++)
        fill_rect(f, dst, b * 8, 0, 8, 2, (seq >> (31 - b)) & 1 ? WHITE : BLACK);
}

static void *pattern_start(const struct dd_format *format, int device) {
    (void)device;
    struct pattern *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->f = *format;
    p->row = malloc((size_t)format->width * 2 + 8);
    if (!p->row) { free(p); return NULL; }
    p->period_ns = 1000000000L / (format->fps ? format->fps : 30);
    clock_gettime(CLOCK_MONOTONIC, &p->next);
    return p;
}

static int pattern_next_frame(void *ctx, uint8_t *dst, uint32_t sequence, struct timespec *timestamp) {
    struct pattern *p = ctx;
    ts_add(&p->next, p->period_ns);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (ts_diff_ns(&now, &p->next) > 3 * p->period_ns) p->next = now; /* fell behind: resync */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &p->next, NULL) == EINTR) {}
    clock_gettime(CLOCK_MONOTONIC, timestamp);
    if (dst) render(p, dst, sequence);
    return dst ? 0 : -1;
}

static void pattern_stop(void *ctx) {
    struct pattern *p = ctx;
    if (!p) return;
    free(p->row);
    free(p);
}

const struct dd_frame_source dd_pattern_source = {
    "pattern", pattern_start, pattern_next_frame, pattern_stop,
};
