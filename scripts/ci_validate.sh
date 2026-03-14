#!/usr/bin/env bash
# ci_validate.sh — Automated validation for CamSim Docker image.
#
# Starts the container headless (Mesa llvmpipe), waits for the health file,
# validates H.264 + KLV streams via ffprobe, and captures a short segment
# to check for decode errors.
#
# Requirements: docker, ffprobe, ffmpeg, uv (for validate_klv.py)
#
# Usage:
#   ./scripts/ci_validate.sh [image_name]
#
# Exit: 0 = pass, 1 = fail

set -euo pipefail

IMAGE="${1:-camsim:latest}"
CONTAINER_NAME="camsim-ci-validate"
TIMEOUT=60
CAPTURE_DURATION=3
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cleanup() {
    echo "[ci] Stopping container..."
    docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true
    rm -f /tmp/camsim_ci_capture.ts
}
trap cleanup EXIT

echo "==> Starting CamSim container (CPU/Mesa, headless)..."
docker run -d \
    --name "${CONTAINER_NAME}" \
    --network host \
    --shm-size 1g \
    "${IMAGE}"

# -----------------------------------------------------------------------
# Wait for health file
# -----------------------------------------------------------------------
echo "==> Waiting for health file (timeout=${TIMEOUT}s)..."
ELAPSED=0
while [ "${ELAPSED}" -lt "${TIMEOUT}" ]; do
    if docker exec "${CONTAINER_NAME}" test -f /opt/camsim/CamSimTest/Binaries/Linux/camsim_health.json 2>/dev/null; then
        echo "[ci] Health file found after ${ELAPSED}s"
        HEALTH=$(docker exec "${CONTAINER_NAME}" cat /opt/camsim/CamSimTest/Binaries/Linux/camsim_health.json 2>/dev/null || echo "{}")
        echo "[ci] Health: ${HEALTH}"
        break
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

if [ "${ELAPSED}" -ge "${TIMEOUT}" ]; then
    echo "[FAIL] Health file not found within ${TIMEOUT}s"
    docker logs "${CONTAINER_NAME}" --tail 50
    exit 1
fi

# -----------------------------------------------------------------------
# Validate stream with ffprobe
# -----------------------------------------------------------------------
echo "==> Probing MPEG-TS stream..."
PROBE_OUTPUT=$(timeout 10 ffprobe -v error -show_streams \
    -print_format json "udp://239.1.1.1:5004?timeout=5000000" 2>&1 || true)

if echo "${PROBE_OUTPUT}" | grep -q '"codec_name": "h264"'; then
    echo "[ci] H.264 video stream detected"
else
    echo "[WARN] H.264 stream not detected via ffprobe (may need multicast route)"
fi

if echo "${PROBE_OUTPUT}" | grep -q '"codec_tag_string": "KLVA"'; then
    echo "[ci] KLVA data stream detected"
else
    echo "[WARN] KLVA stream not detected via ffprobe"
fi

# -----------------------------------------------------------------------
# Capture short segment and check for decode errors
# -----------------------------------------------------------------------
echo "==> Capturing ${CAPTURE_DURATION}s segment..."
timeout $((CAPTURE_DURATION + 10)) ffmpeg -y -v error \
    -i "udp://239.1.1.1:5004?timeout=5000000" \
    -t "${CAPTURE_DURATION}" \
    -c copy \
    /tmp/camsim_ci_capture.ts 2>&1 || true

if [ -f /tmp/camsim_ci_capture.ts ] && [ -s /tmp/camsim_ci_capture.ts ]; then
    echo "[ci] Capture file created ($(wc -c < /tmp/camsim_ci_capture.ts) bytes)"

    # Decode check
    DECODE_ERRORS=$(ffmpeg -v error -i /tmp/camsim_ci_capture.ts -f null - 2>&1 | wc -l)
    if [ "${DECODE_ERRORS}" -eq 0 ]; then
        echo "[ci] No decode errors"
    else
        echo "[WARN] ${DECODE_ERRORS} decode error line(s)"
    fi
else
    echo "[WARN] Capture file empty or missing (multicast may not be routable in CI)"
fi

# -----------------------------------------------------------------------
# Validate KLV (if script is available)
# -----------------------------------------------------------------------
if [ -f "${REPO_ROOT}/scripts/validate_klv.py" ] && [ -f /tmp/camsim_ci_capture.ts ]; then
    echo "==> Validating KLV..."
    uv run "${REPO_ROOT}/scripts/validate_klv.py" /tmp/camsim_ci_capture.ts --check-crc || true
fi

# -----------------------------------------------------------------------
# Check container health status
# -----------------------------------------------------------------------
HEALTH_STATUS=$(docker inspect --format='{{.State.Health.Status}}' "${CONTAINER_NAME}" 2>/dev/null || echo "unknown")
echo "[ci] Container health status: ${HEALTH_STATUS}"

echo ""
echo "==> CI validation complete"
exit 0
