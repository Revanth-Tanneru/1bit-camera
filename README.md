# 1 Bit Camera

ESP32-CAM firmware that captures photos, applies retro effects, and prints
them as 384-dot-wide 1-bit bitmap images on a thermal receipt printer over
Bluetooth SPP (ESC/POS).

```
Camera → greyscale → resize → effect → pack → BT SPP → thermal printer
                                                                 SD card (backup)
```

## Hardware

| Component | Detail |
|-----------|--------|
| Board     | Ai-Thinker ESP32-CAM (OV5640, 4 MB PSRAM) |
| Camera    | OV5640 5 MP, VGA (640×480), greyscale (also works with OV2640) |
| Display   | SSD1306 128×64 OLED, I2C (SDA=GPIO13, SCL=GPIO14) |
| Printer   | 57 mm thermal, ESC/POS over Bluetooth SPP, 384 dots wide |
| Button    | GPIO 0 (BOOT button) — short = cycle effect, long (≥1 s) = capture |
| Dial 1    | GPIO 33 (ADC1) — mode selector: thresh / size / levels |
| Dial 2    | GPIO 2 (ADC2) — value adjustment (disabled when BT active) |
| SD card   | SPI on GPIO 13–15, 2 (shared with OLED on 13/14, switched) |

### Pin sharing

GPIO 13/14 are shared between the OLED (I2C) and SD card (SPI). The firmware
tears down the I2C driver before SPI init and re-initialises the display after
SD card access.

GPIO 2 is shared between the value dial (ADC2) and SD card MISO. When
Bluetooth is active, ADC2 is unreliable, so the dial defaults to mid-scale
and parameters can be set via the serial console.

## Effects

18 effects are available, cycled with a short button press:

| # | Effect | Type | Dial params |
|---|--------|------|-------------|
| 0 | Floyd-Steinberg dither | error diffusion | thresh |
| 1 | Atkinson dither | error diffusion | thresh |
| 2 | Jarvis dither | error diffusion | thresh |
| 3 | Stucki dither | error diffusion | thresh |
| 4 | Bayer 2×2 | ordered dither | — |
| 5 | Bayer 4×4 | ordered dither | — |
| 6 | Bayer 8×8 | ordered dither | — |
| 7 | Random dither | threshold | — |
| 8 | Threshold | fixed threshold | thresh |
| 9 | Halftone | clustered dot | size, thresh |
| 10 | Line halftone | line screen | size |
| 11 | Crosshatch | dual-angle lines | size |
| 12 | Pixelate | downsample + dither | size |
| 13 | ASCII art | character map | size |
| 14 | Binary art | 01 character map | — |
| 15 | Comic | posterise + Ben-Day dots | size, levels |
| 16 | Silhouette | hard threshold | thresh |
| 17 | Blocks | tiled brightness | size, levels |

Pre-processing (auto-contrast, gamma, sharpen) runs before the effect.

## Usage

### Controls

- **Tap** BOOT button — cycle to next effect (or cycle frame count in video standby)
- **Hold 1 s** — capture still + save to SD + print via BT
- **Hold 3 s** — enter **video standby** (tap to cycle 10/25/60 frames, hold 3 s to start, hold 1 s to exit)
- **Dial 1** (GPIO 33) — select parameter: threshold, dot size, or levels
- **Dial 2** (GPIO 2) — adjust selected parameter
- **Serial console** — type `help` for commands

### Console commands

```
effect N     — select effect by number
dot N        — dot/block size (2–16)
thresh N     — threshold/gamma (0–255)
levels N     — levels for comic/blocks (2–6)
invert       — toggle output inversion
preproc      — toggle pre-processing
video [2-60] — set frame count then immediately capture & print flipbook
video        — capture & print flipbook (uses current count)
go           — capture and process
help         — show this menu
```

## Build

### Firmware (ESP-IDF)

```bash
idf.py set-target esp32
idf.py menuconfig       # check PSRAM, BT SPP, camera config
idf.py build
idf.py flash monitor
```

Required ESP-IDF components: `esp_camera`, `esp_bt`, `esp_spp`, `driver`,
`nvs_flash`, `fatfs`, `sdmmc`.

### Test harness (host-side)

The effects pipeline can be tested on macOS/Linux without hardware:

```bash
cc -O2 -lm -o test/test_effects test/test_effects.c main/effects.c -Imain
cd test && ./test_effects              # built-in test card, default effect
./test_effects photo.jpg halftone      # your image, halftone
./test_effects --all                   # generate all effects from test card
./test_effects --list                  # list all effects
```

Output files are written as PBM/PGM + PNG in the current directory.

### Web simulator — video flipbook

Open [`web-sim/index.html`](web-sim/index.html) directly in your browser — no
server needed. Upload a photo or MP4/WebM video, then press **Render** to preview
any effect at 384px wide.
Press **Capture** (video flipbook) to extract real frames at regular intervals and
process each through the current effect. Toggle **Strip** to see all frames stacked
vertically (the way the thermal printer outputs them), or keep **Play** running for
a 5 fps animation preview. If only a static image is loaded, simulated camera shake
is used instead.

## Wiring (Ai-Thinker ESP32-CAM)

```
ESP32-CAM          SSD1306 OLED
GPIO13 ───── SDA ──┐
GPIO14 ───── SCL ──┤
3.3V   ───── VCC ──┤
GND    ───── GND ──┘

ESP32-CAM          BOOT button
GPIO0 ───────────── (built-in, pull-up)

ESP32-CAM          Dials
GPIO33 ─── 10k pot ─── 3.3V   (mode select)
GPIO2  ─── 10k pot ─── 3.3V   (value adjust)

ESP32-CAM          SD card (onboard)
GPIO13 ── CS
GPIO14 ── CLK
GPIO15 ── MOSI
GPIO2  ── MISO
3.3V   ── VCC
GND    ── GND

ESP32-CAM          Thermal printer (BT SPP)
UART TX ── BT module RX  (or use ESP32 built-in BT)
```

## Project structure

```
├── .github/
│   └── workflows/
│       └── jekyll-docker.yml  GitHub Pages workflow
├── CMakeLists.txt             ESP-IDF project file
├── sdkconfig.defaults         ESP-IDF defaults (PSRAM, BT, camera)
├── main/
│   ├── CMakeLists.txt         Component build file
│   ├── main.c                 Firmware entry, capture loop, console
│   ├── effects.c              Image processing (dither, halftone, etc.)
│   ├── effects.h              Effects API
│   ├── display.c              SSD1306 OLED driver (I2C)
│   └── display.h              Display API
├── test/
│   ├── test_effects.c         Host-side test harness
│   └── *.jpg                  Test source images
├── docs/
│   ├── index.html             GitHub Pages landing page
│   └── sim/
│       └── index.html         Browser-based effect preview (Pages copy)
├── scripts/
│   └── sync-sim.sh            Sync web-sim → docs/sim
└── web-sim/
    └── index.html             Browser-based effect preview (canonical)
```

## Licence

MIT
