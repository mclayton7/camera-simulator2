#!/usr/bin/env bash
# GPU automation tests (CamSim.GPU.*) on the real RHI (Metal on macOS).
# NullRHI runs skip them. First run compiles the global shaders (minutes).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
UE_BIN="${UE_BIN:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
FILTER="${1:-CamSim.GPU}"
OUT="$REPO/.cache/automation-report-gpu"
rm -rf "$OUT"
mkdir -p "$REPO/.cache"
"$UE_BIN" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests ${FILTER}+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$OUT" -unattended -nosound -nosplash -DisablePython -RenderOffscreen \
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
