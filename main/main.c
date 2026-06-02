/*
 * 1 Bit Camera — ESP32-CAM Firmware
 *
 * Capture → greyscale → resize → effect → pack → BT SPP → thermal printer
 *
 * ESP-IDF: v4.4 or v5.x (see sdkconfig.defaults for required Kconfig options)
 *
 * Hardware: ESP32-CAM (Ai-Thinker, OV5640)
 * Printer:  57mm thermal, ESC/POS over BT SPP, 384 dots wide
 * Button:   GPIO 0 (BOOT button) — tap = cycle effect,
 *           hold 1 s = capture still, hold 3 s = enter video standby
 * Display:  SSD1306 128×64 OLED, I2C: SDA=GPIO13, SCL=GPIO14,
 *           VCC=3.3V, GND=GND
 * Console:  UART at 115200 baud
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_camera.h"
#include "esp_task_wdt.h"

#include "effects.h"
#include "display.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const char *TAG = "1BITCAM";

/* ── Configuration ──────────────────────────────────────────── */

#define PRINT_W             384
#define CAP_W               640
#define CAP_H               480

enum {
    FX_DITHER_FS,
    FX_DITHER_ATK,
    FX_JARVIS,
    FX_STUCKI,
    FX_BAYER2,
    FX_BAYER4,
    FX_BAYER8,
    FX_RANDOM,
    FX_THRESHOLD,
    FX_HALFTONE,
    FX_LINE,
    FX_CROSS,
    FX_PIXELATE,
    FX_ASCII,
    FX_BINARY,
    FX_COMIC,
    FX_SILHOUETTE,
    FX_BLOCKS,
    FX_COUNT
};

static const char *effect_names[FX_COUNT] = {
    "FS dither",     "Atkinson",    "Jarvis",       "Stucki",
    "Bayer 2x2",     "Bayer 4x4",   "Bayer 8x8",    "Random",
    "Threshold",     "Halftone",    "Line halftn",  "Crosshatch",
    "Pixelate",      "ASCII art",   "Binary art",   "Comic",
    "Silhouette",    "Blocks",
};

#define BTN_GPIO            0
#define DIAL_TH_GPIO        33
#define DIAL_TH_CH          ADC1_CHANNEL_5
#define DIAL_SZ_GPIO        2
#define DIAL_SZ_CH          ADC2_CHANNEL_2

/* SD card (Ai-Thinker ESP32-CAM hardwired pins, shared with OLED/Dial 2) */
#define SD_CS   13
#define SD_MOSI 15
#define SD_MISO 2
#define SD_CLK  14

/* Camera pins — Ai-Thinker ESP32-CAM (OV2640/OV5640 same pinout) */
static const camera_config_t cam_cfg = {
    .pin_pwdn  = 32,
    .pin_reset = -1,
    .pin_xclk = 4,
    .pin_sscb_sda = 26,
    .pin_sscb_scl = 27,
    .pin_d7 = 35,
    .pin_d6 = 34,
    .pin_d5 = 39,
    .pin_d4 = 36,
    .pin_d3 = 21,
    .pin_d2 = 19,
    .pin_d1 = 18,
    .pin_d0 = 5,
    .pin_vsync = 25,
    .pin_href = 23,
    .pin_pclk = 22,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_GRAYSCALE,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY
};

/* ── State ──────────────────────────────────────────────────── */

static int effect = FX_DITHER_FS;
static int p_dot = 3;
static int p_thresh = 128;
static int p_levels = 3;
static int p_invert = 0;
static int p_preproc = 1;
static int p_video_frames = 25;
/* GPIO 33 = mode selector (ADC zones), GPIO 2 = value pot */
#define DIALMODE_THRESH  0
#define DIALMODE_SIZE    1
#define DIALMODE_LEVELS  2
static int dial_mode = DIALMODE_THRESH;
static const char *dial_mode_names[] = {"thresh", "size", "lvl"};
static int dial_mode_last = -1;
static int dial_val_last = -1;

/* PSRAM buffers (allocated once on first capture) */
static uint8_t *grey   = NULL;
static uint8_t *small  = NULL;
static uint8_t *bitmap = NULL;
static uint8_t *packed = NULL;
static int buf_h = 0;

