#!/bin/sh
# Sync web-sim to docs/sim so GitHub Pages can serve it.
cp web-sim/index.html docs/sim/index.html
echo "Copied web-sim/index.html → docs/sim/index.html"
