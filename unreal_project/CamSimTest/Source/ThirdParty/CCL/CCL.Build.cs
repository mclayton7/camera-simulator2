// Copyright CamSim Contributors. All Rights Reserved.
//
// CCL.Build.cs — CIGI Class Library (LGPL-2.1) static library module.
//
// Expected directory layout:
//   Source/ThirdParty/CCL/
//     include/cigicl/         ← CCL public headers (CigiIGSession.h, etc.)
//     lib/
//       Linux/                ← libcigicl.a
//       Mac/                  ← libcigicl.a
//       Win64/                ← cigicl.lib  (OUTPUT_NAME cigicl_static on Windows → cigicl_static.lib; verify on first Win64 build)
//
// Build via: scripts/build_thirdparty_mac.sh  (or _linux.sh)

using UnrealBuildTool;
using System.IO;

public class CCL : ModuleRules
{
	public CCL(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string LibraryDir = Path.Combine(ModuleDirectory, "lib");
		string IncludeDir = Path.Combine(ModuleDirectory, "include");

		PublicIncludePaths.Add(IncludeDir);

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PublicAdditionalLibraries.Add(Path.Combine(LibraryDir, "Linux", "libcigicl.a"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicAdditionalLibraries.Add(Path.Combine(LibraryDir, "Mac", "libcigicl.a"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(Path.Combine(LibraryDir, "Win64", "cigicl_static.lib"));
		}
	}
}
