#!/usr/bin/env bash
# package_for_docker.sh
#
# Stage a LinuxNoEditor package of CamSimTest for the Docker image build.
#
# Wraps `RunUAT.sh BuildCookRun` and copies the resulting `LinuxNoEditor/`
# directory into `deploy/staged/`, where `deploy/Dockerfile` COPYs from.
#
# Usage:
#   scripts/package_for_docker.sh
#
# Environment:
#   UE_BINARY  Path to UnrealEditor (auto-discovered if unset, same convention
#              as scripts/run.sh: tries common /opt and $HOME locations).
#   UE_ROOT    Path to the UE installation root (derived from UE_BINARY).
#
# Output:
#   .cache/staged/LinuxNoEditor/     intermediate (kept; reused by next run)
#   deploy/staged/LinuxNoEditor/     final destination consumed by Dockerfile
#
# Idempotent: re-running on a clean tree reuses the DDC cache and incremental
# build output, so the second invocation is much faster than the first.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_PROJECT="${REPO_ROOT}/unreal_project/CamSimTest/CamSimTest.uproject"
ARCHIVE_DIR="${REPO_ROOT}/.cache/staged"
STAGE_DEST="${REPO_ROOT}/deploy/staged"

# -------------------------------------------------------------------------
# Locate UnrealEditor (and derive UE_ROOT). Mirrors scripts/run.sh:119-158.
# -------------------------------------------------------------------------
if [ -z "${UE_BINARY:-}" ]; then
    for CANDIDATE in \
        /opt/UE/Engine/Binaries/Linux/UnrealEditor \
        "${HOME}/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"; do
        [ -f "${CANDIDATE}" ] && { UE_BINARY="${CANDIDATE}"; break; }
    done
    if [ -z "${UE_BINARY:-}" ]; then
        UE_BINARY="$(find /opt "${HOME}" -maxdepth 6 -name UnrealEditor \
            -path '*/Binaries/Linux/UnrealEditor' 2>/dev/null | head -n1 || true)"
    fi
fi

if [ -z "${UE_BINARY:-}" ] || [ ! -f "${UE_BINARY}" ]; then
    echo "[ERROR] UnrealEditor not found. Set UE_BINARY to the absolute path." >&2
    exit 1
fi

UE_ROOT="${UE_ROOT:-${UE_BINARY%%/Engine/Binaries/*}}"
RUN_UAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"

if [ ! -x "${RUN_UAT}" ]; then
    echo "[ERROR] RunUAT.sh not executable at ${RUN_UAT}" >&2
    exit 1
fi

# -------------------------------------------------------------------------
# Local DDC/Zen cache — match scripts/run.sh so cold-start is amortised
# across `--build-only`, packaging, and editor automation.
# -------------------------------------------------------------------------
DDC_LOCAL_ROOT="${CAMSIM_DDC_DIR:-${REPO_ROOT}/.cache/ue-ddc}"
DDC_ZEN_ROOT="${DDC_LOCAL_ROOT}/Zen"
mkdir -p "${DDC_LOCAL_ROOT}" "${DDC_ZEN_ROOT}"
export UE_LocalDataCachePath="${DDC_LOCAL_ROOT}"
export UE_SharedDataCachePath="None"
export UE_ZenDataPath="${DDC_ZEN_ROOT}"
export UE_ZenSubprocessDataPath="${DDC_ZEN_ROOT}"

# -------------------------------------------------------------------------
# Build → cook → stage → pak → archive
# -------------------------------------------------------------------------
echo "==> UE root:       ${UE_ROOT}"
echo "==> Project:       ${UE_PROJECT}"
echo "==> Archive dir:   ${ARCHIVE_DIR}"
echo "==> Stage dest:    ${STAGE_DEST}"

mkdir -p "${ARCHIVE_DIR}"

"${RUN_UAT}" BuildCookRun \
    -project="${UE_PROJECT}" \
    -noP4 \
    -platform=Linux \
    -clientconfig=Development \
    -build -cook -stage -pak -archive \
    -archivedirectory="${ARCHIVE_DIR}" \
    -utf8output

# -------------------------------------------------------------------------
# Copy into deploy/staged/ for the Dockerfile to COPY.
# -------------------------------------------------------------------------
if [ ! -d "${ARCHIVE_DIR}/LinuxNoEditor" ]; then
    echo "[ERROR] Expected ${ARCHIVE_DIR}/LinuxNoEditor after BuildCookRun" >&2
    exit 1
fi

rm -rf "${STAGE_DEST}/LinuxNoEditor"
mkdir -p "${STAGE_DEST}"
cp -a "${ARCHIVE_DIR}/LinuxNoEditor" "${STAGE_DEST}/"

echo "==> Staged at: ${STAGE_DEST}/LinuxNoEditor"
ls "${STAGE_DEST}/LinuxNoEditor/CamSimTest/Binaries/Linux/" 2>/dev/null || true
