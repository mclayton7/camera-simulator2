#!/usr/bin/env bash
# Fetch the Yale Bright Star Catalogue (BSC5, binary, J2000; public domain) into Content/NonUFS/Sky/.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/unreal_project/CamSimTest/Content/NonUFS/Sky"
mkdir -p "$OUT"
curl -fsSL -o "$OUT/bsc5.dat" "http://tdc-www.harvard.edu/catalogs/BSC5"
echo "sha256: $(sha256sum "$OUT/bsc5.dat")"
