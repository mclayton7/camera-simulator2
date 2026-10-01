#!/usr/bin/env bash
# GPU automation tests (CamSim.GPU.*) on the real RHI (Metal on macOS, Vulkan on Linux).
# NullRHI runs skip them. First run compiles the global shaders (minutes).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
RHI_ARGS=()
XVFB_PID=""
if [[ "$(uname)" == "Linux" ]]; then
  UE_BIN="${UE_BIN:-/opt/UE/Engine/Binaries/Linux/UnrealEditor}"
  RHI_ARGS=(-vulkan)
  # SDL needs an X display even offscreen (as scripts/run.sh --headless).
  if [[ -z "${DISPLAY:-}" ]] && command -v Xvfb >/dev/null; then
    Xvfb :98 -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 & XVFB_PID=$!
    export DISPLAY=:98
    trap '[[ -n "$XVFB_PID" ]] && kill "$XVFB_PID" 2>/dev/null' EXIT
  fi
else
  UE_BIN="${UE_BIN:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
fi
FILTER="${1:-CamSim.GPU}"
OUT="$REPO/.cache/automation-report-gpu"
rm -rf "$OUT"
mkdir -p "$REPO/.cache"
"$UE_BIN" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests ${FILTER}+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$OUT" -unattended -nosound -nosplash -DisablePython -RenderOffscreen ${RHI_ARGS[@]+"${RHI_ARGS[@]}"} \
  -log -stdout -FullStdOutLogOutput > "$REPO/.cache/automation-gpu.log" 2>&1 || true
if [[ ! -f "$OUT/index.json" ]]; then
  echo "no report — see .cache/automation-gpu.log" >&2
  exit 1
fi
python3 - "$OUT/index.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8-sig"))
for t in d["tests"]:
    if t["state"] == "Fail":
        msgs = [e["event"]["message"] for e in t["entries"] if e["event"]["type"] == "Error"]
        print("FAIL", t["fullTestPath"], msgs)
print("succeeded", d["succeeded"], "failed", d["failed"])
sys.exit(1 if d["failed"] or d["succeeded"] == 0 else 0)
EOF
