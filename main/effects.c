#include "effects.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }
static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t xorshift32(void) {
    static uint32_t state = 0x12345678;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* Gamma correction: (v/255)^gamma * 255, gamma derived from thresh.
 * thresh 128 → gamma 1.0 (neutral)
 * thresh 0   → gamma 2.0 (darker midtones)
 * thresh 255 → gamma 0.25 (brighter midtones)
 */
static float gamma_from_thresh(int thresh) {
    float g = 2.0f - thresh / 128.0f;
    if (g < 0.1f) g = 0.1f;
    if (g > 3.9f) g = 3.9f;
    return g;
}

static uint8_t apply_gamma(uint8_t v, float gamma) {
    float fv = v / 255.0f;
    float gv = powf(fv, gamma);
    int r = (int)(gv * 255.0f + 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return (uint8_t)r;
}

/* ── Floyd-Steinberg ────────────────────────────────────────── */

static void dither_fs(const uint8_t *grey, int w, int h, uint8_t *out) {
    int16_t *buf = (int16_t *)malloc((w + 2) * sizeof(int16_t) * 2);
    if (!buf) return;
    int16_t *err_cur = buf;
    int16_t *err_next = buf + (w + 2);
    memset(err_cur, 0, (w + 2) * sizeof(int16_t));

    for (int y = 0; y < h; y++) {
        memset(err_next, 0, (w + 2) * sizeof(int16_t));
        for (int x = 0; x < w; x++) {
            int old = grey[y * w + x] + err_cur[x + 1];
            int np = old > 128 ? 255 : 0;
            out[y * w + x] = np;
            int err = old - np;
            if (x + 1 < w)   err_cur[x + 2]   += (err * 7) / 16;
            if (x - 1 >= 0)  err_next[x]      += (err * 3) / 16;
            err_next[x + 1]  += (err * 5) / 16;
            if (x + 1 < w)   err_next[x + 2]  += (err * 1) / 16;
        }
        int16_t *tmp = err_cur; err_cur = err_next; err_next = tmp;
    }
    free(buf);
}

/* ── Atkinson ───────────────────────────────────────────────── */

static void dither_atkinson(const uint8_t *grey, int w, int h, uint8_t *out) {
    int stride = w + 4;
    int32_t *buf = (int32_t *)calloc(stride * 3, sizeof(int32_t));
    if (!buf) return;
    int32_t *row0 = buf, *row1 = buf + stride, *row2 = buf + stride * 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int old = grey[y * w + x] + row0[x + 2];
            int np = old > 128 ? 255 : 0;
            out[y * w + x] = np;
            int err8 = (old - np) / 8;
            if (x + 1 < w)   row0[x + 3]  += err8;
            if (x + 2 < w)   row0[x + 4]  += err8;
            if (x - 1 >= 0)  row1[x + 1]  += err8;
            row1[x + 2] += err8;
            if (x + 1 < w)   row1[x + 3]  += err8;
            row2[x + 2] += err8;
        }
        int32_t *tmp = row0; row0 = row1; row1 = row2; row2 = tmp;
        memset(row2, 0, stride * sizeof(int32_t));
    }
    free(buf);
}

/* ── Jarvis ─────────────────────────────────────────────────── */