/* ── SD card ────────────────────────────────────────────────── */

static sdmmc_card_t *sd_card = NULL;

static int sd_mount(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 8192,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    display_deinit();
    esp_err_t e = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init fail: %d", e); return -1;
    }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SD_CS;
    dev.host_id = host.slot;
    e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dev, &mount, &sd_card);
    if (e != ESP_OK) {
        spi_bus_free(host.slot);
        ESP_LOGE(TAG, "SD mount fail: %d", e); return -1;
    }
    return 0;
}

static void sd_unmount(void) {
    if (!sd_card) return;
    esp_vfs_fat_sdmmc_unmount();
    sd_card = NULL;
    spi_bus_free(SDSPI_HOST_DEFAULT().slot);
    display_init();
}

/* ── Bluetooth SPP ──────────────────────────────────────────── */

static uint32_t spp_handle = 0;
static volatile int spp_ok = 0;
static int bt_active = 0;

static void spp_cb(esp_spp_cb_event_t ev, esp_spp_cb_param_t *p) {
    switch (ev) {
        case ESP_SPP_SRV_OPEN_EVT:
            spp_handle = p->srv_open.handle;
            spp_ok = 1;
            ESP_LOGI(TAG, "BT connected");
            break;
        case ESP_SPP_CLOSE_EVT:
            spp_ok = 0;
            ESP_LOGI(TAG, "BT disconnected");
            break;
        default: break;
    }
}

static int bt_init(void) {
    esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t e;
    e = esp_bt_controller_init(&bt);
    if (e) { ESP_LOGE(TAG, "BT ctrl init fail: %d", e); return -1; }
    e = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (e) { ESP_LOGE(TAG, "BT enable fail: %d", e); return -1; }
    e = esp_bluedroid_init();
    if (e) { ESP_LOGE(TAG, "Bluedroid init fail: %d", e); return -1; }
    e = esp_bluedroid_enable();
    if (e) { ESP_LOGE(TAG, "Bluedroid enable fail: %d", e); return -1; }
    e = esp_spp_register_callback(spp_cb);
    if (e) { ESP_LOGE(TAG, "SPP reg fail: %d", e); return -1; }
    esp_spp_cfg_t cfg = { .mode = ESP_SPP_MODE_CB, .enable_lrc = false, .tx_buffer_size = 512 };
    e = esp_spp_enhanced_init(&cfg);
    if (e) { ESP_LOGE(TAG, "SPP init fail: %d", e); return -1; }
    e = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "1BitCamera");
    if (e) { ESP_LOGE(TAG, "SPP srv start fail: %d", e); return -1; }
    bt_active = 1;
    return 0;
}

/* ── ESC/POS printer ────────────────────────────────────────── */

static void pr_cmd(const uint8_t *data, int len) {
    if (spp_ok) esp_spp_write(spp_handle, len, data);
}

