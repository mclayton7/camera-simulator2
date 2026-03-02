#!/usr/bin/env bash
# test_video_output.sh
#
# Validates the CamSim MPEG-TS/H.264 multicast output using ffprobe and ffplay.
#
# Prerequisites:
#   brew install ffmpeg
#
# Usage:
#   ./scripts/test_video_output.sh [options]
#
# Options:
#   --addr ADDR     Multicast/unicast address to listen on (default: 239.1.1.1)
#   --port PORT     UDP port (default: 5004)
#   --play          Launch ffplay to show the live video (default: probe only)
#   --duration SEC  How long to probe/record before exiting (default: 10)
#   --save FILE     Dump received stream to a .ts file for offline inspection
#   --help          Show this message

set -euo pipefail

ADDR="239.1.1.1"
PORT="5004"
DO_PLAY=0
DURATION=10
SAVE_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --addr)     ADDR="$2";      shift 2 ;;
        --port)     PORT="$2";      shift 2 ;;
        --play)     DO_PLAY=1;      shift   ;;
        --duration) DURATION="$2";  shift 2 ;;
        --save)     SAVE_FILE="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "[WARN] Unknown option: $1"; shift ;;
    esac
done

# -------------------------------------------------------------------------
# Detect ffprobe / ffplay
# -------------------------------------------------------------------------
if ! command -v ffprobe &>/dev/null; then
    echo "[ERROR] ffprobe not found. Install with: brew install ffmpeg"
    exit 1
fi

# On macOS, UDP multicast reception requires a route to the multicast group.
# If running both CamSim and the test on the same machine, add a loopback route:
#   sudo route add -net 239.0.0.0/8 -interface lo0
#
# Alternatively use unicast for local testing:
#   CAMSIM_MULTICAST_ADDR=127.0.0.1 ./scripts/run_mac.sh
#   ./scripts/test_video_output.sh --addr 127.0.0.1

UDP_URL="udp://@${ADDR}:${PORT}"

echo "==> CamSim video output test"
echo "    Source : ${UDP_URL}"
echo "    Timeout: ${DURATION}s"
echo ""

# -------------------------------------------------------------------------
# Phase 1: ffprobe — stream metadata
# -------------------------------------------------------------------------
echo "--- ffprobe stream info ---"
ffprobe \
    -v error \
    -analyzeduration "${DURATION}000000" \
    -probesize 5000000 \
    -i "${UDP_URL}" \
    -show_streams \
    -show_format \
    -print_format json \
    2>&1 | tee /tmp/camsim_probe.json

echo ""

# Extract key fields for a quick summary
if command -v python3 &>/dev/null; then
    python3 - << 'EOF'
import json, sys

try:
    data = json.load(open("/tmp/camsim_probe.json"))
except Exception as e:
    print(f"[WARN] Could not parse probe JSON: {e}")
    sys.exit(0)

streams = data.get("streams", [])
print("=== Stream Summary ===")
for s in streams:
    idx   = s.get("index", "?")
    codec = s.get("codec_name", "?")
    ctype = s.get("codec_type", "?")
    tag   = s.get("codec_tag_string", "")
    fps   = s.get("avg_frame_rate", "?")
    res   = f"{s.get('width','?')}x{s.get('height','?')}" if ctype == "video" else ""
    print(f"  Stream #{idx}: {ctype:6s}  codec={codec:10s}  tag={tag:6s}  {res}  fps={fps}")

print()

# Validation checks
video_streams = [s for s in streams if s.get("codec_type") == "video"]
data_streams  = [s for s in streams if s.get("codec_type") == "data"]

ok = True
if not video_streams:
    print("[FAIL] No video stream found")
    ok = False
else:
    vs = video_streams[0]
    if vs.get("codec_name") != "h264":
        print(f"[FAIL] Video codec is '{vs.get('codec_name')}', expected h264")
        ok = False
    else:
        print("[PASS] Video stream: H.264")

    w, h = vs.get("width"), vs.get("height")
    if w == 1920 and h == 1080:
        print("[PASS] Resolution: 1920x1080")
    else:
        print(f"[WARN] Resolution: {w}x{h} (expected 1920x1080)")

    fps = vs.get("avg_frame_rate", "")
    if fps in ("30/1", "30000/1000", "30000/1001"):
        print(f"[PASS] Frame rate: {fps}")
    else:
        print(f"[WARN] Frame rate: {fps} (expected ~30)")

if not data_streams:
    print("[WARN] No data (KLV) stream found — Phase 4 not yet active")
else:
    ds = data_streams[0]
    tag = ds.get("codec_tag_string", "")
    if "KLVA" in tag or ds.get("codec_name") == "klv":
        print("[PASS] KLV data stream present")
    else:
        print(f"[WARN] Data stream found but tag='{tag}', expected KLVA")

print()
print("[PASS] Probe complete" if ok else "[FAIL] One or more checks failed")
EOF
fi

# -------------------------------------------------------------------------
# Phase 2: Save to file (optional)
# -------------------------------------------------------------------------
if [ -n "${SAVE_FILE}" ]; then
    echo ""
    echo "--- Saving ${DURATION}s of stream to ${SAVE_FILE} ---"
    ffmpeg -y \
        -i "${UDP_URL}" \
        -t "${DURATION}" \
        -c copy \
        "${SAVE_FILE}"
    echo "[DONE] Saved: ${SAVE_FILE}"
    echo "       Inspect with: ffprobe -show_streams ${SAVE_FILE}"
fi

# -------------------------------------------------------------------------
# Phase 3: Live playback (optional)
# -------------------------------------------------------------------------
if [ "${DO_PLAY}" -eq 1 ]; then
    if ! command -v ffplay &>/dev/null; then
        echo "[ERROR] ffplay not found. Install with: brew install ffmpeg"
        exit 1
    fi
    echo ""
    echo "--- Launching ffplay (close window or Ctrl-C to exit) ---"
    ffplay \
        -fflags nobuffer \
        -flags low_delay \
        -framedrop \
        -strict experimental \
        -i "${UDP_URL}"
fi
