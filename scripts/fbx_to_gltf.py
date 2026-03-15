#!/usr/bin/env python3
"""Convert FBX models to glTF Binary (.glb) via Blender headless.

Usage:
    blender --background --python scripts/fbx_to_gltf.py -- model.fbx
    blender --background --python scripts/fbx_to_gltf.py -- model.fbx --output entities/

Prints a YAML snippet suitable for camsim_config.yaml entity_types block.
"""
import sys
import os
import argparse


def main():
    # Blender passes everything after '--' to the script
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]

    parser = argparse.ArgumentParser(description="FBX to glTF converter for CamSim")
    parser.add_argument("fbx_path", help="Path to the .fbx file")
    parser.add_argument("--output", "-o", default="entities/",
                        help="Output directory for .glb file (default: entities/)")
    parser.add_argument("--type-id", "-t", type=int, default=1001,
                        help="Entity type ID for YAML snippet (default: 1001)")
    parser.add_argument("--class-name", "-c", default="",
                        help="Class name for ML annotation (default: filename stem)")
    args = parser.parse_args(argv)

    fbx_path = os.path.abspath(args.fbx_path)
    if not os.path.isfile(fbx_path):
        print(f"Error: FBX file not found: {fbx_path}", file=sys.stderr)
        sys.exit(1)

    try:
        import bpy
    except ImportError:
        print("Error: This script must be run inside Blender:", file=sys.stderr)
        print("  blender --background --python scripts/fbx_to_gltf.py -- model.fbx",
              file=sys.stderr)
        sys.exit(1)

    # Clear default scene
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Import FBX
    print(f"Importing: {fbx_path}")
    bpy.ops.import_scene.fbx(filepath=fbx_path)

    # Determine output path
    stem = os.path.splitext(os.path.basename(fbx_path))[0]
    os.makedirs(args.output, exist_ok=True)
    glb_path = os.path.join(args.output, f"{stem}.glb")
    glb_path = os.path.abspath(glb_path)

    # Export as GLB
    print(f"Exporting: {glb_path}")
    bpy.ops.export_scene.gltf(
        filepath=glb_path,
        export_format='GLB',
        use_selection=False,
        export_apply=True,
    )

    # Print YAML snippet
    class_name = args.class_name if args.class_name else stem.lower()
    rel_path = f"{stem}.glb"

    print("\n# --- Add to camsim_config.yaml entity_types: ---")
    print(f'  "{args.type_id}":')
    print(f"    mesh: \"{rel_path}\"")
    print(f"    class_name: \"{class_name}\"")
    print(f"    scale: 1.0")
    print(f"    skeletal: false")
    print(f"    rotation:")
    print(f"      pitch: 0.0")
    print(f"      yaw: 0.0")
    print(f"      roll: 0.0")

    print(f"\nDone: {glb_path}")


if __name__ == "__main__":
    main()