static void pr_init(void) {
    uint8_t reset[] = {0x1B, 0x40};
    pr_cmd(reset, 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t spacing[] = {0x1B, 0x32};
    pr_cmd(spacing, 2);
}

static void pr_bitmap(const uint8_t *pix, int w, int h) {
    int wb = (w + 7) / 8;
    uint8_t hdr[] = {
        0x1D, 0x76, 0x30, 0x00,
        (uint8_t)(wb & 0xFF), (uint8_t)((wb >> 8) & 0xFF),
        (uint8_t)(h & 0xFF), (uint8_t)((h >> 8) & 0xFF)
    };
    pr_cmd(hdr, 8);
    for (int y = 0; y < h; y++) {
        pr_cmd(&pix[y * wb], wb);
        if ((y & 0x1F) == 0) esp_task_wdt_reset();
        taskYIELD();
    }
    uint8_t feed[] = {0x1B, 0x64, 0x05};
    pr_cmd(feed, 3);
}

/* ── Camera ─────────────────────────────────────────────────── */

static esp_err_t cam_init(void) {
    esp_err_t e = esp_camera_init(&cam_cfg);
    if (e != ESP_OK) return e;
    sensor_t *s = esp_camera_sensor_get();
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_hmirror(s, 1);
    s->set_vflip(s, 1);
    return ESP_OK;
}

/* ── Display helpers ────────────────────────────────────────── */

static void update_display(void) {
    char buf[17];
    display_clear();

    snprintf(buf, 17, "%-16s", effect_names[effect]);
    display_text(0, 0, buf);

    switch (dial_mode) {
        case DIALMODE_THRESH:
            snprintf(buf, 17, "th=%-3d       ", p_thresh);
            break;
        case DIALMODE_LEVELS:
            snprintf(buf, 17, "th=%-3d lv=%d", p_thresh, p_levels);
            break;
        default: /* SIZE */
            snprintf(buf, 17, "th=%-3d sz=%d", p_thresh, p_dot);
            break;
    }
    display_text(1, 0, buf);

    snprintf(buf, 17, "%s %s inv=%d",
             spp_ok ? "BT" : "NoBT", dial_mode_names[dial_mode], p_invert);
    display_text(2, 0, buf);

    display_text(3, 0, "Ready");
    display_show();
}

static void show_status(const char *s) {
    char buf[17];
    display_clear();
    snprintf(buf, 17, "%-16s", s);
    display_text(3, 0, buf);
    display_show();
}

/* ── Capture + effect + print ───────────────────────────────── */

/* Evaluate 1-bit bitmap (dark=0, light=255) for print quality.
   Rewards: high edge count (detail) and 10-50% ink coverage. */
static int eval_quality(const uint8_t *bitmap, int w, int h) {
    int n = w * h;
    int ink = 0, edges = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int p = y * w + x;
            if (bitmap[p] < 128) ink++;
            if (x > 0 && (bitmap[p] < 128) != (bitmap[p - 1] < 128)) edges++;
            if (y > 0 && (bitmap[p] < 128) != (bitmap[p - w] < 128)) edges++;
        }
    float cov = (float)ink / n;
    float cov_score;
    if (cov < 0.05f || cov > 0.80f) cov_score = 0.0f;
    else if (cov < 0.10f) cov_score = (cov - 0.05f) / 0.05f;
    else if (cov > 0.60f) cov_score = (0.80f - cov) / 0.20f;
    else cov_score = 1.0f;
    return (int)(edges * cov_score);
}

/* Reusable effect dispatch — explicit params, returns output height. */
static int run_effect(uint8_t *small, int w, int h, uint8_t *bitmap,
                       int dot, int thresh, int levels, int invert) {
    int out_h = h;

    int eff_dot = dot;
    int do_rep = 1;
    switch (effect) {
        case FX_HALFTONE: eff_dot = 4;  break;
        case FX_LINE:
        case FX_CROSS:    eff_dot = 8;  break;
        case FX_PIXELATE:
        case FX_COMIC:
        case FX_BLOCKS:   do_rep = 0;   break;
        case FX_ASCII:
        case FX_BINARY:   do_rep = 0;   break;
    }

    memset(bitmap, 0, w * h);
    switch (effect) {
        case FX_DITHER_FS:
            effect_dither(small, w, h, bitmap, DITHER_FLOYD_STEINBERG, 0); break;
        case FX_DITHER_ATK:
            effect_dither(small, w, h, bitmap, DITHER_ATKINSON, 0); break;
        case FX_JARVIS:
            effect_dither(small, w, h, bitmap, DITHER_JARVIS, 0); break;
        case FX_STUCKI:
            effect_dither(small, w, h, bitmap, DITHER_STUCKI, 0); break;
        case FX_BAYER2:
            effect_dither(small, w, h, bitmap, DITHER_BAYER2, 0); break;
        case FX_BAYER4:
            effect_dither(small, w, h, bitmap, DITHER_BAYER4, 0); break;
        case FX_BAYER8:
            effect_dither(small, w, h, bitmap, DITHER_BAYER8, 0); break;
        case FX_RANDOM:
            effect_dither(small, w, h, bitmap, DITHER_RANDOM, 0); break;
        case FX_THRESHOLD:
            effect_dither(small, w, h, bitmap, DITHER_THRESHOLD, thresh); break;
        case FX_HALFTONE:
            effect_halftone(small, w, h, bitmap, eff_dot, thresh); break;
        case FX_LINE:
            effect_halftone_line(small, w, h, bitmap, eff_dot, 45, thresh); break;
        case FX_CROSS:
            effect_halftone_crosshatch(small, w, h, bitmap, eff_dot, thresh); break;
        case FX_PIXELATE:
            effect_pixelate(small, w, h, bitmap, dot); break;
        case FX_ASCII: {
            int cols = w / 8;
            const char *cs = invert ? "@%#*+=-:. " : " .:-=+*#%@";
            effect_ascii(small, w, h, bitmap, cols, cs);
            out_h = cols ? (h * cols / w) * 8 : h;
            if (out_h > h) out_h = h;
            break;
        }
        case FX_BINARY: {
            int cols = w / 8;
            effect_binary(small, w, h, bitmap, cols);
            out_h = cols ? (h * cols / w) * 8 : h;
            if (out_h > h) out_h = h;
            break;
        }
        case FX_COMIC:
            effect_comic(small, w, h, bitmap, dot, levels); break;
        case FX_SILHOUETTE:
            effect_silhouette(small, w, h, bitmap, thresh); break;
        case FX_BLOCKS:
            effect_blocks(small, w, h, bitmap, dot, levels); break;
        default:
            effect_dither(small, w, h, bitmap, DITHER_FLOYD_STEINBERG, 0); break;
    }

    if (do_rep && dot > 1)
        apply_pixel_replication(bitmap, w, out_h, dot);

    if (effect != FX_BINARY)
        for (int i = 0; i < w * out_h; i++) bitmap[i] = 255 - bitmap[i];
    if (invert)
        for (int i = 0; i < w * out_h; i++) bitmap[i] = 255 - bitmap[i];

    return out_h;
}

