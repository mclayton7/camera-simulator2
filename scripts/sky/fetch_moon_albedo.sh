#!/usr/bin/env bash
# Rebuild Content/NonUFS/Sky/moon_albedo.pgm (ROADMAP 6A moon disc) from the NASA SVS CGI Moon Kit colour map
# (LROC WAC Hapke-normalised mosaic; NASA/GSFC/Arizona State University; public domain, credit NASA's Scientific
# Visualization Studio). The download and the derived map must match the pinned sha256s; a mismatch installs nothing.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/unreal_project/CamSimTest/Content/NonUFS/Sky"
URL="https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/lroc_color_16bit_srgb_4k.tif"
SRC_SHA256="9731fa8af425b6c2f88f277ecca82bf8c603f3743894f64ed7b25c5bfefa22ff"   # 61,891,324 bytes, fetched 2026-10-17
PGM_SHA256="0ab592bc8322546900a51be8dae00e67fac4a3c2562fdc2e68225d3ec08cb4e1"   # git lfs ls-files -l
sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1   # macOS
  fi
}
mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/dl"
curl -fsSL -o "$TMP/dl/lroc.tif" "$URL"
GOT="$(sha256 "$TMP/dl/lroc.tif")"
if [[ "$GOT" != "$SRC_SHA256" ]]; then
  echo "lroc_color_16bit_srgb_4k.tif sha256 $GOT != expected $SRC_SHA256; not installed" >&2
  exit 1
fi
uv run --with tifffile --with imagecodecs --with numpy python -I "$REPO/scripts/sky/make_moon_albedo.py" "$TMP/dl/lroc.tif" "$TMP/moon_albedo.pgm"
GOT="$(sha256 "$TMP/moon_albedo.pgm")"
if [[ "$GOT" != "$PGM_SHA256" ]]; then
  echo "moon_albedo.pgm sha256 $GOT != expected $PGM_SHA256; not installed" >&2
  exit 1
fi
chmod 644 "$TMP/moon_albedo.pgm"
mv "$TMP/moon_albedo.pgm" "$OUT/moon_albedo.pgm"
echo "moon_albedo.pgm installed (sha256 $GOT)"
