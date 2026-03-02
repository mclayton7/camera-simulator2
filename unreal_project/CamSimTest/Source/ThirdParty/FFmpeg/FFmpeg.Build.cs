// Copyright CamSim Contributors. All Rights Reserved.
//
// FFmpeg.Build.cs — FFmpeg static library module (GPL build with libx264).
//
// Expected directory layout:
//   Source/ThirdParty/FFmpeg/
//     include/
//       libavcodec/       ← avcodec.h, etc.
//       libavformat/      ← avformat.h, etc.
//       libavutil/        ← avutil.h, opt.h, etc.
//       libswscale/       ← swscale.h
//     lib/
//       Linux/            ← libavcodec.a libavformat.a libavutil.a libswscale.a libx264.a
//       Mac/              ← same
//       Win64/            ← avcodec.lib avformat.lib avutil.lib swscale.lib x264.lib
//
// Build FFmpeg (Linux example):
//   ./configure --enable-gpl --enable-libx264 \
//               --enable-muxer=mpegts --enable-protocol=udp \
//               --enable-static --disable-shared \
//               --prefix=$(pwd)/install
//   make -j$(nproc) install
//   cp install/lib/*.a     <ThirdParty>/FFmpeg/lib/Linux/
//   cp -r install/include/ <ThirdParty>/FFmpeg/include/

using UnrealBuildTool;
using System.IO;

public class FFmpeg : ModuleRules
{
	public FFmpeg(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string LibraryDir = Path.Combine(ModuleDirectory, "lib");
		string IncludeDir = Path.Combine(ModuleDirectory, "include");

		PublicIncludePaths.Add(IncludeDir);

		// Suppress warnings from FFmpeg's C headers when included in C++ TUs
		PublicDefinitions.Add("__STDC_CONSTANT_MACROS");
		PublicDefinitions.Add("__STDC_FORMAT_MACROS");
		PublicDefinitions.Add("__STDC_LIMIT_MACROS");

		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			string PlatformDir = Path.Combine(LibraryDir, "Linux");
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavcodec.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavformat.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavutil.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libswscale.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libx264.a"));

			PublicSystemLibraries.AddRange(new string[] { "pthread", "m", "dl", "z" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			string PlatformDir = Path.Combine(LibraryDir, "Mac");
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavcodec.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavformat.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libavutil.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libswscale.a"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "libx264.a"));

			PublicFrameworks.AddRange(new string[] {
				"CoreFoundation", "VideoToolbox", "AudioToolbox", "Security"
			});
			PublicSystemLibraries.AddRange(new string[] { "bz2", "z" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string PlatformDir = Path.Combine(LibraryDir, "Win64");
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "avcodec.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "avformat.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "avutil.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "swscale.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(PlatformDir, "x264.lib"));
		}
	}
}
