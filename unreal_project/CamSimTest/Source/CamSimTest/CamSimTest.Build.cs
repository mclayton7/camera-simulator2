// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class CamSimTest : ModuleRules
{
	public CamSimTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// With BuildSettingsVersion.V6 + UseExplicitOrSharedPCHs, UBT no longer
		// implicitly adds the module root to the compiler -I flags.  Adding it
		// explicitly lets all subdirectory .cpp files use includes like
		// #include "Encoder/VideoEncoder.h" without further path gymnastics.
		PrivateIncludePaths.Add(ModuleDirectory);

		// Suppress warnings from FFmpeg C headers included via extern "C"
		bEnableUndefinedIdentifierWarnings = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			// Rendering
			"RenderCore",
			"RHI",
			"Renderer",
			// Networking
			"Sockets",
			"Networking",
			// Cesium
			"CesiumRuntime",
			// JSON config
			"Json",
			"JsonUtilities",
			// Runtime glTF loading for entity meshes
			"glTFRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		// ----------------------------------------------------------------
		// ThirdParty: CCL (CIGI Class Library)
		// ----------------------------------------------------------------
		string CclDir = Path.Combine(ModuleDirectory, "..", "ThirdParty", "CCL");
		PublicIncludePaths.Add(Path.Combine(CclDir, "include"));

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PublicAdditionalLibraries.Add(Path.Combine(CclDir, "lib", "Linux", "libcigicl.a"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicAdditionalLibraries.Add(Path.Combine(CclDir, "lib", "Mac", "libcigicl.a"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(Path.Combine(CclDir, "lib", "Win64", "cigicl_static.lib"));
		}

		// ----------------------------------------------------------------
		// ThirdParty: FFmpeg (libavcodec, libavformat, libavutil, libswscale)
		// ----------------------------------------------------------------
		string FfmpegDir = Path.Combine(ModuleDirectory, "..", "ThirdParty", "FFmpeg");
		PublicIncludePaths.Add(Path.Combine(FfmpegDir, "include"));

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			string FfmpegLibDir = Path.Combine(FfmpegDir, "lib", "Linux");
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavcodec.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavformat.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavutil.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libswscale.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libswresample.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libx264.a"));
			// FFmpeg static libs need these system libs
			PublicSystemLibraries.AddRange(new string[] { "pthread", "m", "dl", "z" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			string FfmpegLibDir = Path.Combine(FfmpegDir, "lib", "Mac");
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavcodec.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavformat.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libavutil.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libswscale.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libswresample.a"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "libx264.a"));
			PublicFrameworks.AddRange(new string[] { "CoreFoundation", "VideoToolbox", "AudioToolbox", "Security" });
			// iconv: used by FFmpeg's MPEG-TS/subtitle code; not auto-linked on macOS
			PublicSystemLibraries.AddRange(new string[] { "bz2", "z", "iconv" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string FfmpegLibDir = Path.Combine(FfmpegDir, "lib", "Win64");
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "avcodec.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "avformat.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "avutil.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "swscale.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(FfmpegLibDir, "x264.lib"));
			// Copy FFmpeg DLLs to output directory on Windows (if using shared libs)
			// RuntimeDependencies.Add("$(TargetOutputDir)/avcodec-61.dll", Path.Combine(FfmpegDir, "bin", "Win64", "avcodec-61.dll"));
		}
	}
}