static void dither_jarvis(const uint8_t *grey, int w, int h, uint8_t *out) {
    int stride = w + 4;
    int32_t *buf = (int32_t *)calloc(stride * 3, sizeof(int32_t));
    if (!buf) return;
    int32_t *r0 = buf, *r1 = buf + stride, *r2 = buf + stride * 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int old = grey[y * w + x] + r0[x + 2];
            int np = old > 128 ? 255 : 0;
            out[y * w + x] = np;
            int err = old - np;
            if (x + 1 < w)  r0[x + 3] += (err * 7) / 48;
            if (x + 2 < w)  r0[x + 4] += (err * 5) / 48;
            if (x - 2 >= 0) r1[x]     += (err * 3) / 48;
            if (x - 1 >= 0) r1[x + 1] += (err * 5) / 48;
            r1[x + 2] += (err * 7) / 48;
            if (x + 1 < w)  r1[x + 3] += (err * 5) / 48;
            if (x + 2 < w)  r1[x + 4] += (err * 3) / 48;
            if (x - 2 >= 0) r2[x]     += (err * 1) / 48;
            if (x - 1 >= 0) r2[x + 1] += (err * 3) / 48;
            r2[x + 2] += (err * 5) / 48;
            if (x + 1 < w)  r2[x + 3] += (err * 3) / 48;
            if (x + 2 < w)  r2[x + 4] += (err * 1) / 48;
        }
        int32_t *tmp = r0; r0 = r1; r1 = r2; r2 = tmp;
        memset(r2, 0, stride * sizeof(int32_t));
    }
    free(buf);
}

/* ── Stucki ─────────────────────────────────────────────────── */

static void dither_stucki(const uint8_t *grey, int w, int h, uint8_t *out) {
    int stride = w + 4;
    int32_t *buf = (int32_t *)calloc(stride * 3, sizeof(int32_t));
    if (!buf) return;
    int32_t *r0 = buf, *r1 = buf + stride, *r2 = buf + stride * 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int old = grey[y * w + x] + r0[x + 2];
            int np = old > 128 ? 255 : 0;
            out[y * w + x] = np;
            int err = old - np;
            if (x + 1 < w)  r0[x + 3] += (err * 8) / 42;
            if (x + 2 < w)  r0[x + 4] += (err * 4) / 42;
            if (x - 2 >= 0) r1[x]     += (err * 2) / 42;
            if (x - 1 >= 0) r1[x + 1] += (err * 4) / 42;
            r1[x + 2] += (err * 8) / 42;
            if (x + 1 < w)  r1[x + 3] += (err * 4) / 42;
            if (x + 2 < w)  r1[x + 4] += (err * 2) / 42;
            if (x - 2 >= 0) r2[x]     += (err * 1) / 42;
            if (x - 1 >= 0) r2[x + 1] += (err * 2) / 42;
            r2[x + 2] += (err * 4) / 42;
            if (x + 1 < w)  r2[x + 3] += (err * 2) / 42;
            if (x + 2 < w)  r2[x + 4] += (err * 1) / 42;
        }
        int32_t *tmp = r0; r0 = r1; r1 = r2; r2 = tmp;
        memset(r2, 0, stride * sizeof(int32_t));
    }
    free(buf);
}

/* ── Bayer ──────────────────────────────────────────────────── */

static void dither_bayer(const uint8_t *grey, int w, int h, uint8_t *out, int n) {
    static const uint8_t bayer2[2][2] = {{0,2},{3,1}};
    static const uint8_t bayer4[4][4] = {
        {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}
    };
    static const uint8_t bayer8[8][8] = {
        {0,48,12,60,3,51,15,63},
        {32,16,44,28,35,19,47,31},
        {8,56,4,52,11,59,7,55},
        {40,24,36,20,43,27,39,23},
        {2,50,14,62,1,49,13,61},
        {34,18,46,30,33,17,45,29},
        {10,58,6,54,9,57,5,53},
        {42,26,38,22,41,25,37,21}
    };

    int total = n * n;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int b;
            if (n == 2)      b = bayer2[y & 1][x & 1];
            else if (n == 4) b = bayer4[y & 3][x & 3];
            else             b = bayer8[y & 7][x & 7];
            int t = (b * 256 + total / 2) / total;
            out[y * w + x] = grey[y * w + x] > t ? 255 : 0;
        }
    }
}

/* ── Random dither ──────────────────────────────────────────── */

static void dither_random(const uint8_t *grey, int w, int h, uint8_t *out) {
    for (int i = 0; i < w * h; i++)
        out[i] = grey[i] > (int)(xorshift32() & 0xFF) ? 255 : 0;
}

