// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class IFileHandle;

/** One game-thread tick of render stats for the bench harness (ROADMAP 3A). */
struct FCamSimFrameStatsSample
{
	double UtcSeconds         = 0.0;  // Unix time, same clock as Python time.time()
	double WallMs             = 0.0;  // wall-clock time since the previous tick
	double GameMs             = 0.0;  // `stat unit` Game
	double RenderMs           = 0.0;  // `stat unit` Draw
	double RhiMs              = 0.0;  // `stat unit` RHIT
	double GpuMs              = 0.0;  // `stat unit` GPU
	uint64 FramesEmitted      = 0;    // frames handed to the sensor model so far
	uint64 FramesDropped      = 0;    // frames the encoder queue dropped so far
	float  MinLoadProgressPct = 100.0f;
	double Sse                = 0.0;  // current Cesium maximum screen-space error
	bool   bCameraCut         = false;
	int32  ViewFamilies       = 0;    // scene renders this frame (viewport + captures)
	float  SensorGpuMs        = -1.0f;  // GPU time of the sensor graph, -1 when unavailable (ROADMAP 3B)
	float  SensorGainEv       = 0.0f;   // log2 sensor gain
	float  SceneMedianLog2    = 0.0f;   // log2 histogram median of the detector signal
};

/** One JSON object, no trailing newline. Keys are the harness's contract (scripts/bench/analyze.py). */
FString CamSimFormatFrameStatsRow(const FCamSimFrameStatsSample& S);

/** Appends one JSON row per tick. Game thread only. A failed Open() leaves it off. */
class FCamSimFrameStatsRecorder
{
public:
	~FCamSimFrameStatsRecorder() { Close(); }

	bool Open(const FString& Path);
	void Record(const FCamSimFrameStatsSample& S);
	void Close();
	bool IsOpen() const { return Handle != nullptr; }

private:
	IFileHandle* Handle = nullptr;
	int32 RowsSinceFlush = 0;
};

/**
 * Counts scene view families as they begin rendering: the game viewport and
 * every SceneCapture. The harness uses it to prove one render per frame.
 */
class FCamSimViewFamilyCounter : public FSceneViewExtensionBase
{
public:
	explicit FCamSimViewFamilyCounter(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override
	{
		Count.Store(Count.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::Relaxed);
	}

	/** Families counted since the last call. Game thread. */
	int32 ConsumeCount() { return Count.Exchange(0); }

private:
	TAtomic<int32> Count { 0 };
};
