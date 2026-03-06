#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "[DEPRECATED] scripts/build_thirdparty_mac.sh is deprecated; use scripts/build_thirdparty.sh"
exec "${SCRIPT_DIR}/build_thirdparty.sh" "$@"