/* ── Public dither dispatch ─────────────────────────────────── */

void effect_dither(const uint8_t *grey, int w, int h, uint8_t *out,
                   int method, int threshold) {
    switch (method) {
        case DITHER_FLOYD_STEINBERG: dither_fs(grey, w, h, out); break;
        case DITHER_ATKINSON:        dither_atkinson(grey, w, h, out); break;
        case DITHER_JARVIS:          dither_jarvis(grey, w, h, out); break;
        case DITHER_STUCKI:          dither_stucki(grey, w, h, out); break;
        case DITHER_BAYER2:          dither_bayer(grey, w, h, out, 2); break;
        case DITHER_BAYER4:          dither_bayer(grey, w, h, out, 4); break;
        case DITHER_BAYER8:          dither_bayer(grey, w, h, out, 8); break;
        case DITHER_RANDOM:          dither_random(grey, w, h, out); break;
        case DITHER_THRESHOLD:
            for (int i = 0; i < w * h; i++)
                out[i] = grey[i] > threshold ? 255 : 0;
            break;
        default: dither_fs(grey, w, h, out); break;
    }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

/* ── Halftone screen cache ──────────────────────────────────── */

#define MAX_SCREEN 16
static int screen_init = 0;
static uint8_t hscr[MAX_SCREEN + 1][MAX_SCREEN][MAX_SCREEN];

static void init_screens(void) {
    if (screen_init) return;
    screen_init = 1;
    for (int sz = 2; sz <= MAX_SCREEN; sz++) {
        float half = sz / 2.0f;
        float max_d = 0;
        float tmp[16][16];
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++) {
                float d = (x - half + 0.5f) * (x - half + 0.5f)
                        + (y - half + 0.5f) * (y - half + 0.5f);
                tmp[y][x] = d;
                if (d > max_d) max_d = d;
            }
        if (max_d < 0.001f) max_d = 1.0f;
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++)
                hscr[sz][y][x] = (uint8_t)(tmp[y][x] / max_d * 254.0f);
    }
}

/* ── Halftone (clustered-dot) ───────────────────────────────── */

void effect_halftone(const uint8_t *grey, int w, int h, uint8_t *out,
                     int dot_size, int thresh) {
    init_screens();
    int sz = max_i(dot_size, 2);
    if (sz > MAX_SCREEN) sz = MAX_SCREEN;
    float gamma = gamma_from_thresh(thresh);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t gv = (gamma == 1.0f) ? grey[y * w + x]
                         : apply_gamma(grey[y * w + x], gamma);
            out[y * w + x] = gv > hscr[sz][y % sz][x % sz] ? 255 : 0;
        }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

/* ── Halftone line ──────────────────────────────────────────── */

void effect_halftone_line(const uint8_t *grey, int w, int h, uint8_t *out,
                          int period, int angle, int thresh) {
    if (period < 4) period = 4;
    float rad = angle * 3.14159265f / 180.0f;
    float sa = sinf(rad), ca = cosf(rad);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d = x * sa + y * ca;
            float pos = d / period - floorf(d / period);
            if (pos < 0) pos += 1.0f;
            float dist = (pos > 0.5f) ? 1.0f - pos : pos;
            int screen = (int)((1.0f - 2.0f * dist) * 254.0f);
            if (screen < 0) screen = 0;
            out[y * w + x] = grey[y * w + x] > screen ? 255 : 0;
        }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
    (void)thresh; /* reserved for gamma control */
}

/* ── Halftone crosshatch ────────────────────────────────────── */

