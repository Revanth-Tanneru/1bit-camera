/*
 * Host-side test harness for Postcard Machine effects.
 *
 * Compile:
 *   cc -O2 -lm -o test_effects test_effects.c ../main/effects.c -Imain
 *
 * Usage:
 *   ./test_effects                        # test card + Floyd-Steinberg
 *   ./test_effects photo.pgm              # test card with your photo
 *   ./test_effects halftone               # test card with halftone
 *   ./test_effects photo.pgm ascii        # your photo, ASCII art
 *   ./test_effects photo.pgm halftone -d 6 -t 100  # tune params
 *   ./test_effects --all                  # generate ALL effects
 *   ./test_effects --all --open           # all effects + open folder
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>

#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(s, c) malloc(s)

#include "../main/effects.h"

/* ── Effect registry ─────────────────────────────────────────── */

#define FX_COUNT 18

struct FX {
    int id;
    const char *name;
    const char *label;
    int is_bitmap;
} fx_table[FX_COUNT] = {
    {0,  "fs",             "Floyd-Steinberg",  1},
    {1,  "atkinson",       "Atkinson",         1},
    {2,  "jarvis",         "Jarvis",           1},
    {3,  "stucki",         "Stucki",           1},
    {4,  "bayer2",         "Bayer 2x2",        1},
    {5,  "bayer4",         "Bayer 4x4",        1},
    {6,  "bayer8",         "Bayer 8x8",        1},
    {7,  "random",         "Random dither",    1},
    {8,  "threshold",      "Threshold",        1},
    {9,  "halftone",       "Halftone",         1},
    {10, "line",           "Line halftone",    1},
    {11, "crosshatch",     "Crosshatch",       1},
    {12, "pixelate",       "Pixelate",         1},
    {13, "ascii",          "ASCII art",        0},
    {14, "binary",         "Binary art",       0},
    {15, "comic",          "Comic",            1},
    {16, "silhouette",     "Silhouette",       1},
    {17, "blocks",         "Blocks",           1},
};

static struct FX *find_fx(const char *name) {
    for (int i = 0; i < FX_COUNT; i++)
        if (strcmp(name, fx_table[i].name) == 0) return &fx_table[i];
    return NULL;
}

/* ── PGM/PBM I/O ─────────────────────────────────────────────── */

static uint8_t *read_pgm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    char magic[3];
    if (fscanf(f, "%2s", magic) != 1 || magic[0] != 'P' || magic[1] != '5') {
        fprintf(stderr, "not PGM (P5)\n"); fclose(f); return NULL;
    }
    int maxv;
    if (fscanf(f, "%d %d %d", w, h, &maxv) != 3) {
        fprintf(stderr, "bad PGM header\n"); fclose(f); return NULL;
    }
    while (fgetc(f) != '\n');
    uint8_t *buf = malloc((*w) * (*h));
    if (!buf || (int)fread(buf, 1, (*w) * (*h), f) != (*w) * (*h)) {
        fprintf(stderr, "read fail\n"); free(buf); fclose(f); return NULL;
    }
    fclose(f);
    return buf;
}

/* Minimal 24-bit BMP reader (sips can output BMP on macOS) */
static uint8_t *read_bmp(const char *path, int *w, int *h) {
    unsigned char hdr[54];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f); return NULL;
    }
    int bpp = hdr[28];
    if (bpp != 24) { fclose(f); return NULL; }
    *w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
    int raw_h = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
    int flip = raw_h > 0;
    *h = flip ? raw_h : -raw_h;
    int stride = ((*w * 3) + 3) & ~3;
    unsigned char *row = malloc(stride);
    uint8_t *grey = malloc((*w) * (*h));
    if (!row || !grey) { free(row); free(grey); fclose(f); return NULL; }
    int off = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
    fseek(f, off, SEEK_SET);
    for (int yi = 0; yi < *h; yi++) {
        int y = flip ? *h - 1 - yi : yi;
        if ((int)fread(row, 1, stride, f) != stride) break;
        for (int x = 0; x < *w; x++)
            grey[y * (*w) + x] = (row[x*3+2]*77 + row[x*3+1]*150 + row[x*3]*29 + 128) / 256;
    }
    free(row); fclose(f);
    return grey;
}

