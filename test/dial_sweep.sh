#!/bin/bash
# Dual-dial sweep test: generates effect outputs at multiple dial positions
# Generates TWO sweeps per effect:
#   1. Threshold sweep (Dial 1): varies -t, fixed -d
#   2. Size sweep (Dial 2): varies -d, fixed -t, or one "n/a" image
# Usage: ./dial_sweep.sh <input_image> [output_dir]

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/test_effects"
INPUT="$(cd "$(dirname "${1:?Usage: $0 <input_image> [output_dir]}")" && pwd)/$(basename "$1")"
OUTDIR="${2:-"$SCRIPT_DIR/dial_sweep_$(date +%Y%m%d_%H%M%S)"}"

[ -f "$BINARY" ] || { echo "Build test_effects first"; exit 1; }
[ -f "$INPUT" ]  || { echo "Not found: $INPUT"; exit 1; }

mkdir -p "$OUTDIR"

IMG_TAG="$(basename "$INPUT")"
IMG_TAG="${IMG_TAG%.*}"

echo "=== Dual-Dial Sweep Test ==="
echo "Input:  $INPUT"
echo "Output: $OUTDIR"
echo ""

# ── Effect definitions ────────────────────────────────────
# Format: name:size_param_name
#   size_param_name = dot|blk|per|"" (empty = n/a)

SWEEPS=(
    "fs:px"
    "atkinson:px"
    "jarvis:px"
    "stucki:px"
    "bayer2:px"
    "bayer4:px"
    "bayer8:px"
    "random:px"
    "threshold:px"
    "silhouette:px"
    "halftone:px"
    "comic:px"
    "pixelate:px"
    "blocks:px"
    "line:px"
    "crosshatch:px"
    "ascii:"
    "binary:"
)

TH_VALS=(0 32 64 96 128 160 192 224 255)
SZ_VALS=(2 4 6 8 10 12 14 16)

FIXED_DOT=8
FIXED_TH=128