void effect_halftone_crosshatch(const uint8_t *grey, int w, int h, uint8_t *out,
                                int period, int thresh) {
    if (period < 4) period = 4;
    float r45 = 45 * 3.14159265f / 180.0f;
    float s45 = sinf(r45), c45 = cosf(r45);
    float s135 = sinf(-r45), c135 = cosf(-r45);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d1 = x * s45 + y * c45;
            float p1 = d1 / period - floorf(d1 / period);
            if (p1 < 0) p1 += 1.0f;
            float dist1 = (p1 > 0.5f) ? 1.0f - p1 : p1;
            int s1 = (int)((1.0f - 2.0f * dist1) * 254.0f);

            float d2 = x * s135 + y * c135;
            float p2 = d2 / period - floorf(d2 / period);
            if (p2 < 0) p2 += 1.0f;
            float dist2 = (p2 > 0.5f) ? 1.0f - p2 : p2;
            int s2 = (int)((1.0f - 2.0f * dist2) * 254.0f);

            int screen = (s1 < s2) ? s1 : s2;
            if (screen < 0) screen = 0;
            out[y * w + x] = grey[y * w + x] > screen ? 255 : 0;
        }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
    (void)thresh; /* reserved for gamma control */
}

/* ── Pixel art ──────────────────────────────────────────────── */

void effect_pixelate(const uint8_t *grey, int w, int h, uint8_t *out,
                     int block_size) {
    if (block_size < 2) block_size = 2;
    int cw = max_i(1, w / block_size);
    int ch = max_i(1, h / block_size);
    uint8_t *small = (uint8_t *)malloc(cw * ch);
    if (!small) return;

    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int sy = min_i(cy * block_size + block_size / 2, h - 1);
            int sx = min_i(cx * block_size + block_size / 2, w - 1);
            small[cy * cw + cx] = grey[sy * w + sx];
        }

    uint8_t *dithered = (uint8_t *)malloc(cw * ch);
    if (dithered) {
        dither_fs(small, cw, ch, dithered);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sy = min_i(y / block_size, ch - 1);
                int sx = min_i(x / block_size, cw - 1);
                out[y * w + x] = dithered[sy * cw + sx];
            }
        free(dithered);
    }
    free(small);
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

/* ── Embedded 8x8 bitmap font (IBM VGA, ASCII 32-126, public domain) ── */

