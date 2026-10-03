#!/usr/bin/env bash
# ci_validate.sh — End-to-end validation of a running CamSim.
#
# Launches CamSim headless, waits until it is ready, captures a few seconds of
# the MPEG-TS output and checks it: H.264 + KLVA streams present, no decode
# errors, and KLV that conforms to misb.js (the reference downstream decoder).
# Every check is a hard failure.
#
# Modes:
#   --native   Launch via scripts/run.sh --headless --local (macOS or Linux,
#              no Docker).
#   --docker   (default) Run the camsim image with host networking and the
#              NVIDIA GPU (--gpus all; needs nvidia-container-toolkit).
#
# Both modes drive CamSim with a scripted CIGI host (send_cigi_test.py
# --sweep), since /ready requires CIGI traffic, and check the KLV sensor
# position matches the commanded pose.
# Readiness = HTTP GET /ready on the health port.
# Stream = udp://127.0.0.1:5004 (unicast, no multicast route needed).
#
# Requirements: ffmpeg, ffprobe, curl, perl, python3, node + npm; docker for --docker.
#
# Usage:
#   ./scripts/ci_validate.sh --native
#   ./scripts/ci_validate.sh [--docker] [image_name]
#
# Environment:
#   CAMSIM_CI_DOCKER_LOG   --docker: save the container log to this file
#   CAMSIM_READY_TIMEOUT   Seconds to wait for readiness (default 600: a cold
#                          start compiles shaders)
#   CAMSIM_DOCKER_GPU      --docker: GPU flag (default "--gpus all"; use
#                          "--device nvidia.com/gpu=all" where only CDI
#                          injects the Vulkan ICD, docs/docker.md)
#   CAMSIM_HEALTH_HTTP_PORT  Health port, passed to the container (default 8080)
#
# Exit: 0 = pass, 1 = fail

set -euo pipefail

MODE="docker"
IMAGE="camsim:latest"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --native) MODE="native"; shift ;;
        --docker) MODE="docker"; shift ;;
        -h|--help) sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)        IMAGE="$1";    shift ;;
    esac
done

CONTAINER_NAME="camsim-ci-validate"
CAPTURE_DURATION=5
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/camsim_ci.XXXXXX")"
CAPTURE="${WORK_DIR}/capture.ts"
HEALTH_PORT="${CAMSIM_HEALTH_HTTP_PORT:-8080}"
# Pose the scripted CIGI host commands (native mode). Deliberately far from the
# config's start position, so a camera that ignores the host fails the KLV check.
HOST_LAT=37.7749
HOST_LON=-122.4194
HOST_ALT=1000

READY_TIMEOUT="${CAMSIM_READY_TIMEOUT:-600}"
STREAM_URL="udp://127.0.0.1:${CAMSIM_MULTICAST_PORT:-5004}"

FAILURES=0
fail() { echo "[FAIL] $*"; FAILURES=$((FAILURES + 1)); }

# macOS has no coreutils `timeout`; perl is on every runner we target.
with_timeout() {
    local secs="$1"; shift
    perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "${secs}" "$@"
}

CIGI_PID=""

