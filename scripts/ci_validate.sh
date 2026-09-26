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
#              no Docker). Drives CamSim with a scripted CIGI host
#              (send_cigi_test.py --sweep), since /ready requires CIGI traffic,
#              and checks the KLV sensor position matches the commanded pose.
#              Readiness = HTTP GET /ready on the health port.
#              Stream = udp://127.0.0.1:5004 (unicast, no multicast route needed).
#   --docker   (default) Run the camsim image with host networking.
#              Readiness = camsim_health.json inside the container.
#              Stream = udp://239.1.1.1:5004.
#
# Requirements: ffmpeg, ffprobe, curl, perl, python3, node + npm; docker for --docker.
#
# Usage:
#   ./scripts/ci_validate.sh --native
#   ./scripts/ci_validate.sh [--docker] [image_name]
#
# Environment:
#   CAMSIM_READY_TIMEOUT   Seconds to wait for readiness (default: 60 docker,
#                          600 native — a cold native start compiles shaders)
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

if [ "${MODE}" = "native" ]; then
    READY_TIMEOUT="${CAMSIM_READY_TIMEOUT:-600}"
    STREAM_URL="udp://127.0.0.1:${CAMSIM_MULTICAST_PORT:-5004}"
else
    READY_TIMEOUT="${CAMSIM_READY_TIMEOUT:-60}"
    STREAM_URL="udp://239.1.1.1:5004"
fi
# ffmpeg's udp timeout is in microseconds: give up if no packet arrives for 5 s.
STREAM_INPUT="${STREAM_URL}?timeout=5000000"

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

    echo "==> Starting scripted CIGI host (heading sweep at ${HOST_LAT}, ${HOST_LON}, ${HOST_ALT} m)..."
    python3 "${SCRIPT_DIR}/send_cigi_test.py" --sweep --port "${CAMSIM_CIGI_PORT:-8888}" \
        --lat "${HOST_LAT}" --lon "${HOST_LON}" --alt "${HOST_ALT}" \
        >"${WORK_DIR}/cigi_host.log" 2>&1 &
    CIGI_PID=$!

    echo "==> Waiting for GET /ready on :${HEALTH_PORT} (timeout=${READY_TIMEOUT}s)..."
    UE_PID="$(head -1 "${REPO_ROOT}/.cache/camsim.pid")"
    ELAPSED=0
    READY=0
    while [ "${ELAPSED}" -lt "${READY_TIMEOUT}" ]; do
        if ! kill -0 "${UE_PID}" 2>/dev/null; then
            echo "[FAIL] CamSim exited during startup"
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
        exit 1
    fi
else
    echo "==> Starting CamSim container (CPU/Mesa, headless)..."
    docker run -d \
        --name "${CONTAINER_NAME}" \
        --network host \
        --shm-size 1g \
        "${IMAGE}"

    echo "==> Waiting for health file (timeout=${READY_TIMEOUT}s)..."
    HEALTH_FILE=/opt/camsim/CamSimTest/Binaries/Linux/camsim_health.json
    ELAPSED=0
    while [ "${ELAPSED}" -lt "${READY_TIMEOUT}" ]; do
        if docker exec "${CONTAINER_NAME}" test -f "${HEALTH_FILE}" 2>/dev/null; then
            echo "[ci] Health file found after ${ELAPSED}s"
            echo "[ci] Health: $(docker exec "${CONTAINER_NAME}" cat "${HEALTH_FILE}" 2>/dev/null || echo "{}")"
            break
        fi
        sleep 2
        ELAPSED=$((ELAPSED + 2))
    done
    if [ "${ELAPSED}" -ge "${READY_TIMEOUT}" ]; then
        echo "[FAIL] Health file not found within ${READY_TIMEOUT}s"
        docker logs "${CONTAINER_NAME}" --tail 50
        exit 1
    fi
fi

# -----------------------------------------------------------------------
# Capture a short segment (all streams: video + KLV)
# -----------------------------------------------------------------------
echo "==> Capturing ${CAPTURE_DURATION}s from ${STREAM_URL}..."
with_timeout $((CAPTURE_DURATION + 15)) ffmpeg -y -v error -nostdin \
    -i "${STREAM_INPUT}" -t "${CAPTURE_DURATION}" -map 0 -c copy "${CAPTURE}" \
    2>"${WORK_DIR}/capture.log" || true

if [ ! -s "${CAPTURE}" ]; then
    fail "No stream received on ${STREAM_URL}"
    [ "${MODE}" = "docker" ] && echo "       (multicast needs a route: sudo ip route add 239.0.0.0/8 dev lo)"
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
DECODE_ERRORS="$(ffmpeg -v error -nostdin -i "${CAPTURE}" -map 0:v -f null - 2>&1 | wc -l | tr -d ' ')"
if [ "${DECODE_ERRORS}" -eq 0 ]; then
    echo "[ci] No decode errors"
else
    fail "${DECODE_ERRORS} decode error line(s)"
    ffmpeg -v error -nostdin -i "${CAPTURE}" -map 0:v -f null - 2>&1 | head -5
fi

# -----------------------------------------------------------------------
# KLV conformance against misb.js
# -----------------------------------------------------------------------
echo "==> Validating KLV with misb.js..."
KLV_DIR="${REPO_ROOT}/scripts/klv_conformance"
KLV_ARGS=(--max-age-sec 600 --min-packets 10)
[ "${MODE}" = "native" ] && KLV_ARGS+=(--expect-position "${HOST_LAT},${HOST_LON},${HOST_ALT}")
if (cd "${KLV_DIR}" && npm ci --no-audit --no-fund --silent) \
    && node "${KLV_DIR}/check.js" stream "${CAPTURE}" "${KLV_ARGS[@]}"; then
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
