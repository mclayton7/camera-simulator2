// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameStats.h"
#include "CamSimTest.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

FString CamSimFormatFrameStatsRow(const FCamSimFrameStatsSample& S)
{
	// No sensor controller on the legacy path: null rather than its initial state.
	auto Num = [&S](float V) { return S.bHasSensorStats ? FString::Printf(TEXT("%.3f"), V) : FString(TEXT("null")); };
	return FString::Printf(
		TEXT("{\"t\":%.6f,\"wall_ms\":%.3f,\"game_ms\":%.3f,\"render_ms\":%.3f,\"rhi_ms\":%.3f,\"gpu_ms\":%.3f,")
		TEXT("\"emitted\":%llu,\"dropped\":%llu,\"load_pct\":%.2f,\"sse\":%.3f,\"cut\":%s,\"families\":%d,")
		TEXT("\"sensor_gpu_ms\":%.3f,\"thermal_gpu_ms\":%.3f,\"land_cover_window\":%u,\"sensor_gain_ev\":%s,\"scene_median_log2\":%s}"),
		S.UtcSeconds, S.WallMs, S.GameMs, S.RenderMs, S.RhiMs, S.GpuMs,
		static_cast<unsigned long long>(S.FramesEmitted), static_cast<unsigned long long>(S.FramesDropped),
		S.MinLoadProgressPct, S.Sse, S.bCameraCut ? TEXT("true") : TEXT("false"), S.ViewFamilies,
		S.SensorGpuMs, S.ThermalGpuMs, S.LandCoverWindow, *Num(S.SensorGainEv), *Num(S.SceneMedianLog2));
}

bool FCamSimFrameStatsRecorder::Open(const FString& Path)
{
	Close();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*FPaths::GetPath(Path));
	Handle = PF.OpenWrite(*Path, /*bAppend=*/false);
	if (!Handle)
	{
		UE_LOG(LogCamSim, Warning, TEXT("FrameStats: cannot open '%s' — frame stats disabled"), *Path);
		return false;
	}
	UE_LOG(LogCamSim, Log, TEXT("FrameStats: writing %s"), *Path);
	return true;
}

void FCamSimFrameStatsRecorder::Record(const FCamSimFrameStatsSample& S)
{
	if (!Handle) return;
	FTCHARToUTF8 Utf8(*(CamSimFormatFrameStatsRow(S) + TEXT("\n")));
	Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	// Flush about once a second so a crashed run still leaves usable data.
	if (++RowsSinceFlush >= 30)
	{
		Handle->Flush();
		RowsSinceFlush = 0;
	}
}

void FCamSimFrameStatsRecorder::Close()
{
	if (!Handle) return;
	Handle->Flush();
	delete Handle;
	Handle = nullptr;
}
