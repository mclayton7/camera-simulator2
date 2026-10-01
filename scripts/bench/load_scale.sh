#!/usr/bin/env bash
# Entity load scaling against the Docker image (ROADMAP 3B exit check, 1080p).
#
# For each point, starts the image with the GPU, drives it with
# scripts/stress_entity_rendering.py (N F-16s in a 500 m ring; it also flies
# the camera, entity 1, ~1.2 km away), records 75 s and keeps frames 25-75 s.
# Points: ~0, 100, 250, 500 entities, 500 + ML ground truth, 500 + ML without
# the depth map. Summarise with scripts/bench/load_report.py OUT.
#
# Usage: scripts/bench/load_scale.sh [OUT] (default .cache/bench/load)
# Env:   IMAGE (camsim:latest), CONFIG (camsim_config.yaml to mount; e.g. a
#        1080p copy, see scripts/bench/README.md), CAMSIM_HEALTH_HTTP_PORT (8080)
set -u
cd "$(dirname "$0")/../.."
OUT="${1:-.cache/bench/load}"
IMAGE="${IMAGE:-camsim:latest}"
PORT="${CAMSIM_HEALTH_HTTP_PORT:-8080}"
mkdir -p "$OUT"; chmod 777 "$OUT"
CONFIG_ARGS=()
[ -n "${CONFIG:-}" ] && CONFIG_ARGS=(-v "$(realpath "$CONFIG"):/opt/camsim/CamSimTest/camsim_config.yaml:ro")

point() {  # label count [docker -e args...]
  local label=$1 count=$2; shift 2
  local d="$OUT/$label"
  # The container writes as uid 1000; clear old output through a container too.
  docker run --rm -v "$PWD/$OUT:/out" --entrypoint rm "$IMAGE" -rf "/out/$label"
  mkdir -p "$d"; chmod 777 "$d"
  docker rm -f camsim-bench >/dev/null 2>&1
  docker run -d --name camsim-bench --gpus all --init --network host --shm-size 1g \
    -v "$PWD/$d:/bench" -v camsim-bench-data:/var/lib/camsim -v camsim-bench-cache:/home/camsim/.cache \
    "${CONFIG_ARGS[@]}" -e CAMSIM_MULTICAST_ADDR=127.0.0.1 -e CAMSIM_FRAME_STATS_PATH=/bench/frames.jsonl \
    -e CAMSIM_TRACK_PIPELINE_LATENCY=1 "$@" "$IMAGE" >/dev/null
  python3 scripts/stress_entity_rendering.py --count "$count" --entity-type 1001 \
    --duration 400 --rate 30 > "$d/stress.log" 2>&1 &
  local sp=$!
  for _ in $(seq 1 120); do curl -sf -m2 "localhost:$PORT/ready" >/dev/null && break; sleep 2; done
  local t0; t0=$(date +%s)
  sleep 75
  curl -s "localhost:$PORT/metrics" > "$d/metrics.txt"
  echo "{\"start\": $((t0 + 25)), \"end\": $((t0 + 75))}" > "$d/window.json"
  kill "$sp" 2>/dev/null
  docker logs camsim-bench > "$d/container.log" 2>&1
  docker stop -t 30 camsim-bench >/dev/null; docker rm -f camsim-bench >/dev/null
  echo "$label done"; sleep 35  # health port TIME_WAIT
}

point n0 1  # the sender needs >= 1 entity
point n100 100
point n250 250
point n500 500
point n500-ml 500 -e CAMSIM_ML_ENABLED=1 -e CAMSIM_ML_OUTPUT_DIR=/bench/ml
point n500-ml-nodepth 500 -e CAMSIM_ML_ENABLED=1 -e CAMSIM_ML_OUTPUT_DIR=/bench/ml -e CAMSIM_ML_DEPTH_ENABLED=0
python3 scripts/bench/load_report.py "$OUT"