static void do_capture(void) {
    esp_task_wdt_reset();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { ESP_LOGE(TAG, "capture fail"); return; }

    if (!grey) {
        grey  = heap_caps_malloc(CAP_W * CAP_H, MALLOC_CAP_SPIRAM);
        buf_h = CAP_H * PRINT_W / CAP_W;
        small = heap_caps_malloc(PRINT_W * buf_h, MALLOC_CAP_SPIRAM);
        bitmap= heap_caps_malloc(PRINT_W * buf_h, MALLOC_CAP_SPIRAM);
        int pb = ((PRINT_W + 7) / 8) * buf_h;
        packed= heap_caps_malloc(pb, MALLOC_CAP_SPIRAM);
    }

    memcpy(grey, fb->buf, CAP_W * CAP_H);
    esp_camera_fb_return(fb);

    int ph = buf_h;

    /* ── Process ──────────────────────────────────────────── */
    grey_resize(grey, CAP_W, CAP_H, small, PRINT_W, ph);
    if (p_preproc) preprocess_all(small, PRINT_W, ph, p_thresh);
    int out_h = run_effect(small, PRINT_W, ph, bitmap,
                           p_dot, p_thresh, p_levels, p_invert);

    pack_1bit(bitmap, PRINT_W, out_h, packed);
    esp_task_wdt_reset();

    /* ── Save to SD card ──────────────────────────────────── */
    show_status("SD save...");
    if (sd_mount() == 0) {
        mkdir("/sdcard/capture", 0755);
        char path[40];
        static int seq = 0;
        snprintf(path, sizeof(path), "/sdcard/capture/PC%05d.pbm", ++seq);
        FILE *f = fopen(path, "wb");
        if (f) {
            int wb = (PRINT_W + 7) / 8;
            fprintf(f, "P4\n%d %d\n", PRINT_W, out_h);
            for (int y = 0; y < out_h; y++) {
                uint8_t buf[48];
                for (int x = 0; x < wb; x++)
                    buf[x] = ~packed[y * wb + x];
                fwrite(buf, 1, wb, f);
            }
            fclose(f);
            ESP_LOGI(TAG, "saved %s", path);
        }
        sd_unmount();
    }
    if (!bt_active) {
        adc2_config_channel_atten(ADC2_CHANNEL_2, ADC_ATTEN_DB_11);
    }

    /* ── Print via BT ──────────────────────────────────────── */
    show_status("Printing...");
    if (!spp_ok) { ESP_LOGE(TAG, "No BT"); return; }

    pr_init();
    pr_bitmap(packed, PRINT_W, out_h);
    ESP_LOGI(TAG, "printed %dx%d", PRINT_W, out_h);
}

