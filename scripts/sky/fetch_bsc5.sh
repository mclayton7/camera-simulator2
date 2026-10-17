#!/usr/bin/env bash
# Fetch the Yale Bright Star Catalogue (BSC5, binary, J2000; public domain) into Content/NonUFS/Sky/.
# The download must match the committed LFS object (sha256 below); a mismatch leaves the old file in place.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/unreal_project/CamSimTest/Content/NonUFS/Sky"
EXPECTED_SHA256="e471d02eaf4eecb61c12f879a1cb6432ba9d7b68a9a8c5654a1eb42a0c8cc340"   # git lfs ls-files -l
sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1   # macOS
  fi
}
mkdir -p "$OUT"
TMP="$(mktemp "$OUT/bsc5.dat.XXXXXX")"
trap 'rm -f "$TMP"' EXIT
curl -fsSL -o "$TMP" "http://tdc-www.harvard.edu/catalogs/BSC5"
GOT="$(sha256 "$TMP")"
if [[ "$GOT" != "$EXPECTED_SHA256" ]]; then
  echo "bsc5.dat sha256 $GOT != expected $EXPECTED_SHA256; not installed" >&2
  exit 1
fi
chmod 644 "$TMP"
mv "$TMP" "$OUT/bsc5.dat"
echo "bsc5.dat installed (sha256 $GOT)"
