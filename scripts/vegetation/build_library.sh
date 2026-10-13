#!/usr/bin/env bash
# Copyright CamSim Contributors. All Rights Reserved.
# Rebuild the shipped vegetation library meshes (REALISM R4, docs/vegetation.md "Asset") and print the library.json
# entries. Colours come from scripts/vegetation/calibrate_colours.py (docs/vegetation.md "Colour calibration").
#   Q=<unzipped Quaternius "Textured Stylized Trees - May 2020"> scripts/vegetation/build_library.sh OUT_DIR
set -euo pipefail
OUT=${1:?OUT_DIR}
Q=${Q:-.cache/veg/src/quaternius/Textured Stylized Trees - May 2020}
QURL=https://quaternius.itch.io/textured-lowpoly-trees
PROC="procedural (scripts/vegetation/prepare_tree.py --procedural-shrub)"
PT=(uv run --with pillow python scripts/vegetation/prepare_tree.py)

# Calibrated colours (linear; calibrate_colours.py on pendleton-r4, 2026-10-12).
BROADLEAF_LEAVES=${BROADLEAF_LEAVES:-1.0,0.75,1.0}
BROADLEAF_FAR=${BROADLEAF_FAR:-0.542,0.514,0.383}
PINE_FAR=${PINE_FAR:-}
CHAPARRAL=${CHAPARRAL:-0.216,0.213,0.137}
DESERT=${DESERT:-0.245,0.229,0.164}

far() { [ -n "$1" ] && echo "--far-color $1" || true; }
"${PT[@]}" "$Q/OBJ/Tree_7.obj" --name tree_broadleaf_a --height-m 9.0 --crown-radius-m 2.7 --max-tris 10000 \
  --licence CC0-1.0 --source-url "$QURL" --out "$OUT" \
  --texture "Bark=$Q/Textures/Tree_Bark.jpg" --texture "Tree_Leaves=$Q/Textures/Tree_Leaves.png" \
  --base-color "Tree_Leaves=$BROADLEAF_LEAVES" $(far "$BROADLEAF_FAR")
"${PT[@]}" "$Q/OBJ/Pine_4.obj" --name tree_pine_a --height-m 22.0 --max-tris 10000 --licence CC0-1.0 \
  --source-url "$QURL" --out "$OUT" \
  --texture "Bark=$Q/Textures/Tree_Bark.jpg" --texture "Pine_Leaves=$Q/Textures/Pine_Leaves.png" $(far "$PINE_FAR")
"${PT[@]}" "$Q/OBJ/Pine_5.obj" --name tree_pine_b --height-m 16.0 --max-tris 10000 --licence CC0-1.0 \
  --source-url "$QURL" --out "$OUT" \
  --texture "Bark=$Q/Textures/Tree_Bark.jpg" --texture "Pine_Leaves=$Q/Textures/Pine_Leaves.png" $(far "$PINE_FAR")
"${PT[@]}" --procedural-shrub --name shrub_chaparral --height-m 2.0 --crown-radius-m 1.0 --seed 11 \
  --base-color "shrub=$CHAPARRAL" --role shrub --licence CC0-1.0 --source-url "$PROC" --out "$OUT"
"${PT[@]}" --procedural-shrub --name shrub_desert --height-m 1.4 --crown-radius-m 0.7 --seed 12 \
  --base-color "shrub=$DESERT" --role shrub --licence CC0-1.0 --source-url "$PROC" --out "$OUT"