/* Auto-convert any image to PGM via sips -> BMP */
static uint8_t *read_any(const char *path, int *w, int *h) {
    uint8_t *g;
    /* Try PGM directly (first check magic so we don't spam stderr) */
    FILE *check = fopen(path, "rb");
    char magic[3] = {0};
    if (check) { magic[0] = fgetc(check); magic[1] = fgetc(check); fclose(check); }
    if (magic[0] == 'P' && magic[1] == '5') {
        g = read_pgm(path, w, h);
        if (g) return g;
    }
    /* Convert via sips (macOS) */
    char bmp[1024];
    snprintf(bmp, sizeof(bmp), "/tmp/_fx_%d.bmp", getpid());
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "sips -s format bmp \"%s\" --out \"%s\" >/dev/null 2>&1", path, bmp);
    if (system(cmd) == 0) {
        g = read_bmp(bmp, w, h);
        remove(bmp);
        if (g) return g;
    }
    fprintf(stderr, "Can't read %s — convert to PGM (P5) first with:\n", path);
    fprintf(stderr, "  sips -s format bmp photo.jpg --out photo.bmp\n");
    fprintf(stderr, "  (then rename to .pgm and edit header)\n");
    return NULL;
}

static int write_pgm(const char *path, const uint8_t *pix, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(pix, 1, w * h, f);
    fclose(f);
    return 0;
}

static int write_pbm(const char *path, const uint8_t *pix, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P4\n%d %d\n", w, h);
    int wb = (w + 7) / 8;
    uint8_t *p = malloc(wb * h);
    if (!p) { fclose(f); return -1; }
    memset(p, 0, wb * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (pix[y * w + x] > 128)
                p[y * wb + x / 8] |= 0x80 >> (x & 7);
    fwrite(p, 1, wb * h, f);
    free(p);
    fclose(f);
    return 0;
}

/* Convert to PNG on macOS via sips (strips .pgm/.pbm extension) */
static int to_png(const char *path) {
    char stem[512];
    snprintf(stem, sizeof(stem), "%s", path);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "sips -s format png \"%s\" --out \"%s.png\" >/dev/null 2>&1", path, stem);
    return system(cmd);
}

/* ── Test pattern ────────────────────────────────────────────── */

static void gen_testcard(uint8_t *buf, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w, fy = (float)y / h;
            float cx = fx - 0.5f, cy = fy - 0.5f;
            float rad = sqrtf(cx * cx + cy * cy);
            float ang = atan2f(cy, cx);
            int v;
            if (rad < 0.05f) v = 255;
            else if (rad < 0.1f) v = 0;
            else if (rad < 0.25f) v = (int)((sinf(ang * 4) * 0.5f + 0.5f) * 255);
            else if (rad < 0.35f) v = (int)(fx * 255);
            else v = (int)(fy * 255);
            if (fabsf(cx + 0.15f) < 0.04f && fabsf(cy + 0.15f) < 0.04f) v = 0;
            if (fabsf(cx - 0.15f) < 0.04f && fabsf(cy + 0.15f) < 0.04f) v = 0;
            if (fabsf(cy - 0.15f) < 0.03f && fabsf(cx) < 0.08f) v = 0;
            if (v < 0) v = 0; if (v > 255) v = 255;
            buf[y * w + x] = v;
        }
}

/* ── Apply one effect ────────────────────────────────────────── */

