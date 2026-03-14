#!/usr/bin/env bash
# stop.sh — Kill a detached (or orphaned) CamSim/UE instance started with run.sh --detach.
#
# Usage:
#   ./scripts/stop.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="${REPO_ROOT}/.cache/camsim.pid"

if [ ! -f "${PID_FILE}" ]; then
    echo "No running CamSim instance found (no PID file)."
    exit 0
fi

UE_PID="$(sed -n '1p' "${PID_FILE}")"
XVFB_PID="$(sed -n '2p' "${PID_FILE}")"

# -------------------------------------------------------------------------
# Stop UE
# -------------------------------------------------------------------------
if [ -z "${UE_PID}" ]; then
    echo "PID file is empty — removing."
    rm -f "${PID_FILE}"
    exit 0
fi

if ! kill -0 "${UE_PID}" 2>/dev/null; then
    echo "Process ${UE_PID} is not running — cleaning up PID file."
    rm -f "${PID_FILE}"
    exit 0
fi

echo "==> Sending SIGTERM to UE (PID ${UE_PID}) …"
kill -TERM "${UE_PID}" 2>/dev/null || true

# Poll for clean exit (up to 5s)
for i in $(seq 1 10); do
    sleep 0.5
    if ! kill -0 "${UE_PID}" 2>/dev/null; then
        break
    fi
    if [ "${i}" -eq 10 ]; then
        echo "    UE did not exit within 5s — sending SIGKILL …"
        kill -KILL "${UE_PID}" 2>/dev/null || true
    fi
done

# -------------------------------------------------------------------------
# Stop Xvfb (if any)
# -------------------------------------------------------------------------
if [ -n "${XVFB_PID}" ] && kill -0 "${XVFB_PID}" 2>/dev/null; then
    echo "==> Stopping Xvfb (PID ${XVFB_PID}) …"
    kill -TERM "${XVFB_PID}" 2>/dev/null || true
fi

rm -f "${PID_FILE}"
echo "Stopped."