static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},
    {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00},
    {0x18,0x3e,0x7c,0x3e,0x1e,0x1e,0x3e,0x18},
    {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00},
    {0x38,0x6c,0x6c,0x38,0x76,0xdc,0xcc,0x76},
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00},
    {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00},
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00},
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00},
    {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00},
    {0x3c,0x66,0x76,0x7e,0x6e,0x66,0x3c,0x00},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00},
    {0x3c,0x66,0x06,0x0c,0x30,0x60,0x7e,0x00},
    {0x3c,0x66,0x06,0x1c,0x06,0x66,0x3c,0x00},
    {0x0c,0x1c,0x3c,0x6c,0xfe,0x0c,0x0c,0x00},
    {0x7e,0x60,0x7c,0x06,0x06,0x66,0x3c,0x00},
    {0x3c,0x66,0x60,0x7c,0x66,0x66,0x3c,0x00},
    {0x7e,0x06,0x0c,0x18,0x30,0x30,0x30,0x00},
    {0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0x00},
    {0x3c,0x66,0x66,0x3e,0x06,0x66,0x3c,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    {0x06,0x0c,0x18,0x30,0x18,0x0c,0x06,0x00},
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00},
    {0x60,0x30,0x18,0x0c,0x18,0x30,0x60,0x00},
    {0x3c,0x66,0x06,0x0c,0x18,0x00,0x18,0x00},
    {0x3c,0x66,0x6e,0x7e,0x7e,0x60,0x3c,0x00},
    {0x3c,0x66,0x66,0x7e,0x66,0x66,0x66,0x00},
    {0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0x00},
    {0x3c,0x66,0x60,0x60,0x60,0x66,0x3c,0x00},
    {0x78,0x6c,0x66,0x66,0x66,0x6c,0x78,0x00},
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x7e,0x00},
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x60,0x00},
    {0x3c,0x66,0x60,0x6e,0x66,0x66,0x3e,0x00},
    {0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x00},
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x7e,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3c,0x00},
    {0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00},
    {0xc6,0xee,0xfe,0xfe,0xd6,0xc6,0xc6,0x00},
    {0x66,0x76,0x7e,0x7e,0x6e,0x66,0x66,0x00},
    {0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00},
    {0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0x00},
    {0x3c,0x66,0x66,0x66,0x6e,0x3c,0x07,0x00},
    {0x7c,0x66,0x66,0x7c,0x78,0x6c,0x66,0x00},
    {0x3c,0x66,0x60,0x3c,0x06,0x66,0x3c,0x00},
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3c,0x18,0x00},
    {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00},
    {0x66,0x66,0x3c,0x18,0x3c,0x66,0x66,0x00},
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x00},
    {0x7e,0x06,0x0c,0x18,0x30,0x60,0x7e,0x00},
    {0x7e,0x60,0x60,0x60,0x60,0x60,0x7e,0x00},
    {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00},
    {0x7e,0x06,0x06,0x06,0x06,0x06,0x7e,0x00},
    {0x18,0x3c,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff},
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3c,0x06,0x3e,0x66,0x3e,0x00},
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x7c,0x00},
    {0x00,0x00,0x3c,0x66,0x60,0x66,0x3c,0x00},
    {0x06,0x06,0x3e,0x66,0x66,0x66,0x3e,0x00},
    {0x00,0x00,0x3c,0x66,0x7e,0x60,0x3c,0x00},
    {0x1c,0x36,0x30,0x78,0x30,0x30,0x30,0x00},
    {0x00,0x3e,0x66,0x66,0x3e,0x06,0x3c,0x00},
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00},
    {0x0c,0x00,0x1c,0x0c,0x0c,0x0c,0x6c,0x38},
    {0x60,0x60,0x66,0x6c,0x78,0x6c,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00},
    {0x00,0x00,0xec,0xfe,0xd6,0xc6,0xc6,0x00},
    {0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x3c,0x66,0x66,0x66,0x3c,0x00},
    {0x00,0x00,0x7c,0x66,0x66,0x7c,0x60,0x60},
    {0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x06},
    {0x00,0x00,0x7c,0x66,0x60,0x60,0x60,0x00},
    {0x00,0x00,0x3e,0x60,0x3c,0x06,0x7c,0x00},
    {0x30,0x30,0x7c,0x30,0x30,0x34,0x18,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3e,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3c,0x18,0x00},
    {0x00,0x00,0xc6,0xc6,0xd6,0xfe,0x6c,0x00},
    {0x00,0x00,0x66,0x3c,0x18,0x3c,0x66,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3e,0x06,0x3c},
    {0x00,0x00,0x7e,0x0c,0x18,0x30,0x7e,0x00},
    {0x0c,0x18,0x18,0x70,0x18,0x18,0x0c,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x60,0x30,0x30,0x1c,0x30,0x30,0x60,0x00},
    {0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ── ASCII art ──────────────────────────────────────────────── */

void effect_ascii(const uint8_t *grey, int w, int h, uint8_t *out,
                  int cols, const char *charset) {
    if (cols < 8) cols = 8;
    int nchars = min_i((int)strlen(charset), 32);
    if (nchars < 1) nchars = 1;
    int rows = max_i(1, (h * cols) / w);
    int rw = cols * 8, rh = rows * 8;

    memset(out, 0, rw * rh);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int x0 = c * w / cols, x1 = (c + 1) * w / cols;
            int y0 = r * h / rows, y1 = (r + 1) * h / rows;
            if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            int sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    sum += grey[yy * w + xx]; cnt++;
                }
            int ci = (sum / cnt) * nchars / 256;
            if (ci >= nchars) ci = nchars - 1;
            int fidx = charset[ci] - 32;
            if (fidx < 0 || fidx > 94) fidx = 0;
            for (int gy = 0; gy < 8 && r * 8 + gy < rh; gy++) {
                uint8_t row = font8x8[fidx][gy];
                for (int gx = 0; gx < 8 && c * 8 + gx < rw; gx++)
                    if (row & (0x80 >> gx))
                        out[(r * 8 + gy) * rw + (c * 8 + gx)] = 255;
            }
        }
}

