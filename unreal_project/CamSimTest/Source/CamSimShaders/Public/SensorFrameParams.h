// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"

enum class ESensorGraphMode : uint32 { EO = 0, IR = 1 };

struct FSensorHistogram
{
	static constexpr int32 NumBins     = 256;
	static constexpr float MinLog2     = -16.0f;
	static constexpr float BinsPerStop = 8.0f;
	TStaticArray<uint32, NumBins> Bins;
	uint32 Serial = 0;   // FSensorFrameParams::Serial of the frame it measured
	FSensorHistogram() { for (uint32& B : Bins) B = 0; }
	uint64 Total() const { uint64 T = 0; for (uint32 B : Bins) T += B; return T; }
	static float BinCentreLog2(int32 Bin) { return MinLog2 + (Bin + 0.5f) / BinsPerStop; }
	/** Histogram bin of a detector signal; <= 2^MinLog2, 0 and NaN land in bin 0. */
	static int32 BinOf(float Signal)
	{
		if (!(Signal > 0.0f)) return 0;
		const int32 B = FMath::FloorToInt32((FMath::Log2(Signal) - MinLog2) * BinsPerStop);
		return FMath::Clamp(B, 0, NumBins - 1);
	}
};

/** Everything one frame of the sensor graph needs besides its input textures. */
struct FSensorFrameParams
{
	ESensorGraphMode Mode = ESensorGraphMode::EO;
	uint32    bBlackHot     = 0;          // IR polarity: 1 inverts after the display mapping
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);
	float     InputScale    = 1.0f;       // multiplies scene colour; tests only (runtime uses View.OneOverPreExposure)
	uint32    Serial        = 0;          // tags the histogram this frame produces

	// Exposure (ROADMAP 3B.2). Until the detector model lands the display path is
	// EO: rgb = Knee(c * PhotonGain * AnalogGain);
	// IR: v = DisplayGain * (s * PhotonGain * AnalogGain) + DisplayOffset.
	float PhotonGain    = 1.0f;           // signal -> fraction of full scale before noise
	float AnalogGain    = 1.0f;           // applied after detector noise (photon detectors)
	float DisplayGain   = 1.0f;           // IR AGC on normalised DN; EO 1
	float DisplayOffset = 0.0f;           // added after DisplayGain (IR AGC), normalised units
	float KneeStart     = 0.8f;           // EO soft highlight knee (linear)

	// Detector (copied from FSensorDetectorConfig; DarkE = DarkCurrentEs / frame rate)
	uint32 DetectorType = 0;              // ESensorDetectorType: 0 photon, 1 microbolometer
	float FullWellE = 10000.0f, ReadNoiseE = 2.0f, Prnu = 0.01f, DsnuE = 1.0f, DarkE = 0.0f;
	float TemporalNoise = 0.0f, PixelFpn = 0.0f, ColumnFpn = 0.0f, RowFpn = 0.0f;
	float AdcMax = 4095.0f;               // 2^bits - 1
	float HotFraction = 1e-5f, DeadFraction = 1e-5f;
	uint32 Seed = 1, FrameIndex = 0;

	// Optics (filled from Task 10; 0 = optics off)
	float FocalPx = 0.0f;
	float K1 = 0.0f, K2 = 0.0f, VignettingExponent = 0.0f;
	float PsfSigmaPx = 0.0f;              // OPTICAL sigma (CamSimOptics::PsfOpticalSigmaPx); pixel aperture comes from tap integration
};