/* ── Video flipbook ─────────────────────────────────────────── */

static void do_video(void) {
    int count = p_video_frames;
    if (count < 2) count = 2;
    if (count > 60) count = 60;

    int wb = (PRINT_W + 7) / 8;
    esp_task_wdt_reset();
    if (!grey) {
        grey  = heap_caps_malloc(CAP_W * CAP_H, MALLOC_CAP_SPIRAM);
        buf_h = CAP_H * PRINT_W / CAP_W;
        small = heap_caps_malloc(PRINT_W * buf_h, MALLOC_CAP_SPIRAM);
        bitmap= heap_caps_malloc(PRINT_W * buf_h, MALLOC_CAP_SPIRAM);
    }

    uint8_t **frames = malloc(count * sizeof(uint8_t *));
    int *heights = malloc(count * sizeof(int));
    if (!frames || !heights) { ESP_LOGE(TAG, "video arr fail"); return; }

    int ph = buf_h, fcount = 0;
    for (int i = 0; i < count; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { ESP_LOGE(TAG, "frame %d fail", i); break; }
        memcpy(grey, fb->buf, CAP_W * CAP_H);
        esp_camera_fb_return(fb);
        grey_resize(grey, CAP_W, CAP_H, small, PRINT_W, ph);
        if (p_preproc) preprocess_all(small, PRINT_W, ph, p_thresh);
        int out_h = run_effect(small, PRINT_W, ph, bitmap,
                               p_dot, p_thresh, p_levels, p_invert);
        uint8_t *p = heap_caps_malloc(wb * out_h, MALLOC_CAP_SPIRAM);
        if (!p) { ESP_LOGE(TAG, "frame %d alloc", i); break; }
        pack_1bit(bitmap, PRINT_W, out_h, p);
        frames[fcount] = p;
        heights[fcount] = out_h;
        fcount++;

        char buf[17];
        display_clear();
        snprintf(buf, 17, "%-16s", effect_names[effect]);
        display_text(0, 0, buf);
        snprintf(buf, 17, "Video %d/%d", i + 1, count);
        display_text(1, 0, buf);
        display_show();
        esp_task_wdt_reset();
    }

    ESP_LOGI(TAG, "video: %d frames", fcount);

    /* ── Save to SD card ──────────────────────────────────── */
    if (fcount > 0) {
        show_status("SD save...");
        if (sd_mount() == 0) {
            mkdir("/sdcard/video", 0755);
            static int vseq = 0;
            for (int i = 0; i < fcount; i++) {
                char path[48];
                snprintf(path, sizeof(path), "/sdcard/video/VD%05d_%02d.pbm", ++vseq, i + 1);
                FILE *f = fopen(path, "wb");
                if (f) {
                    int wb = (PRINT_W + 7) / 8;
                    int h = heights[i];
                    fprintf(f, "P4\n%d %d\n", PRINT_W, h);
                    for (int y = 0; y < h; y++) {
                        uint8_t buf[48];
                        for (int x = 0; x < wb; x++)
                            buf[x] = ~frames[i][y * wb + x];
                        fwrite(buf, 1, wb, f);
                    }
                    fclose(f);
                }
            }
            sd_unmount();
        }
    }

    if (fcount > 0 && spp_ok) {
        show_status("Printing...");
        pr_init();
        for (int i = 0; i < fcount; i++) {
            pr_bitmap(frames[i], PRINT_W, heights[i]);
            uint8_t gap[] = {0x1B, 0x64, 0x03};
            pr_cmd(gap, 3);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        ESP_LOGI(TAG, "video printed %d frames", fcount);
    }

    for (int i = 0; i < fcount; i++) free(frames[i]);
    free(frames);
    free(heights);
}

/* ── Button (polling, no ISR) ───────────────────────────────── */
/* Returns 0 = not pressed, 1–999 = short (cycle), 1000–2999 = long (capture),
   ≥3000 = extra-long (video). */

static int read_button(void) {
    if (gpio_get_level(BTN_GPIO) != 0) return 0;
    vTaskDelay(pdMS_TO_TICKS(30));
    if (gpio_get_level(BTN_GPIO) != 0) return 0;

    int ms = 30;
    while (gpio_get_level(BTN_GPIO) == 0 && ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ms += 10;
    }
    return ms;
}

/* ── Console ────────────────────────────────────────────────── */

static void console_cmd(const char *s) {
    int v;
    if      (sscanf(s, "effect %d", &v) == 1 && v >= 0 && v < FX_COUNT) {
        effect = v; printf("fx=%d %s\n", v, effect_names[v]); update_display();
    }
    else if (sscanf(s, "dot %d", &v) == 1 && v >= 2 && v <= 16) {
        p_dot = v; printf("dot=%d\n", v); update_display();
    }
    else if (sscanf(s, "thresh %d", &v) == 1 && v >= 0 && v <= 255) {
        p_thresh = v; printf("t=%d\n", v); update_display();
    }
    else if (sscanf(s, "levels %d", &v) == 1 && v >= 2 && v <= 6) {
        p_levels = v; printf("lvl=%d\n", v); update_display();
    }
    else if (strcmp(s, "invert") == 0) {
        p_invert = !p_invert; printf("inv=%d\n", p_invert); update_display();
    }
    else if (strcmp(s, "preproc") == 0) {
        p_preproc = !p_preproc; printf("pre=%d\n", p_preproc); update_display();
    }
    else if (strcmp(s, "go") == 0) {
        printf("capture\n"); show_status("Capturing...");
        do_capture(); printf("done\n"); update_display();
    }
    else if (sscanf(s, "video %d", &v) == 1 && v >= 2 && v <= 60) {
        p_video_frames = v; printf("video=%d frames\n", v);
        show_status("Video..."); do_video(); printf("done\n"); update_display();
    }
    else if (strcmp(s, "video") == 0) {
        show_status("Video..."); do_video(); printf("done\n"); update_display();
    }
    else if (strcmp(s, "help") == 0) {
        printf("effect N  dot N  thresh N  levels N  invert  preproc  go\n");
        printf("video [2-60] — burst capture & print flipbook\n");
        printf("GPIO33 mode: GND=thresh  1/2Vcc=size  Vcc=levels\n");
        printf("effects:\n");
        for (int i = 0; i < FX_COUNT; i++)
            printf("%d=%s\n", i, effect_names[i]);
        printf("button: tap=cycle  hold1s=capture  hold3s=video-standby\n");
    }
}

static void console_task(void *arg) {
    char line[64]; int pos = 0;
    while (1) {
        int c = getchar();
        if (c == '\n' || c == '\r') {
            printf("\n");
            if (pos) { line[pos] = 0; console_cmd(line); pos = 0; }
        }
        else if (c >= 32 && c < 127 && pos < 63) {
            line[pos++] = c;
            putchar(c);
        }
    }
}

/* ── Dials ──────────────────────────────────────────────────── */

static void dial_init(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(DIAL_TH_CH, ADC_ATTEN_DB_11);
    adc2_config_channel_atten(DIAL_SZ_CH, ADC_ATTEN_DB_11);
}

static int dial_read_adc1(adc1_channel_t ch) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++)
        sum += adc1_get_raw(ch);
    return sum / 16;
}

