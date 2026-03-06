#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "[DEPRECATED] scripts/run_linux.sh is deprecated; use scripts/run.sh"
exec "${SCRIPT_DIR}/run.sh" "$@"
