#!/usr/bin/env bash
# test_matrix.sh
#
# Cross-platform smoke/regression matrix runner for CamSim.
#
# Matrix profiles:
#   linux-native    Run editor headless on local host
#   linux-container Run deploy/docker-compose.yml container profile
#   mac-native      Run editor profile on macOS
#
# Validates:
#   1) MPEG-TS video stream present (test_video_output.sh)
#   2) KLV metadata decodes (validate_klv.py)
#   3) CIGI response heartbeat received (check_cigi_responses.py)
#
# Usage:
#   ./scripts/test_matrix.sh --profile linux-native --build
#   ./scripts/test_matrix.sh --profile all
#   ./scripts/test_matrix.sh --profile linux-native --skip-video

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PROFILE="all"
DO_BUILD=0
VIDEO_ADDR="127.0.0.1"
VIDEO_PORT="5004"
CIGI_HOST="127.0.0.1"
CIGI_PORT="8888"
CIGI_RESP_PORT="8889"
SEND_DURATION="6"
PROBE_DURATION="8"
SKIP_VIDEO=0
SKIP_KLV=0
SKIP_CIGI_RESP=0
WARMUP_SECONDS=10
STARTUP_TIMEOUT=180

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)        PROFILE="$2"; shift 2 ;;
        --build)          DO_BUILD=1; shift ;;
        --video-addr)     VIDEO_ADDR="$2"; shift 2 ;;
        --video-port)     VIDEO_PORT="$2"; shift 2 ;;
        --cigi-host)      CIGI_HOST="$2"; shift 2 ;;
        --cigi-port)      CIGI_PORT="$2"; shift 2 ;;
        --cigi-resp-port) CIGI_RESP_PORT="$2"; shift 2 ;;
        --skip-video)     SKIP_VIDEO=1; shift ;;
        --skip-klv)       SKIP_KLV=1; shift ;;
        --skip-cigi-resp) SKIP_CIGI_RESP=1; shift ;;
        --warmup)         WARMUP_SECONDS="$2"; shift 2 ;;
        --startup-timeout) STARTUP_TIMEOUT="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1"
            exit 1
            ;;
    esac
done

run_checked() {
    local label="$1"
    local log_file="$2"
    shift 2
    if "$@" >"${log_file}" 2>&1; then
        return 0
    fi
    echo "[FAIL] ${label} failed (see ${log_file})"
    tail -n 200 "${log_file}" || true
    return 1
}

