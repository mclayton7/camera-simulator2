#!/usr/bin/env bash
# Fetch the X-Plane plugin SDK into hitl/xplane_plugin/.sdk/SDK (gitignored).
# The plugin compiles against API level XPLM303 (X-Plane 11.50+), so any SDK
# from 3.0.x up works; newer SDKs keep the old API behind the XPLMxxx defines.
#
#   ./fetch_sdk.sh            # pinned default (XPSDK430, sha256-checked)
#   XPSDK_VERSION=411 ./fetch_sdk.sh   # another version (no checksum pin)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HERE/.sdk"
VERSION="${XPSDK_VERSION:-430}"
URL="https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK${VERSION}.zip"
# sha256 of the pinned default, checked 2026-10-01.
PINNED_430="b9875ab27b593927b4f9b3e0ddfffe7401ee5dce6d86b50aea0da65f70ff7816"

if [[ -f "$DEST/SDK/CHeaders/XPLM/XPLMDefs.h" && -f "$DEST/VERSION" && "$(cat "$DEST/VERSION")" == "$VERSION" ]]; then
    echo "X-Plane SDK $VERSION already in $DEST/SDK"
    exit 0
fi

mkdir -p "$DEST"
ZIP="$DEST/XPSDK${VERSION}.zip"
echo "Downloading $URL"
curl -fL --retry 3 -o "$ZIP" "$URL"

if [[ "$VERSION" == "430" ]]; then
    if command -v sha256sum >/dev/null; then
        GOT="$(sha256sum "$ZIP" | cut -d' ' -f1)"
    else
        GOT="$(shasum -a 256 "$ZIP" | cut -d' ' -f1)"
    fi
    if [[ "$GOT" != "$PINNED_430" ]]; then
        echo "error: sha256 mismatch for $ZIP: $GOT (expected $PINNED_430)" >&2
        exit 1
    fi
fi

rm -rf "$DEST/SDK"
unzip -q -o "$ZIP" -d "$DEST"
rm -f "$ZIP"
echo "$VERSION" > "$DEST/VERSION"
echo "X-Plane SDK $VERSION -> $DEST/SDK"