/* ── Binary art ─────────────────────────────────────────────── */

void effect_binary(const uint8_t *grey, int w, int h, uint8_t *out,
                   int cols) {
    if (cols < 8) cols = 8;
    int rows = max_i(1, (h * cols) / w);
    int rw = cols * 8, rh = rows * 8;

    int sum = 0;
    for (int i = 0; i < w * h; i++) sum += grey[i];
    int avg = sum / (w * h);
    int bg = avg > 200 ? 180 : max_i(avg, 100);

    memset(out, 0, rw * rh);
    static const char cset[] = ".,01";
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int sx = min_i(c * w / cols, w - 1);
            int sy = min_i(r * h / rows, h - 1);
            int pv = grey[sy * w + sx];
            char ch = ' ';
            if (pv <= bg) {
                int idx = (bg - pv) * 3 / bg;
                if (idx > 3) idx = 3;
                ch = cset[idx];
            }
            int fidx = ch - 32;
            if (fidx < 0 || fidx > 94) fidx = 0;
            for (int gy = 0; gy < 8 && r * 8 + gy < rh; gy++) {
                uint8_t row = font8x8[fidx][gy];
                for (int gx = 0; gx < 8 && c * 8 + gx < rw; gx++)
                    if (row & (0x80 >> gx))
                        out[(r * 8 + gy) * rw + (c * 8 + gx)] = 255;
            }
        }
}

/* ── Silhouette ─────────────────────────────────────────────── */

void effect_silhouette(const uint8_t *grey, int w, int h, uint8_t *out,
                       int threshold) {
    for (int i = 0; i < w * h; i++)
        out[i] = grey[i] > threshold ? 255 : 0;
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

/* ── Blocks texture ─────────────────────────────────────────── */

void effect_blocks(const uint8_t *grey, int w, int h, uint8_t *out,
                   int block_size, int levels) {
    if (block_size < 2) block_size = 2;
    if (block_size > 16) block_size = 16;
    if (levels < 2) levels = 2;
    if (levels > 8) levels = 8;
    int bs = block_size;

    uint8_t pat[8][256];
    int order[256], oi = 0;
    float cy = (bs - 1) / 2.0f, cx = (bs - 1) / 2.0f;
    for (int py = 0; py < bs; py++)
        for (int px = 0; px < bs; px++) {
            int d = (int)((py - cy) * (py - cy) + (px - cx) * (px - cx));
            order[oi++] = (d << 16) | (py << 8) | px;
        }
    for (int i = 1; i < oi; i++) {
        int k = order[i]; int j = i - 1;
        while (j >= 0 && (order[j] >> 16) > (k >> 16)) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }

    int total = bs * bs;
    for (int l = 0; l < levels; l++) {
        memset(pat[l], 0, bs * bs);
        int n = (l == levels - 1) ? total : (l * total) / (levels - 1);
        for (int i = 0; i < n && i < total; i++)
            pat[l][((order[i] >> 8) & 0xFF) * bs + (order[i] & 0xFF)] = 255;
    }

    int cw = (w + bs - 1) / bs, ch = (h + bs - 1) / bs;
    for (int gy = 0; gy < ch; gy++)
        for (int gx = 0; gx < cw; gx++) {
            int x0 = gx * w / cw, x1 = (gx + 1) * w / cw;
            int y0 = gy * h / ch, y1 = (gy + 1) * h / ch;
            if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            int sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    sum += grey[yy * w + xx]; cnt++;
                }
            int br = sum / cnt;
            int l = br * levels / 256;
            if (l >= levels) l = levels - 1;
            if (l < 0) l = 0;
            for (int py = 0; py < bs && gy * bs + py < h; py++)
                for (int px = 0; px < bs && gx * bs + px < w; px++)
                    out[(gy * bs + py) * w + (gx * bs + px)] = pat[l][py * bs + px];
        }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

/* ── Pre-processing ─────────────────────────────────────────── */

void preprocess_autocontrast(uint8_t *grey, int w, int h) {
    int hist[256] = {0};
    int n = w * h;
    for (int i = 0; i < n; i++) hist[grey[i]]++;

    int total = 0, lo = 0, hi = 255;
    int lo_target = n * 2 / 100;
    int hi_target = n * 98 / 100;
    if (lo_target < 1) lo_target = 1;
    if (hi_target >= n) hi_target = n - 1;

    for (int i = 0; i < 256; i++) {
        total += hist[i];
        if (total >= lo_target) { lo = i; break; }
    }
    total = 0;
    for (int i = 255; i >= 0; i--) {
        total += hist[i];
        if (total >= (n - hi_target)) { hi = i; break; }
    }

    if (hi <= lo + 4) { hi = lo + 4; if (hi > 255) hi = 255; }

    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (i - lo) * 255 / (hi - lo);
        if (v < 0) v = 0; if (v > 255) v = 255;
        lut[i] = v;
    }
    for (int i = 0; i < n; i++) grey[i] = lut[grey[i]];
}

