#!/usr/bin/env python3
"""Convert a split glTF (scene.gltf + scene.bin) to a self-contained .glb.

Usage:
    python3 scripts/gltf_to_glb.py entities/f16/f16-c_falcon/scene.gltf

The output .glb will be written next to the input file.
"""

import sys
import pathlib
import pygltflib

def main():
    if len(sys.argv) < 2:
        print("Usage: gltf_to_glb.py <path/to/scene.gltf>")
        sys.exit(1)

    src = pathlib.Path(sys.argv[1]).resolve()
    if not src.exists():
        print(f"Error: {src} not found")
        sys.exit(1)

    dst = src.with_suffix(".glb")

    print(f"Loading {src} ...")
    gltf = pygltflib.GLTF2().load(str(src))

    # Inline all external buffers and images into the GLB binary chunk
    gltf.convert_buffers(pygltflib.BufferFormat.BINARYBLOB)

    print(f"Writing {dst} ...")
    gltf.save_binary(str(dst))
    print(f"Done. {dst.stat().st_size / 1024:.0f} KB")

if __name__ == "__main__":
    main()