run_native_profile() {
    local platform_label="$1"
    local run_args=("${@:2}")

    echo "==> [${platform_label}] starting CamSim"
    local build_flag=()
    if [[ "${DO_BUILD}" -eq 1 ]]; then
        build_flag=(--build)
    fi

    local run_log
    run_log="$(mktemp /tmp/camsim_matrix_run.XXXXXX.log)"
    set +e
    "${SCRIPT_DIR}/run.sh" "${build_flag[@]}" --mode editor --local "${run_args[@]}" >"${run_log}" 2>&1
    local run_status=$?
    set -e
    cat "${run_log}"
    if [[ ${run_status} -ne 0 ]]; then
        echo "[FAIL] run.sh exited with status ${run_status}"
        rm -f "${run_log}" 2>/dev/null || true
        return "${run_status}"
    fi

    local ue_pid
    ue_pid="$(sed -n 's/.*UE launched (PID \([0-9]\+\)).*/\1/p' "${run_log}" | tail -n 1)"
    if [[ -z "${ue_pid}" ]]; then
        echo "[FAIL] Could not determine UE PID from run.sh output"
        rm -f "${run_log}" 2>/dev/null || true
        return 1
    fi
    local xvfb_pid
    xvfb_pid="$(sed -n 's/.*Xvfb started on .* (PID \([0-9]\+\)).*/\1/p' "${run_log}" | tail -n 1)"

    cleanup_native() {
        rm -f "${run_log:-}" 2>/dev/null || true
        if [[ -n "${ue_pid:-}" ]]; then
            kill "${ue_pid}" 2>/dev/null || true
            sleep 1
            if kill -0 "${ue_pid}" 2>/dev/null; then
                kill -9 "${ue_pid}" 2>/dev/null || true
            fi
        fi
        if [[ -n "${xvfb_pid:-}" ]]; then
            kill "${xvfb_pid}" 2>/dev/null || true
        fi
    }

    if [[ "${WARMUP_SECONDS}" -gt 0 ]]; then
        echo "==> [${platform_label}] warmup ${WARMUP_SECONDS}s"
        sleep "${WARMUP_SECONDS}"
    fi

    if [[ "${SKIP_CIGI_RESP}" -eq 0 ]]; then
        echo "==> [${platform_label}] waiting for CIGI response heartbeat"
        run_checked "[${platform_label}] startup heartbeat" "/tmp/camsim_matrix_startup_resp.log" \
        python3 "${SCRIPT_DIR}/check_cigi_responses.py" \
            --host 0.0.0.0 \
            --port "${CIGI_RESP_PORT}" \
            --timeout "${STARTUP_TIMEOUT}" \
            --min-packets 1 || { cleanup_native; return 1; }
    fi

    echo "==> [${platform_label}] sending CIGI control traffic"
    run_checked "[${platform_label}] CIGI sender" "/tmp/camsim_matrix_send.log" \
    python3 "${SCRIPT_DIR}/send_cigi_test.py" \
        --host "${CIGI_HOST}" \
        --port "${CIGI_PORT}" \
        --duration "${SEND_DURATION}" \
        --rate 20 || { cleanup_native; return 1; }

    if [[ "${SKIP_VIDEO}" -eq 0 ]]; then
        echo "==> [${platform_label}] probing video stream"
        run_checked "[${platform_label}] Video probe" "/tmp/camsim_matrix_probe.log" \
        "${SCRIPT_DIR}/test_video_output.sh" \
            --addr "${VIDEO_ADDR}" \
            --port "${VIDEO_PORT}" \
            --duration "${PROBE_DURATION}" || { cleanup_native; return 1; }
    fi

    if [[ "${SKIP_KLV}" -eq 0 ]]; then
        echo "==> [${platform_label}] validating KLV stream"
        run_checked "[${platform_label}] KLV validator" "/tmp/camsim_matrix_klv.log" \
        python3 "${SCRIPT_DIR}/validate_klv.py" \
            --addr "${VIDEO_ADDR}" \
            --port "${VIDEO_PORT}" \
            --count 1 \
            --timeout 30 || { cleanup_native; return 1; }
    fi

    if [[ "${SKIP_CIGI_RESP}" -eq 0 ]]; then
        echo "==> [${platform_label}] validating CIGI response heartbeat"
        run_checked "[${platform_label}] CIGI response check" "/tmp/camsim_matrix_resp.log" \
        python3 "${SCRIPT_DIR}/check_cigi_responses.py" \
            --host 0.0.0.0 \
            --port "${CIGI_RESP_PORT}" \
            --timeout 10 \
            --min-packets 3 || { cleanup_native; return 1; }
    fi

    cleanup_native
    echo "[PASS] [${platform_label}] smoke checks passed"
}