static int dial_read_adc2(adc2_channel_t ch) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        int v = 0;
        if (adc2_get_raw(ch, ADC_WIDTH_BIT_12, &v) == ESP_OK)
            sum += v;
    }
    return sum / 16;
}

static void dial_apply(void) {
    int raw = dial_read_adc1(DIAL_TH_CH);   /* GPIO 33 — mode selector */

    /* Determine zone with hysteresis */
    static int active_zone = DIALMODE_THRESH;
    if (dial_mode_last < 0) {
        if (raw < 1366) active_zone = DIALMODE_THRESH;
        else if (raw < 2731) active_zone = DIALMODE_SIZE;
        else active_zone = DIALMODE_LEVELS;
        dial_mode_last = 0;
    } else {
        int hyst = 120;
        switch (active_zone) {
            case DIALMODE_THRESH:
                if (raw > 1366 + hyst) active_zone = DIALMODE_SIZE;
                break;
            case DIALMODE_SIZE:
                if (raw < 1366 - hyst) active_zone = DIALMODE_THRESH;
                else if (raw > 2731 + hyst) active_zone = DIALMODE_LEVELS;
                break;
            case DIALMODE_LEVELS:
                if (raw < 2731 - hyst) active_zone = DIALMODE_SIZE;
                break;
        }
    }

    if (active_zone != dial_mode) {
        dial_mode = active_zone;
        dial_val_last = -1;
        update_display();
    }

    int val_raw;
    if (bt_active) {
        val_raw = 2048;                              /* ADC2 unusable with BT */
    } else {
        val_raw = dial_read_adc2(DIAL_SZ_CH);        /* GPIO 2 — value */
    }

    switch (dial_mode) {
        case DIALMODE_THRESH: {
            int v = val_raw * 255 / 4095;
            if (v != dial_val_last) {
                dial_val_last = v;
                p_thresh = v;
                update_display();
            }
            break;
        }
        case DIALMODE_SIZE: {
            int v = 2 + val_raw * 14 / 4095;
            if (v != dial_val_last) {
                dial_val_last = v;
                p_dot = v;
                update_display();
            }
            break;
        }
        case DIALMODE_LEVELS: {
            int v = 2 + val_raw * 4 / 4095;
            if (v > 6) v = 6;
            if (v != dial_val_last) {
                dial_val_last = v;
                p_levels = v;
                update_display();
            }
            break;
        }
    }
}

