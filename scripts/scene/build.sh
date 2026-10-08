#!/usr/bin/env bash
# Run camsim-scene in the reference build container (docs/scene-packages.md).
#   scripts/scene/build.sh [--build-image] <camsim-scene args...>
# Inside: the repo is /work (read-only, cwd), the fetch cache /cache, outputs /out. Defaults:
#   CAMSIM_SCENE_IMAGE=camsim-scene:r0  CAMSIM_SCENE_CACHE=<repo>/.cache/scene  CAMSIM_SCENE_OUT=<repo>/.cache/scene-packages
# Example: scripts/scene/build.sh build /out/pendleton --config scripts/scene/examples/pendleton.toml -j 16
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMAGE=${CAMSIM_SCENE_IMAGE:-camsim-scene:r0}
CACHE=${CAMSIM_SCENE_CACHE:-$REPO/.cache/scene}
OUT=${CAMSIM_SCENE_OUT:-$REPO/.cache/scene-packages}
if [[ "${1:-}" == "--build-image" ]]; then
  shift
  docker build -t "$IMAGE" "$HERE"
fi
mkdir -p "$CACHE" "$OUT"
DIGEST=$(docker image inspect --format '{{.Id}}' "$IMAGE")
# A TTY only when both streams are terminals: with -t docker merges stderr into stdout,
# which would break `--json-progress 2> file`. ${TTY[@]+...} keeps bash 3.2 + set -u happy.
TTY=()
[[ -t 1 && -t 2 ]] && TTY=(-t)
exec docker run --rm ${TTY[@]+"${TTY[@]}"} -u "$(id -u):$(id -g)" \
  -v "$REPO:/work:ro" -v "$CACHE:/cache" -v "$OUT:/out" -w /work \
  -e CAMSIM_SCENE_IMAGE_DIGEST="$DIGEST" -e CAMSIM_SCENE_GIT_COMMIT="$(git -C "$REPO" rev-parse HEAD)" \
  "$IMAGE" "$@"
