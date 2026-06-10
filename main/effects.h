#ifndef EFFECTS_H
#define EFFECTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dither methods */
#define DITHER_FLOYD_STEINBERG 0
#define DITHER_ATKINSON        1
#define DITHER_JARVIS          2
#define DITHER_STUCKI          3
#define DITHER_BAYER2          4
#define DITHER_BAYER4          5
#define DITHER_BAYER8          6
#define DITHER_RANDOM          7
#define DITHER_THRESHOLD       8

/*
 * Output: packed 1-bit (8 pixels/byte, MSB=leftmost pixel)
 * out_w_bytes = (width + 7) / 8
 * out_h = height
 */

/* Convert greyscale to 1-bit using dithering.
 * grey: 8-bit input (w x h)
 * out: packed 1-bit output buffer (must be >= ((w+7)/8) * h bytes)
 * method: one of DITHER_* constants
 * threshold: only used for DITHER_THRESHOLD
 */
void effect_dither(const uint8_t *grey, int w, int h, uint8_t *out,
                   int method, int threshold);

/* Halftone clustered-dot screen.
 * dot_size: 2-16 (diameter of dots)
 * thresh: gamma control 0-255 (128=neutral)
 */
void effect_halftone(const uint8_t *grey, int w, int h, uint8_t *out,
                     int dot_size, int thresh);

/* Halftone line screen.
 * period: 4-24 (pixel spacing)
 * angle: 0 (vertical), 90 (horizontal), 45, 135
 * thresh: unused (reserved)
 */
void effect_halftone_line(const uint8_t *grey, int w, int h, uint8_t *out,
                          int period, int angle, int thresh);

/* Halftone crosshatch (45 + 135 overlay). */
void effect_halftone_crosshatch(const uint8_t *grey, int w, int h, uint8_t *out,
                                int period, int thresh);

/* Pixel art: downsample then dither.
 * block_size: 2-32
 */
void effect_pixelate(const uint8_t *grey, int w, int h, uint8_t *out,
                     int block_size);

/* ASCII art using embedded 8-pixel-wide bitmap font.
 * cols: character columns across
 * charset: pointer to density-sorted character string (e.g. " .:-=+*#%@")
 */
void effect_ascii(const uint8_t *grey, int w, int h, uint8_t *out,
                  int cols, const char *charset);

/* Binary art: background = spaces, subject = 01 patterns */
void effect_binary(const uint8_t *grey, int w, int h, uint8_t *out,
                   int cols);

/* Comic / Ben-Day dots: posterise + dot overlay */
void effect_comic(const uint8_t *grey, int w, int h, uint8_t *out,
                  int dot_size, int levels);

/* Silhouette: simple threshold */
void effect_silhouette(const uint8_t *grey, int w, int h, uint8_t *out,
                       int threshold);

/* Blocks texture: tiled brightness patterns */
void effect_blocks(const uint8_t *grey, int w, int h, uint8_t *out,
                   int block_size, int levels);

/* Pixel replication: post-process to increase apparent pixel size.
 * Divides the bitmap into factor×factor blocks and fills each block
 * with the value of the block's center pixel.
 */
void apply_pixel_replication(uint8_t *bitmap, int w, int h, int factor);

/* ── Pre-processing ─────────────────────────────────────────── */

/* Auto-contrast stretch: remap 2nd-98th percentile to full range. */
void preprocess_autocontrast(uint8_t *grey, int w, int h);

/* Gamma correction: gamma derived from thresh (0-255, 128=1.0). */
void preprocess_gamma(uint8_t *grey, int w, int h, int thresh);

/* Unsharp mask: 3x3 Laplacian edge boost. strength 0.0-2.0 */
void preprocess_sharpen(uint8_t *grey, int w, int h, float strength);

/* Full pipeline: autocontrast + gamma + sharpen. One call. */
void preprocess_all(uint8_t *grey, int w, int h, int gamma_thresh);

/* Utility: resize greyscale image using bilinear interpolation.
 * dst buffer must be >= dst_w * dst_h bytes.
 */
void grey_resize(const uint8_t *src, int src_w, int src_h,
                 uint8_t *dst, int dst_w, int dst_h);

/* Utility: pack 1-bit buffer (8-bit thresholded pixels -> packed MSB-first).
 * src: thresholded 8-bit (255=white, 0=black) w x h
 * dst: packed output ((w+7)/8 * h bytes)
 */
void pack_1bit(const uint8_t *src, int w, int h, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* EFFECTS_H */