/* ── Entry ──────────────────────────────────────────────────── */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    display_init();
    dial_init();

    update_display();

    if (cam_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera fail — restart");
        show_status("Camera fail");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    if (bt_init() != 0)
        ESP_LOGW(TAG, "BT init failed — continuing without printer");
    xTaskCreate(console_task, "console", 4096, NULL, 1, NULL);

    const int vf_counts[] = {10, 25, 60};

    printf("\n=== 1 Bit Camera ready ===\n");
    printf("Btn: tap=cycle  hold1s=capture  hold3s=video\n");

    while (1) {
        int press_ms = read_button();
        if (press_ms >= 3000) {
            /* ── Enter video standby ───────────────────────── */
            int vfi = 0;
            while (vfi < 3 && vf_counts[vfi] != p_video_frames) vfi++;
            if (vfi >= 3) { vfi = 1; p_video_frames = vf_counts[1]; }
            char b[17];
            display_clear();
            snprintf(b, 17, "Video standby");
            display_text(0, 0, b);
            snprintf(b, 17, "%d frames", p_video_frames);
            display_text(1, 0, b);
            display_text(2, 0, "Hold3s=go");
            display_text(3, 0, "Hold1s=exit");
            display_show();
            printf("video standby (%d)\n", p_video_frames);

            while (1) {
                press_ms = read_button();
                if (press_ms >= 3000) {
                    show_status("Video...");
                    do_video();
                    update_display();
                    break;
                } else if (press_ms >= 1000) {
                    update_display();
                    break;
                } else if (press_ms > 0) {
                    vfi = (vfi + 1) % 3;
                    p_video_frames = vf_counts[vfi];
                    printf("video=%d frames\n", p_video_frames);
                    display_clear();
                    snprintf(b, 17, "Video standby");
                    display_text(0, 0, b);
                    snprintf(b, 17, "%d frames", p_video_frames);
                    display_text(1, 0, b);
                    display_text(2, 0, "Hold3s=go");
                    display_show();
                }
                dial_apply();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else if (press_ms >= 1000) {
            show_status("Capturing...");
            do_capture();
            update_display();
        } else if (press_ms > 0) {
            effect = (effect + 1) % FX_COUNT;
            printf("fx=%d %s\n", effect, effect_names[effect]);
            update_display();
        }
        dial_apply();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