void preprocess_gamma(uint8_t *grey, int w, int h, int thresh) {
    float g = 2.0f - thresh / 128.0f;
    if (g < 0.1f) g = 0.1f;
    if (g > 3.9f) g = 3.9f;

    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (int)(powf(i / 255.0f, g) * 255.0f + 0.5f);
        if (v < 0) v = 0; if (v > 255) v = 255;
        lut[i] = v;
    }
    int n = w * h;
    for (int i = 0; i < n; i++) grey[i] = lut[grey[i]];
}

void preprocess_sharpen(uint8_t *grey, int w, int h, float strength) {
    if (strength <= 0) return;
    uint8_t *tmp = (uint8_t *)malloc(w * h);
    if (!tmp) return;
    memcpy(tmp, grey, w * h);

    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++) {
            int center = tmp[y * w + x];
            int lap = center * 4
                    - tmp[(y - 1) * w + x]
                    - tmp[(y + 1) * w + x]
                    - tmp[y * w + x - 1]
                    - tmp[y * w + x + 1];
            int v = center + (int)(lap * strength * 0.5f);
            if (v < 0) v = 0; if (v > 255) v = 255;
            grey[y * w + x] = v;
        }
    free(tmp);
}

void preprocess_all(uint8_t *grey, int w, int h, int gamma_thresh) {
    preprocess_autocontrast(grey, w, h);
    preprocess_gamma(grey, w, h, gamma_thresh);
    preprocess_sharpen(grey, w, h, 0.8f);
}

/* ── Pixel replication (post-process output scaling) ────────── */

void apply_pixel_replication(uint8_t *bitmap, int w, int h, int factor) {
    if (factor <= 1) return;
    uint8_t *tmp = (uint8_t *)malloc(w * h);
    if (!tmp) return;
    memcpy(tmp, bitmap, w * h);
    for (int y = 0; y < h; y++) {
        int sy = (y / factor) * factor + factor / 2;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < w; x++) {
            int sx = (x / factor) * factor + factor / 2;
            if (sx >= w) sx = w - 1;
            bitmap[y * w + x] = tmp[sy * w + sx];
        }
    }
    free(tmp);
}

/* ── Bilinear resize ────────────────────────────────────────── */