TOTAL_SWEEPS=${#SWEEPS[@]}
CURRENT=0

for entry in "${SWEEPS[@]}"; do
    IFS=':' read -r name sz_name <<< "$entry"
    CURRENT=$((CURRENT + 1))

    EFFDIR="$OUTDIR/$name"
    BINDIR="$EFFDIR/bin"
    mkdir -p "$BINDIR"
    cp "$BINARY" "$BINDIR/test_effects"
    echo "[$CURRENT/$TOTAL_SWEEPS] $name"

    # ── Sweep 1: Threshold (Dial 1) ───────────────────────
    echo "       Threshold sweep (${#TH_VALS[@]} positions)"
    for v in "${TH_VALS[@]}"; do
        "$BINDIR/test_effects" "$INPUT" "$name" -t "$v" -d "$FIXED_DOT" -W 384 >/dev/null 2>&1
        for f in "$BINDIR"/fx_*.png; do
            [ -f "$f" ] || continue
            mv "$f" "$EFFDIR/${name}_th${v}.png"
        done
        rm -f "$BINDIR"/fx_*.pbm "$BINDIR"/fx_*.pgm 2>/dev/null || true
    done

    # ── Sweep 2: Size (Dial 2) ────────────────────────────
    if [ -z "$sz_name" ]; then
        echo "       Size: n/a"
        "$BINDIR/test_effects" "$INPUT" "$name" -t "$FIXED_TH" -d "$FIXED_DOT" -W 384 >/dev/null 2>&1
        for f in "$BINDIR"/fx_*.png; do
            [ -f "$f" ] || continue
            mv "$f" "$EFFDIR/${name}_na.png"
        done
        rm -f "$BINDIR"/fx_*.pbm "$BINDIR"/fx_*.pgm 2>/dev/null || true
    else
        echo "       Size sweep ($sz_name, ${#SZ_VALS[@]} positions)"
        for v in "${SZ_VALS[@]}"; do
            "$BINDIR/test_effects" "$INPUT" "$name" -t "$FIXED_TH" -d "$v" -W 384 >/dev/null 2>&1
            for f in "$BINDIR"/fx_*.png; do
                [ -f "$f" ] || continue
                mv "$f" "$EFFDIR/${name}_sz${v}.png"
            done
            rm -f "$BINDIR"/fx_*.pbm "$BINDIR"/fx_*.pgm 2>/dev/null || true
        done
    fi

    rm -rf "$BINDIR"
done

# Clean up stray files in OUTDIR
rm -f "$OUTDIR"/fx_*.png "$OUTDIR"/fx_*.pbm "$OUTDIR"/fx_*.pgm 2>/dev/null || true

# ── Generate index.html ───────────────────────────────────
cat > "$OUTDIR/index.html" << 'HTMLHEAD'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Dual-Dial Sweep Test</title>
<style>
body{font-family:sans-serif;margin:20px;background:#f5f5f5}
h1{color:#333}
.effect{border:1px solid #ccc;border-radius:8px;padding:16px;margin:16px 0;background:#fff}
.effect h2{margin:0 0 8px;color:#555}
.effect .row-label{font-size:13px;color:#888;margin:8px 0 4px}
.sweep{display:flex;flex-wrap:wrap;gap:4px;margin-bottom:8px}
.sweep figure{margin:0;text-align:center}
.sweep img{height:140px;border:1px solid #ddd;border-radius:4px}
.sweep figcaption{font-size:11px;color:#666;margin-top:2px}
</style></head><body>
<h1>Dual-Dial Sweep Test</h1>
<p>Source: INPUT_TAG</p>
HTMLHEAD

for entry in "${SWEEPS[@]}"; do
    IFS=':' read -r name sz_name <<< "$entry"

    echo '<div class="effect">' >> "$OUTDIR/index.html"
    echo "<h2>$name</h2>" >> "$OUTDIR/index.html"

    # ── Threshold row ─────────────────────────────────────
    echo '<div class="row-label">Dial 1 — Threshold</div>' >> "$OUTDIR/index.html"
    echo '<div class="sweep">' >> "$OUTDIR/index.html"
    for v in "${TH_VALS[@]}"; do
        src="${name}_th${v}.png"
        if [ -f "$OUTDIR/$name/$src" ]; then
            cp "$OUTDIR/$name/$src" "$OUTDIR/${name}_th${v}.png"
            echo "<figure><img src=\"${name}_th${v}.png\"><figcaption>th=$v</figcaption></figure>" >> "$OUTDIR/index.html"
        fi
    done
    echo '</div>' >> "$OUTDIR/index.html"

    # ── Size row ──────────────────────────────────────────
    if [ -z "$sz_name" ]; then
        echo '<div class="row-label">Dial 2 — Size: n/a</div>' >> "$OUTDIR/index.html"
        echo '<div class="sweep">' >> "$OUTDIR/index.html"
        src="${name}_na.png"
        if [ -f "$OUTDIR/$name/$src" ]; then
            cp "$OUTDIR/$name/$src" "$OUTDIR/${name}_na.png"
            echo "<figure><img src=\"${name}_na.png\"><figcaption>n/a</figcaption></figure>" >> "$OUTDIR/index.html"
        fi
        echo '</div>' >> "$OUTDIR/index.html"
    else
        LABEL="pixel size"
        echo "<div class=\"row-label\">Dial 2 — $LABEL</div>" >> "$OUTDIR/index.html"
        echo '<div class="sweep">' >> "$OUTDIR/index.html"
        for v in "${SZ_VALS[@]}"; do
            src="${name}_sz${v}.png"
            if [ -f "$OUTDIR/$name/$src" ]; then
                cp "$OUTDIR/$name/$src" "$OUTDIR/${name}_sz${v}.png"
                echo "<figure><img src=\"${name}_sz${v}.png\"><figcaption>px=$v</figcaption></figure>" >> "$OUTDIR/index.html"
            fi
        done
        echo '</div>' >> "$OUTDIR/index.html"
    fi

    echo '</div>' >> "$OUTDIR/index.html"
done

sed -i '' "s|INPUT_TAG|$IMG_TAG|" "$OUTDIR/index.html"

echo ""
echo "=== Done ==="
echo "Open: $OUTDIR/index.html"