cleanup() {
    [ -n "${CIGI_PID}" ] && kill "${CIGI_PID}" 2>/dev/null || true
    if [ "${MODE}" = "native" ]; then
        echo "[ci] Stopping CamSim..."
        "${SCRIPT_DIR}/stop.sh" >/dev/null 2>&1 || true
    else
        echo "[ci] Stopping container..."
        [ -n "${CAMSIM_CI_DOCKER_LOG:-}" ] && docker logs "${CONTAINER_NAME}" >"${CAMSIM_CI_DOCKER_LOG}" 2>&1 || true
        docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    fi
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# -----------------------------------------------------------------------
# Launch and wait for readiness
# -----------------------------------------------------------------------
if [ "${MODE}" = "native" ]; then
    if [ -f "${REPO_ROOT}/.cache/camsim.pid" ] && kill -0 "$(head -1 "${REPO_ROOT}/.cache/camsim.pid")" 2>/dev/null; then
        echo "[FAIL] A CamSim instance is already running (scripts/stop.sh to stop it)"
        trap - EXIT; rm -rf "${WORK_DIR}"; exit 1
    fi
    echo "==> Starting CamSim natively (headless, unicast)..."
    "${SCRIPT_DIR}/run.sh" --headless --local --detach
    UE_PID="$(head -1 "${REPO_ROOT}/.cache/camsim.pid")"
    camsim_alive() { kill -0 "${UE_PID}" 2>/dev/null; }
else
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    echo "==> Starting CamSim container (NVIDIA GPU, headless, unicast)..."
    docker run -d \
        --name "${CONTAINER_NAME}" \
        ${CAMSIM_DOCKER_GPU:---gpus all} \
        --init \
        --network host \
        --shm-size 1g \
        -e CAMSIM_MULTICAST_ADDR=127.0.0.1 \
        ${CAMSIM_CESIUM_ION_TOKEN:+-e CAMSIM_CESIUM_ION_TOKEN} \
        ${CAMSIM_HEALTH_HTTP_PORT:+-e CAMSIM_HEALTH_HTTP_PORT} \
        "${IMAGE}"
    camsim_alive() { [ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null)" = "true" ]; }
fi

echo "==> Starting scripted CIGI host (heading sweep at ${HOST_LAT}, ${HOST_LON}, ${HOST_ALT} m)..."
python3 "${SCRIPT_DIR}/send_cigi_test.py" --sweep --port "${CAMSIM_CIGI_PORT:-8888}" \
    --lat "${HOST_LAT}" --lon "${HOST_LON}" --alt "${HOST_ALT}" \
    >"${WORK_DIR}/cigi_host.log" 2>&1 &
CIGI_PID=$!

echo "==> Waiting for GET /ready on :${HEALTH_PORT} (timeout=${READY_TIMEOUT}s)..."
ELAPSED=0
READY=0
while [ "${ELAPSED}" -lt "${READY_TIMEOUT}" ]; do
    if ! camsim_alive; then
        echo "[FAIL] CamSim exited during startup"
        [ "${MODE}" = "docker" ] && docker logs "${CONTAINER_NAME}" --tail 50
        exit 1
    fi
    if curl -sf -m 2 "http://127.0.0.1:${HEALTH_PORT}/ready" >"${WORK_DIR}/ready.json" 2>/dev/null; then
        echo "[ci] Ready after ${ELAPSED}s: $(cat "${WORK_DIR}/ready.json")"
        READY=1
        break
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done
if [ "${READY}" -eq 0 ]; then
    echo "[FAIL] /ready did not return 200 within ${READY_TIMEOUT}s"
    curl -s -m 2 "http://127.0.0.1:${HEALTH_PORT}/ready" || true
    [ "${MODE}" = "docker" ] && docker logs "${CONTAINER_NAME}" --tail 50
    exit 1
fi

# -----------------------------------------------------------------------
# Capture a short segment (all streams: video + KLV)
# -----------------------------------------------------------------------
# Raw datagrams, not `ffmpeg -c copy`: ffmpeg <= 6.1's remux strips 5 bytes
# from every KLV packet (see scripts/klv_conformance/mpegts.js).
KLV_DIR="${REPO_ROOT}/scripts/klv_conformance"
(cd "${KLV_DIR}" && npm ci --no-audit --no-fund --silent)
echo "==> Capturing ${CAPTURE_DURATION}s from ${STREAM_URL}..."
with_timeout $((CAPTURE_DURATION + 15)) node "${KLV_DIR}/check.js" capture \
    "${STREAM_URL}" "${CAPTURE}" --duration-sec "${CAPTURE_DURATION}" \
    >"${WORK_DIR}/capture.log" 2>&1 || true

if [ ! -s "${CAPTURE}" ]; then
    fail "No stream received on ${STREAM_URL}"
    exit 1
fi
echo "[ci] Captured $(wc -c < "${CAPTURE}" | tr -d ' ') bytes"

# -----------------------------------------------------------------------
# Stream layout
# -----------------------------------------------------------------------
PROBE="$(ffprobe -v error -show_entries stream=codec_name,codec_tag_string -of compact "${CAPTURE}")"
echo "${PROBE}" | sed 's/^/[ci]   /'
echo "${PROBE}" | grep -q 'codec_name=h264' || fail "No H.264 video stream"
echo "${PROBE}" | grep -q 'codec_tag_string=KLVA' || fail "No KLVA data stream"

# -----------------------------------------------------------------------
# Decode check
# -----------------------------------------------------------------------
# The raw capture joins mid-GOP; a video-only stream copy starts it at the
# first keyframe, so frames whose SPS/PPS weren't captured don't count.
VIDEO="${WORK_DIR}/video.ts"
ffmpeg -y -v error -nostdin -i "${CAPTURE}" -map 0:v -c copy "${VIDEO}" 2>/dev/null || true
DECODE_ERRORS="$(ffmpeg -v error -nostdin -i "${VIDEO}" -map 0:v -f null - 2>&1 | wc -l | tr -d ' ')"
if [ "${DECODE_ERRORS}" -eq 0 ]; then
    echo "[ci] No decode errors"
else
    fail "${DECODE_ERRORS} decode error line(s)"
    ffmpeg -v error -nostdin -i "${VIDEO}" -map 0:v -f null - 2>&1 | head -5
fi

# -----------------------------------------------------------------------
# KLV conformance against misb.js
# -----------------------------------------------------------------------
echo "==> Validating KLV with misb.js..."
KLV_ARGS=(--max-age-sec 600 --min-packets 10)
KLV_ARGS+=(--expect-position "${HOST_LAT},${HOST_LON},${HOST_ALT}")
if node "${KLV_DIR}/check.js" stream "${CAPTURE}" "${KLV_ARGS[@]}"; then
    :
else
    fail "KLV does not conform to misb.js"
fi

if [ "${MODE}" = "docker" ]; then
    echo "[ci] Container health status: $(docker inspect --format='{{.State.Health.Status}}' "${CONTAINER_NAME}" 2>/dev/null || echo unknown)"
fi

echo ""
if [ "${FAILURES}" -ne 0 ]; then
    echo "==> CI validation FAILED (${FAILURES} check(s))"
    exit 1
fi
echo "==> CI validation passed"