static int apply_effect(int fx_id, const uint8_t *small, int w, int h,
                        uint8_t *bitmap, int *out_h,
                        int dot, int thresh, int levels,
                        int rev_charset) {
    int ph = h;
    *out_h = ph;
    memset(bitmap, 0, w * ph);
    switch (fx_id) {
        case 0:  effect_dither(small, w, ph, bitmap, 0, 0); break;
        case 1:  effect_dither(small, w, ph, bitmap, 1, 0); break;
        case 2:  effect_dither(small, w, ph, bitmap, 2, 0); break;
        case 3:  effect_dither(small, w, ph, bitmap, 3, 0); break;
        case 4:  effect_dither(small, w, ph, bitmap, 4, 0); break;
        case 5:  effect_dither(small, w, ph, bitmap, 5, 0); break;
        case 6:  effect_dither(small, w, ph, bitmap, 6, 0); break;
        case 7:  effect_dither(small, w, ph, bitmap, 7, 0); break;
        case 8:  effect_dither(small, w, ph, bitmap, 8, thresh); break;
        case 9:  effect_halftone(small, w, ph, bitmap, dot, thresh); break;
        case 10: effect_halftone_line(small, w, ph, bitmap, dot, 45, thresh); break;
        case 11: effect_halftone_crosshatch(small, w, ph, bitmap, dot, thresh); break;
        case 12: effect_pixelate(small, w, ph, bitmap, dot); break;
        case 13: {
            int cols = w / 8;
            const char *cs = rev_charset ? "@%#*+=-:. " : " .:-=+*#%@";
            effect_ascii(small, w, ph, bitmap, cols, cs);
            *out_h = cols ? (ph * cols / w) * 8 : ph;
            if (*out_h > ph) *out_h = ph;
            break;
        }
        case 14: {
            int cols = w / 8;
            effect_binary(small, w, ph, bitmap, cols);
            *out_h = cols ? (ph * cols / w) * 8 : ph;
            if (*out_h > ph) *out_h = ph;
            break;
        }
        case 15: effect_comic(small, w, ph, bitmap, dot, levels); break;
        case 16: effect_silhouette(small, w, ph, bitmap, thresh); break;
        case 17: effect_blocks(small, w, ph, bitmap, dot, levels); break;
        default: effect_dither(small, w, ph, bitmap, 0, 0); break;
    }
    return 0;
}

/* ── Process one effect → file ───────────────────────────────── */

static int process_one(const char *src_name, const uint8_t *grey_src,
                       int sw, int sh, int out_w,
                       int p_preproc, int p_thresh,
                       int dot, int thresh, int levels, int invert,
                       struct FX *fx, int open_result) {
    int out_h = sh * out_w / sw;
    uint8_t *small = malloc(out_w * out_h);
    uint8_t *bitmap = malloc(out_w * out_h);
    if (!small || !bitmap) { free(small); free(bitmap); return -1; }

    grey_resize(grey_src, sw, sh, small, out_w, out_h);
    if (p_preproc) preprocess_all(small, out_w, out_h, p_thresh);
    int rev_charset = 0;
    if (invert && fx->id == 13) rev_charset = 1;

    int rendered_h;

    /* ── Pixel replication (Dial 2 post-process) ────────────────
       For effects where p_dot controls output pixel size (not
       block/comic/dot internals), apply pixel replication here.
       For halftone/line/crosshatch, override their internal pattern
       size since p_dot now means pixel replication. */

    int eff_dot = dot;
    int do_rep = 1;
    switch (fx->id) {
        case 9:  /* halftone */
        case 10: /* line */
        case 11: /* crosshatch */
            eff_dot = (fx->id == 9) ? 4 : 8;
            do_rep = 1;
            break;
        case 12: /* pixelate - uses dot as block size internally */
        case 15: /* comic - uses dot as dot size internally */
        case 17: /* blocks - uses dot as block size internally */
            do_rep = 0;
            break;
        case 13: /* ASCII */
        case 14: /* Binary */
            do_rep = 0;
            break;
    }

    apply_effect(fx->id, small, out_w, out_h, bitmap, &rendered_h,
                 eff_dot, thresh, levels, rev_charset);

    if (do_rep && dot > 1)
        apply_pixel_replication(bitmap, out_w, rendered_h, dot);

    /* ── Pipeline ───────────────────────────────────────────────
       Step 1: Normalize all effects to printer convention (dark → 255=ink).
       Binary already uses this convention.
       Step 2: Manual invert (user toggle, mirrors firmware p_invert).
       Step 3: Flip for human-readable PNG preview (ink=0→black, paper=255→white). */

    /* Step 1: Normalize to printer convention (255=ink where image is dark) */
    int already_correct = (fx->id == 14);
    if (!already_correct)
        for (int i = 0; i < out_w * rendered_h; i++)
            bitmap[i] = 255 - bitmap[i];

    /* Step 2: Manual invert (user toggle) */
    if (invert)
        for (int i = 0; i < out_w * rendered_h; i++)
            bitmap[i] = 255 - bitmap[i];

    /* Step 3: Flip for human-readable PNG preview */
    for (int i = 0; i < out_w * rendered_h; i++)
        bitmap[i] = 255 - bitmap[i];

    char base[256];
    if (src_name)
        snprintf(base, sizeof(base), "fx_%s_%s", src_name, fx->name);
    else
        snprintf(base, sizeof(base), "fx_%s", fx->name);

    char path[512];
    if (fx->is_bitmap)
        snprintf(path, sizeof(path), "%s.pbm", base);
    else
        snprintf(path, sizeof(path), "%s.pgm", base);

    int ok = fx->is_bitmap ? write_pbm(path, bitmap, out_w, rendered_h)
                           : write_pgm(path, bitmap, out_w, rendered_h);

    if (ok == 0) {
        /* Convert to PNG on macOS */
        char png_path[512];
        snprintf(png_path, sizeof(png_path), "%s.png", base);
        to_png(path);
        printf("  %-20s  %s%s\n", fx->label, png_path,
               open_result ? "  (opening...)" : "");
        if (open_result) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "open \"%s\"", png_path);
            system(cmd);
        }
    }

    free(small); free(bitmap);
    return ok;
}

