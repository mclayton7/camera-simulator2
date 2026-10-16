// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/SensorReference.h"
#include "Sensor/SensorTypes.h"

/** Flat-field statistics of the CPU reference detector, shared by the physics tests (SensorPhysicsTest.cpp) and the
 *  radiometric EO tests (SensorRadiometricTest.cpp). Statistics are in double. */
namespace CamSimSensorPhysicsTest
{
	inline TArray<float> Flat(int32 W, int32 H, float V) { TArray<float> A; A.Init(V, W * H); return A; }

	/** One frame of a W x H flat field at Signal through CamSimSensorRef::DetectImage (mono), noise keyed to Frame. */
	inline TArray<float> Detect(float Signal, int32 W, int32 H, const FSensorFrameParams& P, uint32 Frame)
	{
		FSensorFrameParams Q = P; Q.FrameIndex = Frame;
		return CamSimSensorRef::DetectImage(Flat(W, H, Signal), W, H, 1, Q);
	}

	inline double Mean(const TArray<double>& A) { double S = 0; for (double V : A) S += V; return S / A.Num(); }
	inline double Var(const TArray<double>& A)
	{
		const double M = Mean(A); double S = 0;
		for (double V : A) S += (V - M) * (V - M);
		return S / (A.Num() - 1);
	}

	/** Per-pixel combination of two DN images in double. */
	template <typename F> TArray<double> Combine(const TArray<float>& A, const TArray<float>& B, F Fn)
	{
		TArray<double> R; R.SetNumUninitialized(A.Num());
		for (int32 I = 0; I < A.Num(); ++I) R[I] = Fn((double)A[I], (double)B[I]);
		return R;
	}

	/** Temporal noise (electrons rms) of M's photon detector on a Size x Size flat field at Signal with PhotonGain
	 *  (electrons = Signal * PhotonGain * FullWellE): per-pixel variance over Frames frames, averaged over the pixels.
	 *  The detector's read noise, PRNU and DSNU come from M; a 16-bit ADC keeps quantisation negligible, and there is
	 *  no dark signal, analog gain or defect. */
	inline double TemporalStdElectrons(const FSensorModeConfig& M, float Signal, float PhotonGain, int32 Size, int32 Frames)
	{
		FSensorFrameParams P;
		P.DetectorType = static_cast<uint32>(ESensorDetectorType::Photon);
		P.FullWellE    = M.Detector.FullWellE;
		P.ReadNoiseE   = M.Detector.ReadNoiseE;
		P.Prnu         = M.Detector.Prnu;
		P.DsnuE        = M.Detector.DsnuE;
		P.DarkE        = 0.0f;
		P.PhotonGain   = PhotonGain;
		P.AnalogGain   = 1.0f;
		P.AdcMax       = 65535.0f;
		P.HotFraction  = 0.0f; P.DeadFraction = 0.0f;
		P.Seed         = 1;
		const int32 N = Size * Size;
		TArray<double> Sum, SumSq;
		Sum.Init(0.0, N); SumSq.Init(0.0, N);
		for (int32 F = 1; F <= Frames; ++F)
		{
			const TArray<float> D = Detect(Signal, Size, Size, P, static_cast<uint32>(F));
			for (int32 I = 0; I < N; ++I) { Sum[I] += D[I]; SumSq[I] += (double)D[I] * D[I]; }
		}
		double VarDn = 0.0;
		for (int32 I = 0; I < N; ++I) VarDn += (SumSq[I] - Sum[I] * Sum[I] / Frames) / (Frames - 1);
		VarDn /= N;
		const double EPerDn = P.FullWellE / P.AdcMax;
		return FMath::Sqrt(VarDn) * EPerDn;
	}
}
