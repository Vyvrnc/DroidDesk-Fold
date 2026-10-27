#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
/* Host test of app/android/app/src/main/cpp/camera_jni.c (CameraBridge conversion): rotation,
 * mirror, centre crop, scaling, chroma de-interleaving, bounds check, seqlock word. Run by
 * test-glibc.sh; stub/ stands in for jni.h and the NDK shared memory API. */
#include "../../../app/android/app/src/main/cpp/camera_jni.c"

struct buf { uint8_t *p; long cap; };
static void *addr(void *env, jobject b) { (void)env; return ((struct buf *)b)->p; }
static jlong cap(void *env, jobject b) { (void)env; return ((struct buf *)b)->cap; }
static const struct JNINativeInterface_ iface = {addr, cap};
static JNIEnv envv = &iface;

static int fails;
#define SW 8
#define SH 4

int main(void) {
    /* source: Y with row stride 16, chroma interleaved (V first like NV21: U at +1) */
    static uint8_t y[16 * SH], vu[16 * (SH / 2)];
    for (int r = 0; r < SH; r++) for (int c = 0; c < SW; c++) y[r * 16 + c] = (uint8_t)(10 * r + c + 1);
    for (int r = 0; r < SH / 2; r++) for (int c = 0; c < SW / 2; c++) {
        vu[r * 16 + 2 * c] = (uint8_t)(100 + 10 * r + c);     /* V */
        vu[r * 16 + 2 * c + 1] = (uint8_t)(200 + 10 * r + c); /* U */
    }
    struct buf Y = {y, sizeof(y)}, U = {vu + 1, sizeof(vu) - 1}, V = {vu, sizeof(vu) - 1};
    struct { int rot, mirror, dw, dh; } cases[] = {{0, 0, 8, 4}, {90, 0, 4, 8}, {180, 0, 8, 4}, {270, 0, 4, 8}, {0, 1, 8, 4}, {90, 1, 4, 8}};
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        int dw = cases[k].dw, dh = cases[k].dh, rot = cases[k].rot, mir = cases[k].mirror;
        int size = 4096 + 3 * 4096;
        int fd = (int)Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeCreate(&envv, 0, size);
        jlong a = Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeMap(&envv, 0, fd, size);
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeInitHeader(&envv, 0, a, dw, dh, 3, 4096, 4096);
        int r = Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeWriteFrame(&envv, 0, a, 1, 7, 123, &Y, &U, &V, 16, 16, 2, SW, SH, rot, mir);
        struct ddcam_header *h = (void *)(uintptr_t)a;
        uint8_t *d = (uint8_t *)h + 4096 + 4096;
        int bad = r != 0 || h->slot_seq[1] != 7;
        for (int yy = 0; yy < dh; yy++) for (int xx = 0; xx < dw; xx++) {
            int rx = mir ? dw - 1 - xx : xx, ry = yy, sx, sy;
            switch (rot) {
                case 90: sx = ry; sy = SH - 1 - rx; break;
                case 180: sx = SW - 1 - rx; sy = SH - 1 - ry; break;
                case 270: sx = SW - 1 - ry; sy = rx; break;
                default: sx = rx; sy = ry;
            }
            if (d[yy * dw + xx] != y[sy * 16 + sx]) bad = 1;
        }
        /* chroma */
        int cw = dw / 2, ch = dh / 2;
        uint8_t *du = d + dw * dh, *dv = du + cw * ch;
        for (int yy = 0; yy < ch; yy++) for (int xx = 0; xx < cw; xx++) {
            int rx = mir ? cw - 1 - xx : xx, ry = yy, sx, sy;
            switch (rot) {
                case 90: sx = ry; sy = SH / 2 - 1 - rx; break;
                case 180: sx = SW / 2 - 1 - rx; sy = SH / 2 - 1 - ry; break;
                case 270: sx = SW / 2 - 1 - ry; sy = rx; break;
                default: sx = rx; sy = ry;
            }
            if (du[yy * cw + xx] != vu[sy * 16 + 2 * sx + 1] || dv[yy * cw + xx] != vu[sy * 16 + 2 * sx]) bad = 1;
        }
        printf("rot %3d mirror %d -> %dx%d: %s\n", rot, mir, dw, dh, bad ? "FAIL" : "ok");
        if (bad) {
            fails++;
            for (int yy = 0; yy < dh; yy++) { for (int xx = 0; xx < dw; xx++) printf("%3d ", d[yy * dw + xx]); printf("\n"); }
        }
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeUnmap(&envv, 0, a, size);
        close(fd);
    }
    /* crop + scale: 8x4 -> 4x4 (centre crop 4x4, 1:1) and 8x4 -> 4x2 (2:1 down) */
    {
        int size = 4096 * 4;
        int fd = (int)Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeCreate(&envv, 0, size);
        jlong a = Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeMap(&envv, 0, fd, size);
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeInitHeader(&envv, 0, a, 4, 4, 3, 4096, 4096);
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeWriteFrame(&envv, 0, a, 0, 1, 0, &Y, &U, &V, 16, 16, 2, SW, SH, 0, 0);
        uint8_t *d = (uint8_t *)(uintptr_t)a + 4096;
        int bad = 0;
        for (int yy = 0; yy < 4; yy++) for (int xx = 0; xx < 4; xx++) if (d[yy * 4 + xx] != y[yy * 16 + xx + 2]) bad = 1;
        printf("centre crop 8x4 -> 4x4: %s\n", bad ? "FAIL" : "ok");
        fails += bad;
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeInitHeader(&envv, 0, a, 4, 2, 3, 4096, 4096);
        Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeWriteFrame(&envv, 0, a, 0, 2, 0, &Y, &U, &V, 16, 16, 2, SW, SH, 0, 0);
        bad = 0;
        for (int yy = 0; yy < 2; yy++) for (int xx = 0; xx < 4; xx++) {
            int want = (y[2 * yy * 16 + 2 * xx] + y[2 * yy * 16 + 2 * xx + 1] + y[(2 * yy + 1) * 16 + 2 * xx] + y[(2 * yy + 1) * 16 + 2 * xx + 1] + 2) / 4;
            if (abs(d[yy * 4 + xx] - want) > 1) bad = 1;
        }
        printf("downscale 8x4 -> 4x2: %s\n", bad ? "FAIL" : "ok");
        fails += bad;
        int r = Java_com_orailnoor_droiddesk_runtime_CameraBridge_nativeWriteFrame(&envv, 0, a, 0, 3, 0, &Y, &U, &V, 16, 16, 2, SW, SH + 2, 0, 0);
        printf("short buffer rejected: %s\n", r == -EFAULT ? "ok" : "FAIL");
        fails += r != -EFAULT;
    }
    printf(fails ? "JNI CONVERSION: FAIL\n" : "JNI CONVERSION: PASS\n");
    return fails != 0;
}