run_linux_container() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "[SKIP] [linux-container] docker not available"
        return 0
    fi
    if ! docker compose version >/dev/null 2>&1; then
        echo "[SKIP] [linux-container] docker compose plugin not available"
        return 0
    fi

    echo "==> [linux-container] starting container"
    (
        cd "${REPO_ROOT}/deploy"
        CAMSIM_MULTICAST_ADDR="${VIDEO_ADDR}" \
        CAMSIM_MULTICAST_PORT="${VIDEO_PORT}" \
        CAMSIM_CIGI_PORT="${CIGI_PORT}" \
        CAMSIM_CIGI_RESPONSE_ADDR=127.0.0.1 \
        CAMSIM_CIGI_RESPONSE_PORT="${CIGI_RESP_PORT}" \
        docker compose up -d --build
    )

    cleanup_container() {
        (
            cd "${REPO_ROOT}/deploy"
            docker compose down >/dev/null 2>&1 || true
        )
    }

    if [[ "${WARMUP_SECONDS}" -gt 0 ]]; then
        echo "==> [linux-container] warmup ${WARMUP_SECONDS}s"
        sleep "${WARMUP_SECONDS}"
    fi

    if [[ "${SKIP_CIGI_RESP}" -eq 0 ]]; then
        echo "==> [linux-container] waiting for CIGI response heartbeat"
        run_checked "[linux-container] startup heartbeat" "/tmp/camsim_matrix_startup_resp_container.log" \
        python3 "${SCRIPT_DIR}/check_cigi_responses.py" \
            --host 0.0.0.0 \
            --port "${CIGI_RESP_PORT}" \
            --timeout "${STARTUP_TIMEOUT}" \
            --min-packets 1 || { cleanup_container; return 1; }
    fi

    echo "==> [linux-container] sending CIGI control traffic"
    run_checked "[linux-container] CIGI sender" "/tmp/camsim_matrix_send_container.log" \
    python3 "${SCRIPT_DIR}/send_cigi_test.py" \
        --host "${CIGI_HOST}" \
        --port "${CIGI_PORT}" \
        --duration "${SEND_DURATION}" \
        --rate 20 || { cleanup_container; return 1; }

    if [[ "${SKIP_VIDEO}" -eq 0 ]]; then
        echo "==> [linux-container] probing video stream"
        run_checked "[linux-container] Video probe" "/tmp/camsim_matrix_probe_container.log" \
        "${SCRIPT_DIR}/test_video_output.sh" \
            --addr "${VIDEO_ADDR}" \
            --port "${VIDEO_PORT}" \
            --duration "${PROBE_DURATION}" || { cleanup_container; return 1; }
    fi

    if [[ "${SKIP_KLV}" -eq 0 ]]; then
        echo "==> [linux-container] validating KLV stream"
        run_checked "[linux-container] KLV validator" "/tmp/camsim_matrix_klv_container.log" \
        python3 "${SCRIPT_DIR}/validate_klv.py" \
            --addr "${VIDEO_ADDR}" \
            --port "${VIDEO_PORT}" \
            --count 1 \
            --timeout 30 || { cleanup_container; return 1; }
    fi

    if [[ "${SKIP_CIGI_RESP}" -eq 0 ]]; then
        echo "==> [linux-container] validating CIGI response heartbeat"
        run_checked "[linux-container] CIGI response check" "/tmp/camsim_matrix_resp_container.log" \
        python3 "${SCRIPT_DIR}/check_cigi_responses.py" \
            --host 0.0.0.0 \
            --port "${CIGI_RESP_PORT}" \
            --timeout 10 \
            --min-packets 3 || { cleanup_container; return 1; }
    fi

    cleanup_container
    echo "[PASS] [linux-container] smoke checks passed"
}

run_profile() {
    case "$1" in
        linux-native)
            if [[ "$(uname -s)" != "Linux" ]]; then
                echo "[SKIP] [linux-native] not running on Linux"
                return 0
            fi
            run_native_profile "linux-native" --headless
            ;;
        mac-native)
            if [[ "$(uname -s)" != "Darwin" ]]; then
                echo "[SKIP] [mac-native] not running on macOS"
                return 0
            fi
            run_native_profile "mac-native"
            ;;
        linux-container)
            run_linux_container
            ;;
        *)
            echo "[ERROR] Invalid profile: $1"
            return 1
            ;;
    esac
}

if [[ "${SKIP_VIDEO}" -eq 0 ]] && ! command -v ffprobe >/dev/null 2>&1; then
    echo "[ERROR] ffprobe is required for video integrity checks; install ffmpeg or pass --skip-video"
    exit 1
fi

echo "==> CamSim test matrix: profile=${PROFILE}"
case "${PROFILE}" in
    all)
        run_profile linux-native
        run_profile mac-native
        run_profile linux-container
        ;;
    linux-native|mac-native|linux-container)
        run_profile "${PROFILE}"
        ;;
    *)
        echo "[ERROR] Unsupported --profile: ${PROFILE}"
        exit 1
        ;;
esac

echo "==> Matrix run complete"