void grey_resize(const uint8_t *src, int sw, int sh,
                 uint8_t *dst, int dw, int dh) {
    for (int dy = 0; dy < dh; dy++) {
        float syf = (dy + 0.5f) * sh / dh - 0.5f;
        if (syf < 0) syf = 0;
        int sy = (int)syf;
        if (sy >= sh - 1) sy = sh - 2;
        float fy = syf - sy;
        for (int dx = 0; dx < dw; dx++) {
            float sxf = (dx + 0.5f) * sw / dw - 0.5f;
            if (sxf < 0) sxf = 0;
            int sx = (int)sxf;
            if (sx >= sw - 1) sx = sw - 2;
            float fx = sxf - sx;
            int v00 = src[sy * sw + sx];
            int v10 = src[sy * sw + sx + 1];
            int v01 = src[(sy + 1) * sw + sx];
            int v11 = src[(sy + 1) * sw + sx + 1];
            float top = v00 + (v10 - v00) * fx;
            float bot = v01 + (v11 - v01) * fx;
            int v = (int)(top + (bot - top) * fy + 0.5f);
            if (v < 0) v = 0; if (v > 255) v = 255;
            dst[dy * dw + dx] = v;
        }
    }
}

/* ── Pack 8-bit → 1-bit MSB-first ───────────────────────────── */

void pack_1bit(const uint8_t *src, int w, int h, uint8_t *dst) {
    int wb = (w + 7) / 8;
    memset(dst, 0, wb * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (src[y * w + x] > 128)
                dst[y * wb + x / 8] |= 0x80 >> (x & 7);
}

/* ── Comic / Ben-Day dots ──────────────────────────────────── */

void effect_comic(const uint8_t *grey, int w, int h, uint8_t *out,
                  int dot_size, int levels) {
    if (dot_size < 2) dot_size = 2;
    if (dot_size > 16) dot_size = 16;
    if (levels < 2) levels = 2;
    if (levels > 8) levels = 8;
    int bs = dot_size;
    int half = bs / 2;
    float max_d2 = half * half + half * half;

    uint8_t pat[8][256];
    int order[256], oi = 0;
    float cy = (bs - 1) / 2.0f, cx = (bs - 1) / 2.0f;
    for (int py = 0; py < bs; py++)
        for (int px = 0; px < bs; px++) {
            int d = (int)((py - cy) * (py - cy) + (px - cx) * (px - cx));
            order[oi++] = (d << 16) | (py << 8) | px;
        }
    for (int i = 1; i < oi; i++) {
        int k = order[i]; int j = i - 1;
        while (j >= 0 && (order[j] >> 16) > (k >> 16)) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }

    int total = bs * bs;
    for (int l = 0; l < levels; l++) {
        memset(pat[l], 0, bs * bs);
        int n = (l == levels - 1) ? total : (l * total) / (levels - 1);
        for (int i = 0; i < n && i < total; i++)
            pat[l][((order[i] >> 8) & 0xFF) * bs + (order[i] & 0xFF)] = 255;
    }

    int cw = (w + bs - 1) / bs, ch = (h + bs - 1) / bs;
    for (int gy = 0; gy < ch; gy++)
        for (int gx = 0; gx < cw; gx++) {
            int x0 = gx * w / cw, x1 = (gx + 1) * w / cw;
            int y0 = gy * h / ch, y1 = (gy + 1) * h / ch;
            if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            int sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    sum += grey[yy * w + xx]; cnt++;
                }
            int br = sum / cnt;
            int l = br * levels / 256;
            if (l >= levels) l = levels - 1;
            if (l < 0) l = 0;
            for (int py = 0; py < bs && gy * bs + py < h; py++)
                for (int px = 0; px < bs && gx * bs + px < w; px++)
                    out[(gy * bs + py) * w + (gx * bs + px)] = pat[l][py * bs + px];
        }
    for (int i = 0; i < w * h; i++) out[i] = 255 - out[i];
}