/* ── Help ────────────────────────────────────────────────────── */

static void list_effects(void) {
    printf("Available effects:\n");
    for (int i = 0; i < FX_COUNT; i++)
        printf("  %-14s  %s\n", fx_table[i].name, fx_table[i].label);
}

static void print_help(const char *prog) {
    printf("Usage: %s [input] [effect] [options]\n", prog);
    printf("\n");
    printf("Arguments:\n");
    printf("  input          PGM image file (omit for built-in test card)\n");
    printf("  effect         Effect name (omit for Floyd-Steinberg)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -d <n>         Dot / block size  (2-16, default: 3)\n");
    printf("  -t <n>         Threshold / gamma  (0-255, default: 128)\n");
    printf("  -l <n>         Levels  (2-6, default: 3)\n");
    printf("  -i             Invert output\n");
    printf("  -p             Skip pre-processing\n");
    printf("  -W <n>         Output width in pixels (default: 384)\n");
    printf("  -a, --all      Generate all effects\n");
    printf("  --ht           Halftone grid: 7 thresholds (32-224)\n");
    printf("  --open         Open result with default image viewer\n");
    printf("  --list         List all available effects\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                         test card, Floyd-Steinberg\n", prog);
    printf("  %s halftone                test card, halftone\n", prog);
    printf("  %s photo.pgm               your photo, Floyd-Steinberg\n", prog);
    printf("  %s photo.pgm ascii         your photo, ASCII art\n", prog);
    printf("  %s photo.pgm halftone -d 6 -t 100   tune it\n", prog);
    printf("  %s -a                      all effects from test card\n", prog);
    printf("  %s -a --open               all effects, open results\n", prog);
    printf("  %s --list                  show effects\n", prog);
    printf("\n");
    list_effects();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    { /* chdir to the directory containing the binary so output files are predictable */
        const char *slash = strrchr(argv[0], '/');
        if (slash) {
            char dir[1024];
            size_t len = slash - argv[0];
            if (len > 0) {
                snprintf(dir, sizeof(dir), "%.*s", (int)len, argv[0]);
                chdir(dir);
            }
        }
    }
    const char *inpath = NULL;
    const char *fx_name = NULL;
    int dot = 3, thresh = 128, levels = 3, invert = 0, preproc = 1;
    int out_w = 384;
    int do_all = 0, do_open = 0, do_ht_grid = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]); return 0;
        }
        if (strcmp(argv[i], "--list") == 0) {
            list_effects(); return 0;
        }
        if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-a") == 0) {
            do_all = 1; continue;
        }
        if (strcmp(argv[i], "--open") == 0) { do_open = 1; continue; }
        if (strcmp(argv[i], "--ht") == 0 || strcmp(argv[i], "--halftone-test") == 0) {
            do_ht_grid = 1; continue;
        }
        if (strcmp(argv[i], "-d") == 0 && i+1 < argc) { dot = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "-t") == 0 && i+1 < argc) { thresh = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "-l") == 0 && i+1 < argc) { levels = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "-W") == 0 && i+1 < argc) { out_w = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "-i") == 0) { invert = 1; continue; }
        if (strcmp(argv[i], "-p") == 0) { preproc = 0; continue; }
        if (argv[i][0] != '-') {
            if (!inpath && !fx_name) {
                /* First positional: try file, then effect name */
                FILE *f = fopen(argv[i], "rb");
                if (f) { fclose(f); inpath = argv[i]; continue; }
                struct FX *m = find_fx(argv[i]);
                if (m) { fx_name = argv[i]; continue; }
                fprintf(stderr, "Not a file or effect: %s\n", argv[i]);
                return 1;
            }
            if (!fx_name) {
                struct FX *m = find_fx(argv[i]);
                if (m) { fx_name = argv[i]; continue; }
                fprintf(stderr, "Unknown effect: %s\n", argv[i]);
                return 1;
            }
            fprintf(stderr, "Unexpected: %s\n", argv[i]);
            return 1;
        }
        fprintf(stderr, "Unknown flag: %s\n", argv[i]);
        return 1;
    }

    /* Load or create input */
    int sw, sh;
    uint8_t *grey;
    if (inpath) {
        grey = read_any(inpath, &sw, &sh);
        if (!grey) return 1;
    } else {
        sw = out_w; sh = out_w * 480 / 640;
        grey = malloc(sw * sh);
        if (!grey) { fprintf(stderr, "OOM\n"); return 1; }
        gen_testcard(grey, sw, sh);
        write_pgm("testcard.pgm", grey, sw, sh);
        to_png("testcard.pgm");
        printf("Built-in test card saved as testcard.png\n");
    }

    char src_tag[64];
    if (inpath) {
        const char *base = strrchr(inpath, '/');
        snprintf(src_tag, sizeof(src_tag), "%.40s",
                 base ? base + 1 : inpath);
        char *dotp = strchr(src_tag, '.');
        if (dotp) *dotp = 0;
    } else {
        snprintf(src_tag, sizeof(src_tag), "testcard");
    }

    /* --all: generate every effect */
    if (do_all) {
        printf("Generating all effects (src=%s, W=%d):\n", src_tag, out_w);
        for (int i = 0; i < FX_COUNT; i++) {
            process_one(src_tag, grey, sw, sh, out_w,
                        preproc, thresh, dot, thresh, levels, invert,
                        &fx_table[i], 0);
        }
        printf("\nDone.  Use `open fx_*.png` to view.\n");
        if (do_open) system("open .");
        free(grey);
        return 0;
    }

    /* --halftone-test: generate halftone at multiple thresholds */
    if (do_ht_grid) {
        int ht_thresh[] = {32, 64, 96, 128, 160, 192, 224};
        int n = sizeof(ht_thresh) / sizeof(ht_thresh[0]);
        printf("Halftone threshold grid (src=%s, W=%d, dot=%d):\n", src_tag, out_w, dot);
        for (int i = 0; i < n; i++) {
            char tag[128];
            snprintf(tag, sizeof(tag), "%s_ht%d", src_tag, ht_thresh[i]);
            process_one(tag, grey, sw, sh, out_w,
                        preproc, ht_thresh[i], dot, ht_thresh[i], levels, invert,
                        &fx_table[9], 0);
        }
        printf("\nDone.\n");
        if (do_open) system("open .");
        free(grey);
        return 0;
    }

    /* Single effect */
    struct FX *fx = NULL;
    if (fx_name) {
        fx = find_fx(fx_name);
        if (!fx) {
            fprintf(stderr, "Unknown effect '%s'. Use --list to see available.\n", fx_name);
            free(grey);
            return 1;
        }
    } else {
        fx = &fx_table[0]; /* default: Floyd-Steinberg */
    }

    printf("Effect: %s  |  dot=%d  thresh=%d  levels=%d  invert=%d  preproc=%d\n",
           fx->label, dot, thresh, levels, invert, preproc);
    printf("Input:  %s (%dx%d)  →  width=%d\n", src_tag, sw, sh, out_w);

    process_one(src_tag, grey, sw, sh, out_w,
                preproc, thresh, dot, thresh, levels, invert,
                fx, do_open);

    free(grey);
    return 0;
}
