# Ocean Surface for Boats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A globe-conforming ocean at EGM96 sea level with Beaufort/CIGI sea state, boats that ride the waves, and HAT/HOT that sees the water.

**Architecture:** Pure C++ wave maths (`FOceanWaves`) and sea surface (`FOceanSurface`, owned by the subsystem) feed three consumers: boat placement (`CamSimSurface`), HAT/HOT (`FCigiQueryHandler`) and rendering (`FOceanManager` → warped-grid `UProceduralMeshComponent` + scripted Single Layer Water material `M_Ocean` driven by `MPC_Ocean`). The GPU mirrors the CPU wave function expression for expression.

**Tech Stack:** UE 5.8 C++ (CamSimTest module), Cesium for Unreal, UE Automation tests, UE editor Python (material generation), Python 3 acceptance scripts.

**Spec:** `docs/superpowers/specs/2026-09-29-ocean-surface-design.md`

## Global Constraints

- Altitudes are WGS-84 ellipsoid metres everywhere; sea level = EGM96 geoid undulation (`CamSim::Geospatial::GetGeoidUndulation`) + CIGI tide offset.
- New interfaces use metres, degrees, seconds (sim time from `FSimClock`); UE centimetres only at the mesh/material boundary (1 UE unit = 1 cm).
- Never `SetActorRotation` with protocol angles; entity poses go through `CommitPose` / `GlobeAnchor`.
- Angles follow CIGI: pitch nose-up positive, roll right-side-down positive (as `ClampGround`).
- Ocean off ⇒ boat placement and HAT/HOT byte-identical to today.
- Max 4 waves. Σ Qᵢkᵢaᵢ ≤ 1.
- UE code style: `// Copyright CamSim Contributors. All Rights Reserved.` header, `CoreMinimal.h` first, PascalCase, tabs.
- Build: `scripts/run.sh --build-only` (macOS: never pipe through `tee` without `set -o pipefail`).
- Headless tests (macOS): `"$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject -ExecCmds="Automation RunTests <Filter>+Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath=.cache/automation-report -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput` with `UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"`; results in `.cache/automation-report/index.json` (read with `encoding="utf-8-sig"`). Below this is abbreviated as **`run_tests <Filter>`**.
- GPU tests: `scripts/run_gpu_tests.sh <Filter>`.
- Commits end with the two attribution lines:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ`.

## Review Focus

1. **Boat far from the frame centre / camera moving fast:** mesh rebuilds must not hitch (> 10 ms logged) and boats must keep floating (CPU placement never depends on the mesh). Pinned by Task 7 rebuild-policy tests and Task 11 frame-time criteria.
2. **Geoid grid missing (LFS pointer):** ocean must disable itself with a warning, not render at 0 m or place boats at 0 m. Pinned in Task 2 (`SeaLevelM` unset ⇒ `SurfaceHeightM` unset) and Task 5 (unset sea ⇒ today's behaviour).
3. **CIGI Wave Control with ID ≥ 4, zero length, or disable of an unknown ID:** ignored/logged, never crashes or produces NaN. Pinned in Task 2 tests.
4. **Stationary boat (no DR motion, no new PDUs):** still bobs with the waves. Pinned in Task 5 (re-commit each tick) and verified in Task 11.
5. **Lake above sea level / land boat near the coast:** lake height wins over sea level; ground vehicles are unaffected by the ocean. Pinned in Task 5 tests.

---

### Task 1: FOceanWaves — pure Gerstner wave maths

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanWaves.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanWaves.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/OceanWavesTest.cpp`
- Keep (read only): `Ocean/FBeaufortTable.h`

**Interfaces:**
- Consumes: `FBeaufortTable::Sample(float)`, `CamSimFrames::GeodeticToEcef(LatDeg, LonDeg, Alt)` (`Geospatial/EcefFrames.h`).
- Produces:
  ```cpp
  struct FOceanWave { double HeightM, LengthM, PeriodS, FromDeg, PhaseRad, Steepness; };
  class FOceanWaves {
    static constexpr int32 MaxWaves = 4; static constexpr double G = 9.80665;
    static TArray<FOceanWave> FromBeaufort(double Beaufort, double FromDeg, double Choppiness);
    static void AssignSteepness(TArray<FOceanWave>& Waves, double Choppiness);
    void SetWaves(const TArray<FOceanWave>&); const TArray<FOceanWave>& GetWaves() const;
    void SetAnchor(double Lat, double Lon); bool HasAnchor() const; double GetAnchorLat() const; double GetAnchorLon() const;
    FVector GetAnchorEcef() const; FVector GetAxisNorthEcef() const; FVector GetAxisEastEcef() const; FVector GetAxisUpEcef() const;
    void SetTime(double SimSeconds); double GetTime() const;
    double WaveNumber(int32 i) const; double AngularFrequency(int32 i) const; FVector2D TravelDir(int32 i) const; double Phase(int32 i) const;
    FVector2D PlaneCoords(double Lat, double Lon, double AltM) const;   // (N, E) metres
    FVector Displacement(double N, double E) const;                      // (dN, dE, dZ)
    double HeightAtPlane(double N, double E) const; FVector NormalAtPlane(double N, double E) const;
    double HeightAt(double Lat, double Lon, double SeaAltM) const; FVector NormalAt(double Lat, double Lon, double SeaAltM) const;
    double SignificantHeight() const; };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/OceanWavesTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanWaves.h"
#include "Ocean/FBeaufortTable.h"
#include "Geospatial/EcefFrames.h"
#include "Geospatial/CigiFrames.h"

namespace
{
	FOceanWave Wave(double H, double L, double From, double Q = 0.0, double T = 0.0)
	{
		FOceanWave W; W.HeightM = H; W.LengthM = L; W.FromDeg = From; W.Steepness = Q; W.PeriodS = T; return W;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesBeaufortTest, "CamSim.Ocean.Waves.BeaufortSignificantHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesBeaufortTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Beaufort 0 → no waves"), FOceanWaves::FromBeaufort(0.0, 270.0, 0.5).Num(), 0);
	for (int32 B = 1; B <= 12; ++B)
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(B, 270.0, 0.5));
		const double Table = FBeaufortTable::Sample(B).WaveHtM;
		TestEqual(*FString::Printf(TEXT("Beaufort %d: 4 waves"), B), W.GetWaves().Num(), 4);
		TestEqual(*FString::Printf(TEXT("Beaufort %d: Hs"), B), W.SignificantHeight(), Table, Table * 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesSteepnessTest, "CamSim.Ocean.Waves.SteepnessBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesSteepnessTest::RunTest(const FString& Parameters)
{
	for (double C : { 0.0, 0.5, 1.0, 3.0 })
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(6.0, 200.0, C));
		double Sum = 0.0;
		for (int32 i = 0; i < W.GetWaves().Num(); ++i)
		{
			Sum += W.GetWaves()[i].Steepness * W.WaveNumber(i) * W.GetWaves()[i].HeightM * 0.5;
		}
		TestTrue(*FString::Printf(TEXT("choppiness %.1f: sum Q k a = %.4f <= 1"), C, Sum), Sum <= 1.0 + 1e-9);
		TestEqual(*FString::Printf(TEXT("choppiness %.1f: sum = min(C, 1)"), C), Sum, FMath::Min(C, 1.0), 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesDispersionTest, "CamSim.Ocean.Waves.Dispersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesDispersionTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(1.0, 100.0, 0.0), Wave(1.0, 100.0, 0.0, 0.0, 8.0) });
	TestEqual(TEXT("deep water: w = sqrt(g k)"), W.AngularFrequency(0), FMath::Sqrt(FOceanWaves::G * 2.0 * PI / 100.0), 1e-12);
	TestEqual(TEXT("host period: w = 2 pi / T"), W.AngularFrequency(1), 2.0 * PI / 8.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesDirectionTest, "CamSim.Ocean.Waves.DirectionFrom270MovesEast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesDirectionTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(2.0, 60.0, 270.0) });
	const FVector2D D = W.TravelDir(0);
	TestEqual(TEXT("travel north component"), D.X, 0.0, 1e-12);
	TestEqual(TEXT("travel east component"), D.Y, 1.0, 1e-12);
	const double C = W.AngularFrequency(0) / W.WaveNumber(0);   // phase speed
	W.SetTime(1000.0);
	const double H0 = W.HeightAtPlane(0.0, 10.0);
	W.SetTime(1002.0);
	TestEqual(TEXT("crest pattern moved east by c*dt"), W.HeightAtPlane(0.0, 10.0 + 2.0 * C), H0, 1e-6);
	TestTrue(TEXT("not west"), FMath::Abs(W.HeightAtPlane(0.0, 10.0 - 2.0 * C) - H0) > 1e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesPlaneTest, "CamSim.Ocean.Waves.PlaneCoordsAreEcefProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesPlaneTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetAnchor(37.8, -122.45);
	TestTrue(TEXT("anchor set"), W.HasAnchor());
	// A point ~200 km NE: plane coords = (ECEF - anchor ECEF) . axes.
	double Lat, Lon, Alt;
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(140000.0, 140000.0, 0.0), Lat, Lon, Alt);
	const FVector Rel = CamSimFrames::GeodeticToEcef(Lat, Lon, -30.0) - W.GetAnchorEcef();
	const FVector2D P = W.PlaneCoords(Lat, Lon, -30.0);
	TestEqual(TEXT("north"), P.X, FVector::DotProduct(Rel, W.GetAxisNorthEcef()), 1e-3);
	TestEqual(TEXT("east"),  P.Y, FVector::DotProduct(Rel, W.GetAxisEastEcef()),  1e-3);
	TestTrue(TEXT("roughly 140 km north"), FMath::Abs(P.X - 140000.0) < 2000.0);
	TestEqual(TEXT("axes orthonormal"), FVector::DotProduct(W.GetAxisNorthEcef(), W.GetAxisEastEcef()), 0.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesInvertTest, "CamSim.Ocean.Waves.HeightMatchesBruteForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesInvertTest::RunTest(const FString& Parameters)
{
	for (const double Chop : { 0.8, 1.0 })
	{
		FOceanWaves W;
		W.SetWaves(FOceanWaves::FromBeaufort(6.0, 240.0, Chop));
		W.SetTime(1234.5);
		const double Tol = Chop < 0.9 ? 0.01 : 0.05;
		double MaxErr = 0.0;
		for (int32 s = 0; s < 40; ++s)
		{
			const double N = 3.1 * s, E = -2.3 * s;
			// Brute force: search the parameter point whose displaced position lands on (N, E).
			double Best = TNumericLimits<double>::Max(), BestZ = 0.0;
			for (double pn = N - 6.0; pn <= N + 6.0; pn += 0.05)
			for (double pe = E - 6.0; pe <= E + 6.0; pe += 0.05)
			{
				const FVector D = W.Displacement(pn, pe);
				const double Err = FMath::Square(pn + D.X - N) + FMath::Square(pe + D.Y - E);
				if (Err < Best) { Best = Err; BestZ = D.Z; }
			}
			MaxErr = FMath::Max(MaxErr, FMath::Abs(W.HeightAtPlane(N, E) - BestZ));
		}
		TestTrue(*FString::Printf(TEXT("choppiness %.1f: max |height - brute| = %.4f m <= %.2f"), Chop, MaxErr, Tol), MaxErr <= Tol);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesNormalTest, "CamSim.Ocean.Waves.NormalSingleWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesNormalTest::RunTest(const FString& Parameters)
{
	// Q = 0 sine along north: z = a cos(k N + phase); slope dz/dN = -a k sin(...).
	FOceanWaves W;
	W.SetWaves({ Wave(2.0, 40.0, 180.0) });   // from south → travels north
	W.SetTime(0.0);
	const double k = W.WaveNumber(0), a = 1.0;
	for (double N : { 0.0, 3.0, 7.5, 12.0 })
	{
		const FVector Nrm = W.NormalAtPlane(N, 0.0);
		const double Slope = -a * k * FMath::Sin(k * N + W.Phase(0));
		const FVector Expected = FVector(-Slope, 0.0, 1.0).GetSafeNormal();
		TestTrue(*FString::Printf(TEXT("normal at N=%.1f"), N), Nrm.Equals(Expected, 1e-9));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanWavesTimeTest, "CamSim.Ocean.Waves.PhaseFromSimTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanWavesTimeTest::RunTest(const FString& Parameters)
{
	FOceanWaves W;
	W.SetWaves({ Wave(1.0, 30.0, 90.0) });
	W.SetTime(1.7e9);   // Unix-epoch sim seconds: phase stays precise
	const double P0 = W.Phase(0);
	TestTrue(TEXT("phase wrapped"), P0 >= 0.0 && P0 < 2.0 * PI);
	W.SetTime(1.7e9);
	TestEqual(TEXT("frozen time → same surface"), W.Phase(0), P0, 0.0);
	W.SetTime(1.7e9 + 1.0);
	const double Expected = FMath::Fmod(P0 - W.AngularFrequency(0) + 4.0 * PI, 2.0 * PI);
	TestEqual(TEXT("1 s later: phase - w"), W.Phase(0), Expected, 1e-5);
	return true;
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile error — `Ocean/OceanWaves.h` not found.

- [ ] **Step 3: Implement** — `Ocean/OceanWaves.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** One Gerstner wave. Heights/lengths in metres, directions true-north degrees. */
struct FOceanWave
{
	double HeightM   = 0.0;  // crest-to-trough; amplitude a = HeightM / 2
	double LengthM   = 0.0;
	double PeriodS   = 0.0;  // 0 → deep-water dispersion
	double FromDeg   = 0.0;  // direction the wave comes FROM (travels toward FromDeg + 180)
	double PhaseRad  = 0.0;
	double Steepness = 0.0;  // Gerstner Q (AssignSteepness keeps sum Q k a <= 1)
};

/**
 * Sum of up to four Gerstner waves on the tangent plane of a fixed anchor.
 * Plane coordinates (N, E) of a point: its ECEF position minus the anchor's,
 * projected on the anchor's north/east axes (exact, rigid — the GPU does the
 * same with UE world positions). Pure: no UObjects, no engine state.
 *
 * theta_i = k_i (d_i . x) + Phase(i), Phase(i) = PhaseRad_i - w_i t (wrapped)
 * displacement: horizontal -sum Q_i a_i d_i sin theta_i, vertical sum a_i cos theta_i
 * M_Ocean's custom HLSL mirrors Displacement/NormalAtPlane expression for expression.
 */
class CAMSIMTEST_API FOceanWaves
{
public:
	static constexpr int32  MaxWaves = 4;
	static constexpr double G        = 9.80665;

	static TArray<FOceanWave> FromBeaufort(double Beaufort, double FromDeg, double Choppiness);
	/** Q_i = min(C, 1) / (k_i a_i n) over the n waves with k a > 0, so sum Q k a = min(C, 1). */
	static void AssignSteepness(TArray<FOceanWave>& Waves, double Choppiness);

	/** Keeps the first MaxWaves; drops waves with non-finite or non-positive height/length. */
	void SetWaves(const TArray<FOceanWave>& InWaves);
	const TArray<FOceanWave>& GetWaves() const { return Waves; }

	void   SetAnchor(double Lat, double Lon);
	bool   HasAnchor() const { return bHasAnchor; }
	double GetAnchorLat() const { return AnchorLat; }
	double GetAnchorLon() const { return AnchorLon; }
	FVector GetAnchorEcef() const { return AnchorEcef; }
	FVector GetAxisNorthEcef() const { return AxisN; }
	FVector GetAxisEastEcef() const { return AxisE; }
	FVector GetAxisUpEcef() const { return AxisU; }

	void   SetTime(double SimSeconds) { TimeS = SimSeconds; }
	double GetTime() const { return TimeS; }

	double    WaveNumber(int32 i) const;         // 2 pi / L
	double    AngularFrequency(int32 i) const;   // sqrt(g k) or 2 pi / T
	FVector2D TravelDir(int32 i) const;          // unit (N, E)
	double    Phase(int32 i) const;              // PhaseRad - w t, wrapped to [0, 2 pi)
	double    Amplitude(int32 i) const { return Waves[i].HeightM * 0.5; }

	FVector2D PlaneCoords(double Lat, double Lon, double AltM) const;
	FVector   Displacement(double N, double E) const;      // (dN, dE, dZ) at a parameter point
	double    HeightAtPlane(double N, double E) const;     // at the point the surface lands on (N, E)
	FVector   NormalAtPlane(double N, double E) const;     // unit (N, E, Up)
	double    HeightAt(double Lat, double Lon, double SeaAltM) const;
	FVector   NormalAt(double Lat, double Lon, double SeaAltM) const;
	double    SignificantHeight() const;               // 4 sqrt(sum a^2 / 2)

private:
	FVector2D Invert(double N, double E) const;          // parameter point landing on (N, E)
	FVector   NormalAtParam(double N, double E) const;

	TArray<FOceanWave> Waves;
	bool    bHasAnchor = false;
	double  AnchorLat = 0.0, AnchorLon = 0.0;
	FVector AnchorEcef = FVector::ZeroVector;
	FVector AxisN = FVector::ZeroVector, AxisE = FVector::ZeroVector, AxisU = FVector::ZeroVector;
	double  TimeS = 0.0;
};
```

`Ocean/OceanWaves.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanWaves.h"
#include "Ocean/FBeaufortTable.h"
#include "Geospatial/EcefFrames.h"

namespace
{
	double Wrap2Pi(double X)
	{
		const double R = FMath::Fmod(X, 2.0 * PI);
		return R < 0.0 ? R + 2.0 * PI : R;
	}
}

TArray<FOceanWave> FOceanWaves::FromBeaufort(double Beaufort, double FromDeg, double Choppiness)
{
	const FBeaufortEntry E = FBeaufortTable::Sample(static_cast<float>(Beaufort));
	TArray<FOceanWave> Out;
	if (E.WaveHtM <= 0.0f || E.WaveLenM <= 0.0f) return Out;

	static constexpr double LenMul[MaxWaves] = { 0.6, 0.85, 1.1, 1.4 };
	static constexpr double DirOff[MaxWaves] = { -30.0, -10.0, 10.0, 30.0 };
	static constexpr double Weight[MaxWaves] = { 0.2, 0.35, 0.3, 0.15 };
	double SumW2 = 0.0;
	for (double W : Weight) SumW2 += W * W;
	// Hs = 4 sqrt(sum a_i^2 / 2) with a_i = s w_i
	const double S = E.WaveHtM / (4.0 * FMath::Sqrt(SumW2 / 2.0));
	for (int32 i = 0; i < MaxWaves; ++i)
	{
		FOceanWave W;
		W.HeightM  = 2.0 * S * Weight[i];
		W.LengthM  = E.WaveLenM * LenMul[i];
		W.FromDeg  = FromDeg + DirOff[i];
		W.PhaseRad = 1.7 * i;   // decorrelate the crests
		Out.Add(W);
	}
	AssignSteepness(Out, Choppiness);
	return Out;
}

void FOceanWaves::AssignSteepness(TArray<FOceanWave>& InWaves, double Choppiness)
{
	const double C = FMath::Clamp(Choppiness, 0.0, 1.0);
	int32 N = 0;
	for (const FOceanWave& W : InWaves) { if (W.HeightM > 0.0 && W.LengthM > 0.0) ++N; }
	for (FOceanWave& W : InWaves)
	{
		const double KA = (W.LengthM > 0.0) ? (2.0 * PI / W.LengthM) * W.HeightM * 0.5 : 0.0;
		W.Steepness = (KA > 0.0 && N > 0) ? C / (KA * N) : 0.0;
	}
}

void FOceanWaves::SetWaves(const TArray<FOceanWave>& InWaves)
{
	Waves.Reset();
	for (const FOceanWave& W : InWaves)
	{
		const bool bValid = FMath::IsFinite(W.HeightM) && FMath::IsFinite(W.LengthM) && W.HeightM > 0.0 && W.LengthM > 0.0
			&& FMath::IsFinite(W.PeriodS) && W.PeriodS >= 0.0 && FMath::IsFinite(W.FromDeg) && FMath::IsFinite(W.PhaseRad)
			&& FMath::IsFinite(W.Steepness);
		if (bValid && Waves.Num() < MaxWaves) Waves.Add(W);
	}
}

void FOceanWaves::SetAnchor(double Lat, double Lon)
{
	AnchorLat = Lat; AnchorLon = Lon; bHasAnchor = true;
	AnchorEcef = CamSimFrames::GeodeticToEcef(Lat, Lon, 0.0);
	const double La = FMath::DegreesToRadians(Lat), Lo = FMath::DegreesToRadians(Lon);
	AxisE = FVector(-FMath::Sin(Lo), FMath::Cos(Lo), 0.0);
	AxisN = FVector(-FMath::Sin(La) * FMath::Cos(Lo), -FMath::Sin(La) * FMath::Sin(Lo), FMath::Cos(La));
	AxisU = FVector(FMath::Cos(La) * FMath::Cos(Lo), FMath::Cos(La) * FMath::Sin(Lo), FMath::Sin(La));
}

double FOceanWaves::WaveNumber(int32 i) const { return 2.0 * PI / Waves[i].LengthM; }

double FOceanWaves::AngularFrequency(int32 i) const
{
	return Waves[i].PeriodS > 0.0 ? 2.0 * PI / Waves[i].PeriodS : FMath::Sqrt(G * WaveNumber(i));
}

FVector2D FOceanWaves::TravelDir(int32 i) const
{
	const double B = FMath::DegreesToRadians(Waves[i].FromDeg + 180.0);
	FVector2D D(FMath::Cos(B), FMath::Sin(B));
	if (FMath::Abs(D.X) < 1e-15) D.X = 0.0;
	if (FMath::Abs(D.Y) < 1e-15) D.Y = 0.0;
	return D;
}

double FOceanWaves::Phase(int32 i) const
{
	return Wrap2Pi(Wrap2Pi(Waves[i].PhaseRad) - Wrap2Pi(AngularFrequency(i) * TimeS));
}

FVector2D FOceanWaves::PlaneCoords(double Lat, double Lon, double AltM) const
{
	const FVector Rel = CamSimFrames::GeodeticToEcef(Lat, Lon, AltM) - AnchorEcef;
	return FVector2D(FVector::DotProduct(Rel, AxisN), FVector::DotProduct(Rel, AxisE));
}

FVector FOceanWaves::Displacement(double N, double E) const
{
	FVector D = FVector::ZeroVector;
	for (int32 i = 0; i < Waves.Num(); ++i)
	{
		const FVector2D Dir = TravelDir(i);
		const double a = Amplitude(i), Th = WaveNumber(i) * (Dir.X * N + Dir.Y * E) + Phase(i);
		const double S = FMath::Sin(Th), C = FMath::Cos(Th), Qa = Waves[i].Steepness * a;
		D.X -= Qa * Dir.X * S;
		D.Y -= Qa * Dir.Y * S;
		D.Z += a * C;
	}
	return D;
}

FVector2D FOceanWaves::Invert(double N, double E) const
{
	FVector2D P(N, E);
	for (int32 It = 0; It < 8; ++It)
	{
		const FVector D = Displacement(P.X, P.Y);
		const FVector2D Next(N - D.X, E - D.Y);
		const bool bDone = FVector2D::DistSquared(Next, P) < 1e-6;
		P = Next;
		if (bDone) break;
	}
	return P;
}

double FOceanWaves::HeightAtPlane(double N, double E) const
{
	if (Waves.Num() == 0) return 0.0;
	const FVector2D P = Invert(N, E);
	return Displacement(P.X, P.Y).Z;
}

FVector FOceanWaves::NormalAtParam(double N, double E) const
{
	FVector Nrm(0.0, 0.0, 1.0);
	for (int32 i = 0; i < Waves.Num(); ++i)
	{
		const FVector2D Dir = TravelDir(i);
		const double k = WaveNumber(i), a = Amplitude(i), Th = k * (Dir.X * N + Dir.Y * E) + Phase(i);
		const double KA = k * a;
		Nrm.X += Dir.X * KA * FMath::Sin(Th);
		Nrm.Y += Dir.Y * KA * FMath::Sin(Th);
		Nrm.Z -= Waves[i].Steepness * KA * FMath::Cos(Th);
	}
	return Nrm.GetSafeNormal();
}

FVector FOceanWaves::NormalAtPlane(double N, double E) const
{
	if (Waves.Num() == 0) return FVector(0.0, 0.0, 1.0);
	const FVector2D P = Invert(N, E);
	return NormalAtParam(P.X, P.Y);
}

double FOceanWaves::HeightAt(double Lat, double Lon, double SeaAltM) const
{
	const FVector2D P = PlaneCoords(Lat, Lon, SeaAltM);
	return HeightAtPlane(P.X, P.Y);
}

FVector FOceanWaves::NormalAt(double Lat, double Lon, double SeaAltM) const
{
	const FVector2D P = PlaneCoords(Lat, Lon, SeaAltM);
	return NormalAtPlane(P.X, P.Y);
}

double FOceanWaves::SignificantHeight() const
{
	double S = 0.0;
	for (int32 i = 0; i < Waves.Num(); ++i) S += FMath::Square(Amplitude(i)) / 2.0;
	return 4.0 * FMath::Sqrt(S);
}
```

Note on `NormalSingleWave`: with Q = 0 the normal from `NormalAtParam` is (k a sin θ · d, 1) which equals (−slope, 1) with slope = −a k sin θ along the travel direction; the test's `Wave(2.0, 40.0, 180.0)` travels north (d = (1, 0)), anchor unset is fine for plane-coordinate calls.

- [ ] **Step 4: Build and run**

Run: `scripts/run.sh --build-only && run_tests CamSim.Ocean.Waves`
Expected: 8 tests pass.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanWaves.* unreal_project/CamSimTest/Source/CamSimTest/Tests/OceanWavesTest.cpp
git commit -m "feat(ocean): FOceanWaves — Gerstner sum on a fixed ECEF tangent plane"
```

---

### Task 2: FOceanSurface — sea level + wave sources

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanSurface.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanSurface.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/OceanSurfaceTest.cpp`

**Interfaces:**
- Consumes: Task 1 `FOceanWaves`, `FOceanWave`; `CamSim::Geospatial::GetGeoidUndulation(double, double) → TOptional<double>` (`Geospatial/Geoid.h`).
- Produces:
  ```cpp
  class FOceanSurface {
    using FGeoidFn = TFunction<TOptional<double>(double Lat, double Lon)>;
    explicit FOceanSurface(FGeoidFn InGeoid = FGeoidFn());   // empty → GetGeoidUndulation
    void SetBeaufort(double Beaufort, double FromDeg, double Choppiness);
    void SetHostWave(int32 WaveId, const TOptional<FOceanWave>& Wave); // unset = remove; Id outside [0,4) ignored
    bool HasHostWaves() const;
    void SetTideOffsetM(double M); double GetTideOffsetM() const;
    void SetClarity(double C); double GetClarity() const;             // [0,1], default 1
    void SetWaterTempC(double C); double GetWaterTempC() const;
    void SetTime(double SimSeconds);
    void SetAnchor(double Lat, double Lon);
    const FOceanWaves& GetWaves() const;
    TOptional<double>  SeaLevelM(double Lat, double Lon) const;       // geoid + tide
    TOptional<double>  SurfaceHeightM(double Lat, double Lon) const;  // sea level + waves
    TOptional<FVector> SurfaceNormalNeu(double Lat, double Lon) const; // unit (N, E, Up)
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/OceanSurfaceTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanSurface.h"

namespace
{
	FOceanSurface MakeSurface(double Geoid = -32.0)
	{
		return FOceanSurface([Geoid](double, double) { return TOptional<double>(Geoid); });
	}
	FOceanWave HostWave(double H, double L, double From)
	{
		FOceanWave W; W.HeightM = H; W.LengthM = L; W.FromDeg = From; return W;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceSeaLevelTest, "CamSim.Ocean.Surface.SeaLevelIsGeoidPlusTide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceSeaLevelTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-32.0);
	S.SetAnchor(37.8, -122.4);
	TestEqual(TEXT("geoid"), S.SeaLevelM(37.8, -122.4).Get(0.0), -32.0, 1e-12);
	S.SetTideOffsetM(1.5);
	TestEqual(TEXT("geoid + tide"), S.SeaLevelM(37.8, -122.4).Get(0.0), -30.5, 1e-12);
	TestEqual(TEXT("calm: surface = sea level"), S.SurfaceHeightM(37.8, -122.4).Get(0.0), -30.5, 1e-12);

	FOceanSurface NoGrid([](double, double) { return TOptional<double>(); });
	NoGrid.SetAnchor(37.8, -122.4);
	TestFalse(TEXT("no geoid → no sea level"), NoGrid.SeaLevelM(37.8, -122.4).IsSet());
	TestFalse(TEXT("no geoid → no surface"), NoGrid.SurfaceHeightM(37.8, -122.4).IsSet());
	TestFalse(TEXT("no geoid → no normal"), NoGrid.SurfaceNormalNeu(37.8, -122.4).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceSourcesTest, "CamSim.Ocean.Surface.HostWavesReplaceBeaufort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceSourcesTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface();
	S.SetAnchor(37.8, -122.4);
	S.SetBeaufort(5.0, 270.0, 0.5);
	TestEqual(TEXT("Beaufort: 4 waves"), S.GetWaves().GetWaves().Num(), 4);

	S.SetHostWave(2, HostWave(1.0, 50.0, 90.0));
	TestTrue(TEXT("host waves active"), S.HasHostWaves());
	TestEqual(TEXT("host replaces Beaufort"), S.GetWaves().GetWaves().Num(), 1);
	TestEqual(TEXT("host wave height"), S.GetWaves().GetWaves()[0].HeightM, 1.0, 1e-12);

	S.SetHostWave(0, HostWave(0.5, 20.0, 0.0));
	TestEqual(TEXT("two host waves"), S.GetWaves().GetWaves().Num(), 2);
	TestEqual(TEXT("ordered by ID"), S.GetWaves().GetWaves()[0].LengthM, 20.0, 1e-12);

	S.SetHostWave(7, HostWave(1.0, 10.0, 0.0));   // ID >= 4: ignored
	TestEqual(TEXT("ID 7 ignored"), S.GetWaves().GetWaves().Num(), 2);
	S.SetHostWave(3, {});                          // removing an unknown ID: no-op
	TestEqual(TEXT("unknown removal no-op"), S.GetWaves().GetWaves().Num(), 2);
	S.SetHostWave(1, HostWave(1.0, 0.0, 0.0));     // zero length: dropped by SetWaves, no NaN
	TestEqual(TEXT("zero length dropped"), S.GetWaves().GetWaves().Num(), 2);
	TestTrue(TEXT("finite height"), FMath::IsFinite(S.SurfaceHeightM(37.8, -122.4).Get(TNumericLimits<double>::Max())));

	S.SetHostWave(0, {}); S.SetHostWave(1, {}); S.SetHostWave(2, {});
	TestFalse(TEXT("all removed"), S.HasHostWaves());
	TestEqual(TEXT("back to Beaufort"), S.GetWaves().GetWaves().Num(), 4);

	S.SetBeaufort(0.0, 270.0, 0.5);
	TestEqual(TEXT("Beaufort 0: calm"), S.GetWaves().GetWaves().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceWavesTest, "CamSim.Ocean.Surface.WavesMoveTheSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceWavesTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-32.0);
	S.SetAnchor(37.8, -122.4);
	S.SetHostWave(0, HostWave(2.0, 60.0, 270.0));
	double Min = 1e9, Max = -1e9;
	for (int32 i = 0; i < 60; ++i)
	{
		S.SetTime(i * 0.2);
		const double H = S.SurfaceHeightM(37.8, -122.4).GetValue();
		Min = FMath::Min(Min, H); Max = FMath::Max(Max, H);
	}
	TestEqual(TEXT("crest ≈ sea level + a"), Max, -31.0, 0.05);
	TestEqual(TEXT("trough ≈ sea level - a"), Min, -33.0, 0.05);
	const FVector N = S.SurfaceNormalNeu(37.8, -122.4).GetValue();
	TestTrue(TEXT("normal points up"), N.Z > 0.9 && FMath::IsNearlyEqual(N.Size(), 1.0, 1e-9));
	return true;
}
```

- [ ] **Step 2: Run to verify they fail** — `scripts/run.sh --build-only` → compile error (`Ocean/OceanSurface.h` missing).

- [ ] **Step 3: Implement** — `Ocean/OceanSurface.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanWaves.h"

/**
 * The sea: EGM96 sea level (+ CIGI tide offset) plus the active wave set.
 * Waves come from the config's Beaufort state, or — while any CIGI Wave
 * Control wave is enabled — from the host's waves (IDs 0..3, by ID).
 * Game thread only. Owned by UCamSimSubsystem; read by boat placement,
 * HAT/HOT and FOceanManager (rendering).
 */
class CAMSIMTEST_API FOceanSurface
{
public:
	using FGeoidFn = TFunction<TOptional<double>(double Lat, double Lon)>;

	/** Empty InGeoid → CamSim::Geospatial::GetGeoidUndulation. */
	explicit FOceanSurface(FGeoidFn InGeoid = FGeoidFn());

	void SetBeaufort(double Beaufort, double FromDeg, double Choppiness);
	void SetHostWave(int32 WaveId, const TOptional<FOceanWave>& Wave);
	bool HasHostWaves() const { return HostWaves.Num() > 0; }

	void   SetTideOffsetM(double M) { TideOffsetM = FMath::IsFinite(M) ? M : 0.0; }
	double GetTideOffsetM() const { return TideOffsetM; }
	void   SetClarity(double C) { Clarity = FMath::IsFinite(C) ? FMath::Clamp(C, 0.0, 1.0) : 1.0; }
	double GetClarity() const { return Clarity; }
	void   SetWaterTempC(double C) { WaterTempC = C; }
	double GetWaterTempC() const { return WaterTempC; }

	void SetTime(double SimSeconds) { Waves.SetTime(SimSeconds); }
	void SetAnchor(double Lat, double Lon) { Waves.SetAnchor(Lat, Lon); }
	const FOceanWaves& GetWaves() const { return Waves; }

	TOptional<double>  SeaLevelM(double Lat, double Lon) const;
	TOptional<double>  SurfaceHeightM(double Lat, double Lon) const;
	TOptional<FVector> SurfaceNormalNeu(double Lat, double Lon) const;

private:
	void RebuildWaves();

	FGeoidFn Geoid;
	FOceanWaves Waves;
	TSortedMap<int32, FOceanWave> HostWaves;
	double Beaufort = 0.0, FromDeg = 270.0, Choppiness = 0.5;
	double TideOffsetM = 0.0, Clarity = 1.0, WaterTempC = 15.0;
};
```

`Ocean/OceanSurface.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanSurface.h"
#include "Geospatial/Geoid.h"

FOceanSurface::FOceanSurface(FGeoidFn InGeoid)
	: Geoid(InGeoid ? MoveTemp(InGeoid) : FGeoidFn([](double Lat, double Lon) { return CamSim::Geospatial::GetGeoidUndulation(Lat, Lon); }))
{
}

void FOceanSurface::SetBeaufort(double InBeaufort, double InFromDeg, double InChoppiness)
{
	Beaufort = InBeaufort; FromDeg = InFromDeg; Choppiness = InChoppiness;
	RebuildWaves();
}

void FOceanSurface::SetHostWave(int32 WaveId, const TOptional<FOceanWave>& Wave)
{
	if (WaveId < 0 || WaveId >= FOceanWaves::MaxWaves) return;
	if (Wave.IsSet()) HostWaves.Add(WaveId, *Wave);
	else              HostWaves.Remove(WaveId);
	RebuildWaves();
}

void FOceanSurface::RebuildWaves()
{
	if (HostWaves.Num() == 0)
	{
		Waves.SetWaves(FOceanWaves::FromBeaufort(Beaufort, FromDeg, Choppiness));
		return;
	}
	TArray<FOceanWave> Host;
	for (const TPair<int32, FOceanWave>& P : HostWaves) Host.Add(P.Value);
	FOceanWaves::AssignSteepness(Host, Choppiness);
	Waves.SetWaves(Host);
}

TOptional<double> FOceanSurface::SeaLevelM(double Lat, double Lon) const
{
	const TOptional<double> G = Geoid(Lat, Lon);
	if (!G.IsSet() || !FMath::IsFinite(*G)) return {};
	return *G + TideOffsetM;
}

TOptional<double> FOceanSurface::SurfaceHeightM(double Lat, double Lon) const
{
	const TOptional<double> Sea = SeaLevelM(Lat, Lon);
	if (!Sea.IsSet()) return {};
	return *Sea + Waves.HeightAt(Lat, Lon, *Sea);
}

TOptional<FVector> FOceanSurface::SurfaceNormalNeu(double Lat, double Lon) const
{
	const TOptional<double> Sea = SeaLevelM(Lat, Lon);
	if (!Sea.IsSet()) return {};
	return Waves.NormalAt(Lat, Lon, *Sea);
}
```

(`AssignSteepness` counts only waves with positive height/length, and `SetWaves` drops invalid ones, so a zero-length host wave neither contributes nor produces NaN.)

- [ ] **Step 4: Build and run** — `scripts/run.sh --build-only && run_tests CamSim.Ocean` → all `CamSim.Ocean.Waves.*` and `CamSim.Ocean.Surface.*` pass.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanSurface.* unreal_project/CamSimTest/Source/CamSimTest/Tests/OceanSurfaceTest.cpp
git commit -m "feat(ocean): FOceanSurface — sea level (geoid + tide) and Beaufort/host wave sources"
```

---

### Task 3: Config `ocean:`, remove the Phase 19 flat plane, subsystem owns the surface

**Files:**
- Modify: `Config/CamSimConfig.h` (`FPhase19Config` → `FOceanConfig`, member `Phase19` → `Ocean`)
- Modify: `Config/CamSimConfig.cpp` (YAML `phase19:` block ~L962–978 → `ocean:`; env block ~L1292–1303; `KeepRestartOnlySettings`)
- Modify: `deploy/camsim_config.yaml` (replace `phase19:` block, ~L745–764)
- Modify: `docs/configuration.md` (replace the Phase 19 section)
- Delete: `Ocean/FGerstnerOceanSurface.h/.cpp`, `Ocean/IOceanSurface.h`, `Tests/Phase19OceanTest.cpp`
- Rewrite: `Ocean/FOceanManager.h/.cpp` (render-less skeleton for now)
- Modify: `Entity/CamSimEntity.h/.cpp` (delete `ApplyVesselMotion`), `Entity/CamSimEntityManager.h/.cpp` (delete `SetOceanSurface`, `OceanSurface` member, the Phase 19C block ~L272–279)
- Modify: `Environment/CamSimEnvironment.h/.cpp` (new manager API; see below)
- Modify: `Subsystem/CamSimSubsystem.h/.cpp` (own `TUniquePtr<FOceanSurface>`, pre-actor-tick time, hot reload)
- Test: `Tests/ConfigLoadTest.cpp` (extend `Config.YamlSectionsApplied` / `EnvOverridesYaml`), new `Tests/OceanConfigTest.cpp`

**Interfaces:**
- Consumes: Task 2 `FOceanSurface`.
- Produces:
  ```cpp
  struct FCamSimConfig::FOceanConfig {
    bool bEnabled = true; float Beaufort = 3.0f; float WaveDirectionDeg = 270.0f; float Choppiness = 0.5f;
    bool bVesselMotion = true; float VesselMotionScale = 1.0f; float MaxRadiusKm = 400.0f;
    FString MaterialPath = TEXT("/Game/Ocean/M_Ocean"); };
  FCamSimConfig::Ocean
  FOceanSurface*       UCamSimSubsystem::GetOceanSurface();        // nullptr when disabled or no geoid
  const FOceanSurface* UCamSimSubsystem::GetOceanSurface() const;
  ```

- [ ] **Step 1: Write the failing config tests** — `Tests/OceanConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanConfigDefaultsTest, "CamSim.Ocean.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FOceanConfig O;
	TestTrue (TEXT("enabled"), O.bEnabled);
	TestEqual(TEXT("beaufort"), O.Beaufort, 3.0f);
	TestEqual(TEXT("direction"), O.WaveDirectionDeg, 270.0f);
	TestEqual(TEXT("choppiness"), O.Choppiness, 0.5f);
	TestTrue (TEXT("vessel motion"), O.bVesselMotion);
	TestEqual(TEXT("motion scale"), O.VesselMotionScale, 1.0f);
	TestEqual(TEXT("max radius"), O.MaxRadiusKm, 400.0f);
	TestEqual(TEXT("material"), O.MaterialPath, FString(TEXT("/Game/Ocean/M_Ocean")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanConfigYamlTest, "CamSim.Ocean.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanConfigYamlTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	const FString Yaml = TEXT(
		"ocean:\n"
		"  enabled: false\n"
		"  beaufort: 6.5\n"
		"  wave_direction_deg: 45\n"
		"  choppiness: 0.9\n"
		"  vessel_motion: false\n"
		"  vessel_motion_scale: 0.5\n"
		"  max_radius_km: 100\n"
		"  material: \"/Game/X/M_Y\"\n");
	TestTrue(TEXT("parsed"), FCamSimConfig::LoadFromYamlString(Yaml, Cfg));
	TestFalse(TEXT("enabled"), Cfg.Ocean.bEnabled);
	TestEqual(TEXT("beaufort"), Cfg.Ocean.Beaufort, 6.5f);
	TestEqual(TEXT("direction"), Cfg.Ocean.WaveDirectionDeg, 45.0f);
	TestEqual(TEXT("choppiness"), Cfg.Ocean.Choppiness, 0.9f);
	TestFalse(TEXT("motion"), Cfg.Ocean.bVesselMotion);
	TestEqual(TEXT("scale"), Cfg.Ocean.VesselMotionScale, 0.5f);
	TestEqual(TEXT("radius"), Cfg.Ocean.MaxRadiusKm, 100.0f);
	TestEqual(TEXT("material"), Cfg.Ocean.MaterialPath, FString(TEXT("/Game/X/M_Y")));

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT("2.5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WAVE_DIR"), TEXT("90"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_ENABLED"), TEXT("true"));
	FCamSimConfig::ApplyEnvOverrides(Cfg);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_BEAUFORT"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WAVE_DIR"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_ENABLED"), TEXT(""));
	TestEqual(TEXT("env beaufort (fractional)"), Cfg.Ocean.Beaufort, 2.5f);
	TestEqual(TEXT("env direction"), Cfg.Ocean.WaveDirectionDeg, 90.0f);
	TestTrue (TEXT("env enabled accepts true"), Cfg.Ocean.bEnabled);
	return true;
}
```

Before writing: open `Tests/ConfigLoadTest.cpp` and `Config/CamSimConfig.h` and use the actual names of the YAML-string loader and env-override entry points (e.g. `LoadFromYamlString`, `ApplyEnvOverrides`) — adjust the two calls above to match, and the bool env helper (`GetEnvBool`, which accepts true/yes/on). Also add an `ocean:` block with a non-default value to the YAML in `Config.YamlSectionsApplied` and assert it there.

- [ ] **Step 2: Run to verify they fail** — build fails: `FOceanConfig` unknown.

- [ ] **Step 3: Implement config.** In `CamSimConfig.h` replace the whole `FPhase19Config` struct and `FPhase19Config Phase19;` with:

```cpp
	/** Ocean surface for boats (ROADMAP 2.6). */
	struct FOceanConfig
	{
		bool    bEnabled          = true;     // startup only
		float   Beaufort          = 3.0f;     // 0–12, fractional; used while no CIGI Wave Control wave is enabled
		float   WaveDirectionDeg  = 270.0f;   // direction waves come FROM, true north
		float   Choppiness        = 0.5f;     // 0 = sine, 1 = steepest without looping
		bool    bVesselMotion     = true;     // boats pitch/roll/heave with the waves
		float   VesselMotionScale = 1.0f;
		float   MaxRadiusKm       = 400.0f;   // horizon cap for the ocean mesh
		FString MaterialPath      = TEXT("/Game/Ocean/M_Ocean");
	};
	FOceanConfig Ocean;
```

In `CamSimConfig.cpp` replace the `phase19` YAML block with (using the file's existing helpers):

```cpp
		if (YamlHas(Root, "ocean"))
		{
			ryml::ConstNodeRef O = Root["ocean"];
			YamlBool  (O, "enabled",             Cfg.Ocean.bEnabled);
			YamlFloat (O, "beaufort",            Cfg.Ocean.Beaufort);
			YamlFloat (O, "wave_direction_deg",  Cfg.Ocean.WaveDirectionDeg);
			YamlFloat (O, "choppiness",          Cfg.Ocean.Choppiness);
			YamlBool  (O, "vessel_motion",       Cfg.Ocean.bVesselMotion);
			YamlFloat (O, "vessel_motion_scale", Cfg.Ocean.VesselMotionScale);
			YamlFloat (O, "max_radius_km",       Cfg.Ocean.MaxRadiusKm);
			YamlString(O, "material",            Cfg.Ocean.MaterialPath);
		}
```

and the env block with:

```cpp
	// Ocean (ROADMAP 2.6)
	Cfg.Ocean.bEnabled          = GetEnvBool (TEXT("CAMSIM_OCEAN_ENABLED"),        Cfg.Ocean.bEnabled);
	Cfg.Ocean.Beaufort          = GetEnvFloat(TEXT("CAMSIM_OCEAN_BEAUFORT"),       Cfg.Ocean.Beaufort);
	Cfg.Ocean.WaveDirectionDeg  = GetEnvFloat(TEXT("CAMSIM_OCEAN_WAVE_DIR"),       Cfg.Ocean.WaveDirectionDeg);
	Cfg.Ocean.Choppiness        = GetEnvFloat(TEXT("CAMSIM_OCEAN_CHOPPINESS"),     Cfg.Ocean.Choppiness);
	Cfg.Ocean.bVesselMotion     = GetEnvBool (TEXT("CAMSIM_OCEAN_MOTION_ENABLED"), Cfg.Ocean.bVesselMotion);
	Cfg.Ocean.VesselMotionScale = GetEnvFloat(TEXT("CAMSIM_OCEAN_MOTION_SCALE"),   Cfg.Ocean.VesselMotionScale);
	Cfg.Ocean.MaxRadiusKm       = GetEnvFloat(TEXT("CAMSIM_OCEAN_MAX_RADIUS_KM"),  Cfg.Ocean.MaxRadiusKm);
```

In `KeepRestartOnlySettings(Running, Reloaded)` add `Reloaded.Ocean.bEnabled = Running.Ocean.bEnabled; Reloaded.Ocean.MaterialPath = Running.Ocean.MaterialPath;`.

In `deploy/camsim_config.yaml` replace the `phase19:` block with the spec's `ocean:` block (copy verbatim from the spec, with the env var comments). In `docs/configuration.md` replace the Phase 19 section with an "Ocean (`ocean:`)" table: key, env var, default, meaning — one row per field above; note `enabled`/`material` are restart-only; note removal of the `phase19` keys.

- [ ] **Step 4: Remove the flat plane.** Delete `FGerstnerOceanSurface.*`, `IOceanSurface.h`, `Phase19OceanTest.cpp`. Delete `ACamSimEntity::ApplyVesselMotion` (decl + def, and the `#include "Ocean/IOceanSurface.h"`), `FCamSimEntityManager::SetOceanSurface`, its `OceanSurface` member, and the Phase 19C block in `CamSimEntityManager.cpp`. `grep -rn "IOceanSurface\|Phase19\|ApplyVesselMotion\|SetOceanSurface" unreal_project/CamSimTest/Source` must return nothing afterwards (except `ConfigLoadTest` if it referenced `Phase19`, which you update to `Ocean`).

- [ ] **Step 5: Subsystem owns the surface.** In `CamSimSubsystem.h` add (public):

```cpp
	/** The sea (ROADMAP 2.6); nullptr when ocean.enabled is off or the EGM96 grid is missing. */
	FOceanSurface*       GetOceanSurface();
	const FOceanSurface* GetOceanSurface() const;
	/** Apply the hot-reloadable ocean fields (Beaufort, direction, choppiness). */
	void ApplyOceanConfig(const FCamSimConfig::FOceanConfig& Cfg);
```

Forward-declare `class FOceanSurface;`. Hold it in the Pimpl (`FSubsystemImpl`) as `TUniquePtr<FOceanSurface> Ocean;` plus `FDelegateHandle OceanPreTickHandle;` (match how other Impl members are declared in `CamSimSubsystem.cpp`). In `Initialize` after config load:

```cpp
	if (Config.Ocean.bEnabled)
	{
		if (CamSim::Geospatial::GetGeoidUndulation(0.0, 0.0).IsSet())
		{
			Impl->Ocean = MakeUnique<FOceanSurface>();
			ApplyOceanConfig(Config.Ocean);
			// Sim time before any actor ticks, so boat placement and the material share one t.
			Impl->OceanPreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddLambda(
				[this](UWorld* World, ELevelTick, float)
				{
					if (World == GetWorld() && Impl && Impl->Ocean)
					{
						Impl->Ocean->SetTime(static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6);
					}
				});
			UE_LOG(LogCamSim, Log, TEXT("Ocean: on (Beaufort %.1f from %.0f deg)"), Config.Ocean.Beaufort, Config.Ocean.WaveDirectionDeg);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("Ocean: disabled — EGM96 grid missing (Content/NonUFS/Geoid/WW15MGH.DAC; run git lfs pull)"));
		}
	}
```

`Deinitialize`: `FWorldDelegates::OnWorldPreActorTick.Remove(Impl->OceanPreTickHandle); Impl->Ocean.Reset();`.

```cpp
void UCamSimSubsystem::ApplyOceanConfig(const FCamSimConfig::FOceanConfig& Cfg)
{
	if (FOceanSurface* O = GetOceanSurface())
	{
		O->SetBeaufort(Cfg.Beaufort, Cfg.WaveDirectionDeg, Cfg.Choppiness);
	}
}
```

Call `ApplyOceanConfig(Config.Ocean)` at the end of `HotReloadConfig`.

- [ ] **Step 6: FOceanManager skeleton.** Rewrite `Ocean/FOceanManager.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
class UWorld;
class AActor;
struct FOceanWaveCommand;
struct FMaritimeSurfaceCommand;

/**
 * Draws the subsystem's FOceanSurface (ROADMAP 2.6) and applies CIGI ocean
 * commands to it. Lives inside ACamSimEnvironment. Rendering arrives in Task 9.
 */
class FOceanManager
{
public:
	void Init(UWorld* World, AActor* Owner, UCamSimSubsystem* Subsystem);
	void Tick();
	void ApplyWave(const FOceanWaveCommand& Cmd);
	void ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd);

private:
	UCamSimSubsystem* Subsystem = nullptr;
};
```

`FOceanManager.cpp`: `Init` stores the subsystem; `Tick` is empty for now; `ApplyWave`/`ApplyMaritimeSurface` are empty stubs (Task 4 fills them). In `ACamSimEnvironment`: `OceanManager.Init(GetWorld(), this, Subsystem);`, remove the old wave/maritime drain bodies (leave the dequeue loops calling nothing is not acceptable — keep draining and call `OceanManager.ApplyWave(CamSim::Cigi::ToOceanWaveCommand(WaveState))` / `OceanManager.ApplyMaritimeSurface(CamSim::Cigi::ToMaritimeSurfaceCommand(MaritimeState))`), `OceanManager.Tick();`, and delete `OnAtmosphereChanged`'s ocean call (keep the function if others use it; otherwise delete it and its call site).

- [ ] **Step 7: Build and run** — `scripts/run.sh --build-only && run_tests CamSim` → everything passes (the 11 `Phase19` tests are gone; count drops accordingly).

- [ ] **Step 8: Commit**

```bash
git add -A unreal_project/CamSimTest/Source deploy/camsim_config.yaml docs/configuration.md
git commit -m "refactor(ocean): ocean: config, subsystem-owned FOceanSurface; remove the Phase 19 flat plane"
```

---

### Task 4: CIGI Wave Control and Maritime Surface through the host adapter

**Files:**
- Modify: `CIGI/CigiPacketTypes.h` (`FCigiWaveState`), `CIGI/CigiReceiver.cpp` (`FWaveCtrlProcessor` ~L556)
- Modify: `Sim/Commands.h` (`FOceanWaveCommand`), `Hosts/CigiCommands.cpp` (`ToOceanWaveCommand`)
- Modify: `Ocean/FOceanManager.cpp` (`ApplyWave`, `ApplyMaritimeSurface`)
- Test: `Tests/HostCommandsTest.cpp`, new `Tests/OceanCommandsTest.cpp`

**Interfaces:**
- Consumes: Task 2 `FOceanSurface::SetHostWave/SetTideOffsetM/SetClarity/SetWaterTempC`; Task 3 `UCamSimSubsystem::GetOceanSurface()`.
- Produces:
  ```cpp
  struct FOceanWaveCommand { FWeatherCommand::EScope Scope; uint16 RegionId; uint8 WaveId; bool bEnabled;
    float HeightM, LengthM, PeriodS, DirectionDeg /*propagates toward, CIGI*/, PhaseOffsetDeg; uint8 Breaker; };
  namespace CamSimOcean { TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand&); }   // Ocean/OceanCommands.h
  ```

- [ ] **Step 1: Write the failing tests.** Append to `Tests/HostCommandsTest.cpp` (follow the file's existing test style/flags):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostCommandsOceanWaveTest, "CamSim.Hosts.Cigi.OceanWaveCarriesEveryField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FHostCommandsOceanWaveTest::RunTest(const FString& Parameters)
{
	FCigiWaveState In;
	In.WaveID = 2; In.bEnabled = true; In.WaveHtM = 1.5f; In.WaveLenM = 40.0f; In.PeriodS = 6.0f;
	In.DirectionDeg = 45.0f; In.PhaseOffsetDeg = 90.0f; In.Scope = 0; In.EntityRgnId = 7; In.Breaker = 1;
	const FOceanWaveCommand C = CamSim::Cigi::ToOceanWaveCommand(In);
	TestEqual(TEXT("id"), C.WaveId, (uint8)2);
	TestTrue (TEXT("enabled"), C.bEnabled);
	TestEqual(TEXT("height"), C.HeightM, 1.5f);
	TestEqual(TEXT("length"), C.LengthM, 40.0f);
	TestEqual(TEXT("period"), C.PeriodS, 6.0f);
	TestEqual(TEXT("direction"), C.DirectionDeg, 45.0f);
	TestEqual(TEXT("phase"), C.PhaseOffsetDeg, 90.0f);
	TestTrue (TEXT("global scope"), C.Scope == FWeatherCommand::EScope::Global);
	TestEqual(TEXT("region"), C.RegionId, (uint16)7);
	TestEqual(TEXT("breaker"), C.Breaker, (uint8)1);
	return true;
}
```

`Tests/OceanCommandsTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanCommands.h"
#include "Sim/Commands.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanCommandsWaveTest, "CamSim.Ocean.Commands.WaveCommandToWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanCommandsWaveTest::RunTest(const FString& Parameters)
{
	FOceanWaveCommand C;
	C.WaveId = 1; C.bEnabled = true; C.HeightM = 2.0f; C.LengthM = 50.0f; C.PeriodS = 0.0f;
	C.DirectionDeg = 90.0f; C.PhaseOffsetDeg = 180.0f;
	const TOptional<FOceanWave> W = CamSimOcean::ToOceanWave(C);
	TestTrue (TEXT("enabled → wave"), W.IsSet());
	TestEqual(TEXT("propagates toward 90 → from 270"), W->FromDeg, 270.0, 1e-9);
	TestEqual(TEXT("phase in radians"), W->PhaseRad, PI, 1e-9);
	TestEqual(TEXT("height"), W->HeightM, 2.0, 1e-9);

	C.bEnabled = false;
	TestFalse(TEXT("disabled → removal"), CamSimOcean::ToOceanWave(C).IsSet());
	C.bEnabled = true; C.HeightM = NAN;
	TestFalse(TEXT("non-finite → removal"), CamSimOcean::ToOceanWave(C).IsSet());
	return true;
}
```

Also add a CCL round-trip: in `Tests/CigiExtendedResponseTest.cpp`'s style (which encodes with a CCL host session and decodes), encode a `CigiWaveCtrlV3` with Direction 135 and PhaseOff 30 and feed it through `FCigiReceiver`'s parse path the way existing receiver tests do; assert `DequeueWaveState` yields `DirectionDeg == 135`, `PhaseOffsetDeg == 30`. If no existing test drives `FCigiReceiver` from bytes, skip this sub-test and record that in the task report (the adapter test above still pins the mapping).

- [ ] **Step 2: Run to verify they fail** — build fails on the new fields.

- [ ] **Step 3: Implement.** `CigiPacketTypes.h`:

```cpp
/** CIGI Wave Control (opcode 14) — ocean wave parameters from the host. */
struct FCigiWaveState
{
	uint16 EntityRgnId    = 0;
	uint8  WaveID         = 0;
	bool   bEnabled       = false;
	uint8  Scope          = 0;     // 0 Global, 1 Regional, 2 Entity
	uint8  Breaker        = 0;     // 0 plunging, 1 spilling, 2 surging (ignored)
	float  WaveHtM        = 0.0f;  // crest-to-trough, metres
	float  WaveLenM       = 0.0f;  // metres
	float  PeriodS        = 0.0f;  // seconds
	float  DirectionDeg   = 0.0f;  // direction the wave propagates, true north (CIGI 3.3)
	float  PhaseOffsetDeg = 0.0f;
};
```

Receiver:

```cpp
		FCigiWaveState State;
		State.EntityRgnId    = static_cast<uint16>(Pkt->GetEntityRgnID());
		State.WaveID         = static_cast<uint8>(Pkt->GetWaveID());
		State.bEnabled       = Pkt->GetWaveEn();
		State.Scope          = static_cast<uint8>(Pkt->GetScope());
		State.Breaker        = static_cast<uint8>(Pkt->GetBreaker());
		State.WaveHtM        = static_cast<float>(Pkt->GetWaveHt());
		State.WaveLenM       = static_cast<float>(Pkt->GetWaveLen());
		State.PeriodS        = static_cast<float>(Pkt->GetPeriod());
		State.DirectionDeg   = static_cast<float>(Pkt->GetDirection());
		State.PhaseOffsetDeg = static_cast<float>(Pkt->GetPhaseOff());
```

`Sim/Commands.h`:

```cpp
struct FOceanWaveCommand
{
	FWeatherCommand::EScope Scope = FWeatherCommand::EScope::Global;
	uint16 RegionId       = 0;
	uint8  WaveId         = 0;
	bool   bEnabled       = false;
	float  HeightM        = 0.0f;
	float  LengthM        = 0.0f;
	float  PeriodS        = 0.0f;
	float  DirectionDeg   = 0.0f;  // propagates toward, true north
	float  PhaseOffsetDeg = 0.0f;
	uint8  Breaker        = 0;
};
```

(`FOceanWaveCommand` must be declared after `FWeatherCommand` in the header; move it if needed.)

`ToOceanWaveCommand`: fill every field; `Out.Scope = ToScope(In.Scope); Out.RegionId = In.EntityRgnId;` (reuse the file's `ToScope`).

Create `Ocean/OceanCommands.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanWaves.h"

struct FOceanWaveCommand;

namespace CamSimOcean
{
	/** Enabled, finite command → wave (FromDeg = CIGI direction + 180); otherwise unset (= remove). */
	CAMSIMTEST_API TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand& Cmd);
}
```

and `Ocean/OceanCommands.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanCommands.h"
#include "Sim/Commands.h"

namespace CamSimOcean
{
	TOptional<FOceanWave> ToOceanWave(const FOceanWaveCommand& Cmd)
	{
		const bool bFinite = FMath::IsFinite(Cmd.HeightM) && FMath::IsFinite(Cmd.LengthM) && FMath::IsFinite(Cmd.PeriodS)
			&& FMath::IsFinite(Cmd.DirectionDeg) && FMath::IsFinite(Cmd.PhaseOffsetDeg);
		if (!Cmd.bEnabled || !bFinite) return {};
		FOceanWave W;
		W.HeightM  = Cmd.HeightM;
		W.LengthM  = Cmd.LengthM;
		W.PeriodS  = FMath::Max(0.0f, Cmd.PeriodS);
		W.FromDeg  = FRotator::NormalizeAxis(Cmd.DirectionDeg + 180.0);
		if (W.FromDeg < 0.0) W.FromDeg += 360.0;
		W.PhaseRad = FMath::DegreesToRadians(static_cast<double>(Cmd.PhaseOffsetDeg));
		return W;
	}
}
```

`FOceanManager`:

```cpp
void FOceanManager::ApplyWave(const FOceanWaveCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedWave) { bWarnedScopedWave = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Wave Control ignored (Global only)")); }
		return;
	}
	if (Cmd.WaveId >= FOceanWaves::MaxWaves)
	{
		if (!bWarnedWaveId) { bWarnedWaveId = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: Wave ID %u ignored (0-3 supported)"), Cmd.WaveId); }
		return;
	}
	Ocean->SetHostWave(Cmd.WaveId, CamSimOcean::ToOceanWave(Cmd));
}

void FOceanManager::ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean || !Cmd.bEnabled) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedMaritime) { bWarnedScopedMaritime = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Maritime Surface Conditions ignored (Global only)")); }
		return;
	}
	Ocean->SetTideOffsetM(Cmd.SurfaceHeightM);
	Ocean->SetClarity(Cmd.Clarity);
	Ocean->SetWaterTempC(Cmd.WaterTempC);
}
```

(add `bool bWarnedScopedWave = false, bWarnedWaveId = false, bWarnedScopedMaritime = false;` members.)

- [ ] **Step 4: Build and run** — `run_tests CamSim.Hosts+CamSim.Ocean` (use `CamSim` if `+` filter lists aren't supported) → pass.

- [ ] **Step 5: Commit** — `git commit -m "feat(ocean): CIGI Wave Control direction/phase/scope and Maritime Surface through the host adapter"`

---

### Task 5: Boats float on the sea and ride the waves

**Files:**
- Modify: `Entity/SurfaceClamp.h/.cpp` (`FWaterInput`, `ClampWater` overload)
- Modify: `Entity/SurfaceProbe.h/.cpp` (`PlaceOnSurface` takes `const FOceanSurface*` + motion)
- Modify: `Entity/CamSimEntity.h/.cpp` (`CommitPose` passes the ocean; re-commit resting boats each tick)
- Test: `Tests/SurfaceClampTest.cpp`

**Interfaces:**
- Consumes: Task 2 `FOceanSurface::SeaLevelM/SurfaceHeightM/GetWaves`; Task 3 `UCamSimSubsystem::GetOceanSurface()`, `Config.Ocean.bVesselMotion/VesselMotionScale`.
- Produces:
  ```cpp
  namespace CamSimSurface {
    struct FWaterInput { const FOceanSurface* Ocean = nullptr; bool bMotion = true; double MotionScale = 1.0; };
    FGeoPose ClampWater(const FGeoPose& Sender, TOptional<double> CentreHit, TOptional<double> SeaLevelM,
                        double DtSec, FClampState& State, const FWaterInput& Water, double HalfLengthM, double HalfBeamM);
    FGeoPose PlaceOnSurface(ESurfaceMode, const FGeoPose&, double HalfLengthM, double HalfBeamM, double DtSec,
                            const ISurfaceProbe&, FClampState&, const FWaterInput& Water = FWaterInput());
  }
  ```

- [ ] **Step 1: Write the failing tests** — append to `Tests/SurfaceClampTest.cpp`:

```cpp
#include "Ocean/OceanSurface.h"

namespace
{
	FOceanSurface Sea(double Geoid = -32.0)
	{
		FOceanSurface S([Geoid](double, double) { return TOptional<double>(Geoid); });
		S.SetAnchor(37.795, -122.46);
		return S;
	}
	FOceanWave OneWave(double H, double L, double From)
	{
		FOceanWave W; W.HeightM = H; W.LengthM = L; W.FromDeg = From; return W;
	}
	struct FStubProbe final : ISurfaceProbe
	{
		TOptional<double> Height;
		TOptional<double> TraceHeight(double, double, double, double) const override { return Height; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampOceanSeabedTest, "CamSim.Entity.SurfaceClamp.OceanBeatsSeabed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampOceanSeabedTest::RunTest(const FString& Parameters)
{
	const FOceanSurface S = Sea(-32.0);   // calm (no waves)
	FWaterInput W; W.Ocean = &S;
	FClampState St;
	FStubProbe Seabed; Seabed.Height = -55.0;   // bathymetry, 23 m below sea level
	CamSimFrames::FGeoPose P = PlaceOnSurface(ESurfaceMode::Water, Sender(), 3.0, 1.2, 0.0, Seabed, St, W);
	TestEqual(TEXT("seabed loses to sea level"), P.Alt, -32.0, 1e-6);

	FClampState St2;
	FStubProbe Lake; Lake.Height = 300.0;
	P = PlaceOnSurface(ESurfaceMode::Water, Sender(), 3.0, 1.2, 0.0, Lake, St2, W);
	TestEqual(TEXT("lake above sea level wins"), P.Alt, 300.0, 1e-6);

	FClampState St3;
	FStubProbe Miss;
	P = PlaceOnSurface(ESurfaceMode::Water, Sender(), 3.0, 1.2, 0.0, Miss, St3, W);
	TestEqual(TEXT("no hit: sea level"), P.Alt, -32.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampOceanOffTest, "CamSim.Entity.SurfaceClamp.OceanOffUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampOceanOffTest::RunTest(const FString& Parameters)
{
	FStubProbe Seabed; Seabed.Height = -55.0;
	FClampState A, B;
	const CamSimFrames::FGeoPose Old = PlaceOnSurface(ESurfaceMode::Water, Sender(), 3.0, 1.2, 0.0, Seabed, A);
	const CamSimFrames::FGeoPose New = PlaceOnSurface(ESurfaceMode::Water, Sender(), 3.0, 1.2, 0.0, Seabed, B, FWaterInput());
	TestEqual(TEXT("no ocean: seabed as today"), New.Alt, Old.Alt, 0.0);
	TestTrue (TEXT("no ocean: attitude as today"), New.Neu.Equals(Old.Neu, 0.0));
	// Ground vehicles ignore the ocean.
	const FOceanSurface S = Sea();
	FWaterInput W; W.Ocean = &S;
	FStubProbe Ground; Ground.Height = -40.0;   // polder below sea level: a truck stays on the ground
	FClampState G;
	TestEqual(TEXT("ground ignores ocean"), PlaceOnSurface(ESurfaceMode::Ground, Sender(), 3.0, 1.2, 0.0, Ground, G, W).Alt, -40.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampOceanMotionTest, "CamSim.Entity.SurfaceClamp.WavePitchRollHeave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampOceanMotionTest::RunTest(const FString& Parameters)
{
	// One 40 m wave travelling north (from 180); hull 6 m long, 2.4 m beam, sampled over time.
	FOceanSurface S = Sea(-32.0);
	S.SetHostWave(0, OneWave(2.0, 40.0, 180.0));
	FWaterInput W; W.Ocean = &S;
	FStubProbe Miss;
	double MaxPitchHeadingNorth = 0.0, MaxRollHeadingNorth = 0.0, MaxPitchHeadingEast = 0.0, MaxRollHeadingEast = 0.0;
	double MinAlt = 1e9, MaxAlt = -1e9;
	for (int32 i = 0; i < 80; ++i)
	{
		S.SetTime(i * 0.1);
		FClampState A, B;
		const CamSimFrames::FGeoPose N = PlaceOnSurface(ESurfaceMode::Water, Sender(0.0),  3.0, 1.2, 0.0, Miss, A, W);
		const CamSimFrames::FGeoPose E = PlaceOnSurface(ESurfaceMode::Water, Sender(90.0), 3.0, 1.2, 0.0, Miss, B, W);
		MaxPitchHeadingNorth = FMath::Max(MaxPitchHeadingNorth, FMath::Abs(N.Neu.Rotator().Pitch));
		MaxRollHeadingNorth  = FMath::Max(MaxRollHeadingNorth,  FMath::Abs(N.Neu.Rotator().Roll));
		MaxPitchHeadingEast  = FMath::Max(MaxPitchHeadingEast,  FMath::Abs(E.Neu.Rotator().Pitch));
		MaxRollHeadingEast   = FMath::Max(MaxRollHeadingEast,   FMath::Abs(E.Neu.Rotator().Roll));
		MinAlt = FMath::Min(MinAlt, N.Alt); MaxAlt = FMath::Max(MaxAlt, N.Alt);
		TestEqual(TEXT("heading kept"), N.Neu.Rotator().Yaw, 0.0, 1e-6);
	}
	// Max slope a k = 1 * 2pi/40 ≈ 0.157 rad; a 6 m hull on a 40 m wave sees ~ atan(2a sin(k L/2)/L)... ≈ 8.5°.
	TestTrue(*FString::Printf(TEXT("head seas pitch (%.2f deg) in 5..10"), MaxPitchHeadingNorth), MaxPitchHeadingNorth > 5.0 && MaxPitchHeadingNorth < 10.0);
	TestTrue(*FString::Printf(TEXT("head seas roll (%.3f deg) ~0"), MaxRollHeadingNorth), MaxRollHeadingNorth < 0.1);
	TestTrue(*FString::Printf(TEXT("beam seas roll (%.2f deg) > 2"), MaxRollHeadingEast), MaxRollHeadingEast > 2.0);
	TestTrue(*FString::Printf(TEXT("beam seas pitch (%.3f deg) ~0"), MaxPitchHeadingEast), MaxPitchHeadingEast < 0.1);
	TestTrue(*FString::Printf(TEXT("heave range %.2f m ≈ 2 m"), MaxAlt - MinAlt), FMath::IsNearlyEqual(MaxAlt - MinAlt, 2.0, 0.3));

	// Sign: bow higher than stern → nose up; port higher → right side down (+roll).
	S.SetHostWave(0, OneWave(2.0, 40.0, 180.0));
	for (int32 i = 0; i < 80; ++i)
	{
		S.SetTime(i * 0.1);
		double Lat, Lon, Alt;
		FClampState A;
		const CamSimFrames::FGeoPose N = PlaceOnSurface(ESurfaceMode::Water, Sender(0.0), 3.0, 1.2, 0.0, Miss, A, W);
		CamSimFrames::OffsetGeodetic(N.Lat, N.Lon, 0.0, FVector(3.0, 0.0, 0.0), Lat, Lon, Alt);
		const double Bow = S.SurfaceHeightM(Lat, Lon).GetValue();
		CamSimFrames::OffsetGeodetic(N.Lat, N.Lon, 0.0, FVector(-3.0, 0.0, 0.0), Lat, Lon, Alt);
		const double Stern = S.SurfaceHeightM(Lat, Lon).GetValue();
		if (FMath::Abs(Bow - Stern) > 0.2)
		{
			TestEqual(TEXT("pitch sign follows bow - stern"), FMath::Sign(N.Neu.Rotator().Pitch), FMath::Sign(Bow - Stern));
		}
	}

	// Motion off: sea level, level hull.
	W.bMotion = false;
	S.SetTime(1.3);
	FClampState C;
	const CamSimFrames::FGeoPose Off = PlaceOnSurface(ESurfaceMode::Water, Sender(0.0), 3.0, 1.2, 0.0, Miss, C, W);
	TestEqual(TEXT("motion off: sea level"), Off.Alt, -32.0, 1e-6);
	TestEqual(TEXT("motion off: level"), Off.Neu.Rotator().Pitch, 0.0, 1e-6);
	return true;
}
```

(If a test's expected pitch band proves wrong because the analytic estimate above is loose, compute the expected maximum from the same four-point formula in the test rather than widening the band.)

- [ ] **Step 2: Run to verify they fail** — build fails (`FWaterInput` unknown).

- [ ] **Step 3: Implement.** `SurfaceClamp.h` — add after `FClampState`:

```cpp
	/** The sea for surface vessels (ROADMAP 2.6). Ocean == nullptr: 2.5 behaviour exactly. */
	struct FWaterInput
	{
		const FOceanSurface* Ocean = nullptr;
		bool   bMotion     = true;
		double MotionScale = 1.0;
	};
```

(forward-declare `class FOceanSurface;` at file scope) and the new `ClampWater` overload declaration from Interfaces. `SurfaceClamp.cpp`:

```cpp
	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State, const FWaterInput& Water,
		double HalfLengthM, double HalfBeamM)
	{
		const TOptional<double> Sea = Water.Ocean ? Water.Ocean->SeaLevelM(Sender.Lat, Sender.Lon) : TOptional<double>();
		if (!Sea.IsSet())
		{
			return ClampWater(Sender, CentreHit, SeaLevelM, DtSec, State);   // no ocean / no geoid: as 2.5
		}
		// Base: the higher of the drawn tile surface and the sea (seabed loses, a lake above sea level wins).
		const TOptional<double> Hit = Finite(CentreHit);
		const bool   bLake  = Hit.IsSet() && *Hit > *Sea;
		const double Target = bLake ? *Hit : *Sea;
		State.Height = State.bHasSurface ? Ease(State.Height, Target, DtSec, /*bSnap=*/true) : Target;
		State.bHasSurface = true;
		State.PitchDeg = State.RollDeg = 0.0;

		CamSimFrames::FGeoPose Out = Sender;
		Out.Alt = State.Height;
		if (bLake || !Water.bMotion || Water.Ocean->GetWaves().GetWaves().Num() == 0 || HalfLengthM <= 0.0 || HalfBeamM <= 0.0)
		{
			Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, 0.0, 0.0);
			return Out;
		}
		// Waves: four hull points (never eased — the motion is the signal).
		const FFootprint F = GetFootprint(Sender.Lat, Sender.Lon, Sender.Neu.Rotator().Yaw, HalfLengthM, HalfBeamM);
		double H[4];
		for (int32 i = 0; i < 4; ++i)
		{
			H[i] = Water.Ocean->SurfaceHeightM(F.Lat[i], F.Lon[i]).Get(*Sea) - *Sea;
		}
		const double S = Water.MotionScale;
		const double Pitch = FMath::RadiansToDegrees(FMath::Atan((H[0] - H[1]) / (2.0 * HalfLengthM))) * S;
		const double Roll  = FMath::RadiansToDegrees(FMath::Atan((H[2] - H[3]) / (2.0 * HalfBeamM))) * S;
		Out.Alt += 0.25 * (H[0] + H[1] + H[2] + H[3]) * S;
		Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, Pitch, Roll);
		return Out;
	}
```

(`#include "Ocean/OceanSurface.h"` in the .cpp.) In `SurfaceProbe.cpp` `PlaceOnSurface`, add the `const FWaterInput& Water` parameter and in the Water branch call `ClampWater(Sender, Hit, Sea, DtSec, State, Water, HalfLengthM, HalfBeamM)` (the old overload is still reached through it when `Water.Ocean` is null). Update the header default argument.

- [ ] **Step 4: Entity wiring.** In `ACamSimEntity::CommitPose` build the input:

```cpp
		CamSimSurface::FWaterInput Water;
		if (UCamSimSubsystem* Sub = GetCamSimSubsystem())   // use the entity's existing subsystem accessor
		{
			Water.Ocean       = Sub->GetOceanSurface();
			Water.bMotion     = Sub->GetConfig().Ocean.bVesselMotion;
			Water.MotionScale = Sub->GetConfig().Ocean.VesselMotionScale;
		}
		Pose = CamSimSurface::PlaceOnSurface(SurfaceMode, SenderPose, HalfLength, HalfBeam, Dt, *SurfaceProbe, SurfaceState, Water);
```

Resting boats must bob: in `ACamSimEntity::Tick`, after dead reckoning, add

```cpp
	// A water entity with no motion model still rides the waves (ROADMAP 2.6).
	if (SurfaceMode == ESurfaceMode::Water && !DR.bHasMotion && !bAttached && LastCommitTimeSec >= 0.0)
	{
		if (UCamSimSubsystem* Sub = GetCamSimSubsystem(); Sub && Sub->GetOceanSurface())
		{
			CommitPose(LastCommitSender);
		}
	}
```

Find how `ACamSimEntity` reaches the subsystem today (grep `Subsystem` in `CamSimEntity.cpp`); use that rather than inventing `GetCamSimSubsystem` if it differs.

- [ ] **Step 5: Build and run** — `run_tests CamSim.Entity.SurfaceClamp` → all pass (old + 3 new).

- [ ] **Step 6: Commit** — `git commit -m "feat(ocean): boats float at sea level over bathymetry and pitch/roll/heave with the waves"`

---

### Task 6: HAT/HOT sees the water

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanQueries.h/.cpp`
- Modify: `CIGI/CigiQueryHandler.cpp` (`ProcessHatHotRequests`, ~L87–141)
- Test: `Tests/OceanQueriesTest.cpp`

**Interfaces:**
- Consumes: Task 2 `FOceanSurface`.
- Produces:
  ```cpp
  namespace CamSimOcean {
    struct FHotResult { bool bValid = false; double HotM = 0.0; bool bWater = false; FVector NormalNeu = FVector::UpVector; };
    FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon);
  }
  ```

- [ ] **Step 1: Failing test** — `Tests/OceanQueriesTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanQueries.h"
#include "Ocean/OceanSurface.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanQueriesHotTest, "CamSim.Ocean.Queries.HotSeesWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanQueriesHotTest::RunTest(const FString& Parameters)
{
	FOceanSurface S([](double, double) { return TOptional<double>(-32.0); });
	S.SetAnchor(37.8, -122.45);
	using namespace CamSimOcean;

	FHotResult R = CombineHot(-55.0, &S, 37.8, -122.45);
	TestTrue (TEXT("seabed: valid"), R.bValid);
	TestTrue (TEXT("seabed: water wins"), R.bWater);
	TestEqual(TEXT("seabed: HOT = sea"), R.HotM, -32.0, 1e-6);

	R = CombineHot(120.0, &S, 37.8, -122.45);
	TestFalse(TEXT("hill: terrain wins"), R.bWater);
	TestEqual(TEXT("hill: HOT = terrain"), R.HotM, 120.0, 1e-9);

	R = CombineHot(TOptional<double>(), &S, 37.8, -122.45);
	TestTrue (TEXT("miss over sea: valid water"), R.bValid && R.bWater);

	R = CombineHot(-55.0, nullptr, 37.8, -122.45);
	TestTrue (TEXT("no ocean: terrain as before"), R.bValid && !R.bWater && R.HotM == -55.0);
	R = CombineHot(TOptional<double>(), nullptr, 37.8, -122.45);
	TestFalse(TEXT("no ocean, miss: invalid as before"), R.bValid);

	S.SetHostWave(0, [] { FOceanWave W; W.HeightM = 2.0; W.LengthM = 40.0; W.FromDeg = 180.0; return W; }());
	S.SetTime(3.0);
	R = CombineHot(-55.0, &S, 37.8, -122.45);
	TestEqual(TEXT("waves: HOT = surface height"), R.HotM, S.SurfaceHeightM(37.8, -122.45).GetValue(), 1e-9);
	TestTrue (TEXT("waves: normal is the water normal"), R.NormalNeu.Equals(S.SurfaceNormalNeu(37.8, -122.45).GetValue(), 1e-9));
	return true;
}
```

- [ ] **Step 2: Verify failure** — build fails (`OceanQueries.h` missing).

- [ ] **Step 3: Implement** `Ocean/OceanQueries.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FOceanSurface;

namespace CamSimOcean
{
	struct FHotResult
	{
		bool    bValid    = false;
		double  HotM      = 0.0;
		bool    bWater    = false;
		FVector NormalNeu = FVector(0.0, 0.0, 1.0);   // only meaningful when bWater
	};

	/** HOT = max(terrain hit, sea surface incl. waves). Ocean == nullptr: the terrain hit alone. */
	CAMSIMTEST_API FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon);
}
```

`Ocean/OceanQueries.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanQueries.h"
#include "Ocean/OceanSurface.h"

namespace CamSimOcean
{
	FHotResult CombineHot(TOptional<double> TerrainHotM, const FOceanSurface* Ocean, double Lat, double Lon)
	{
		FHotResult R;
		if (TerrainHotM.IsSet() && FMath::IsFinite(*TerrainHotM)) { R.bValid = true; R.HotM = *TerrainHotM; }
		const TOptional<double> Water = Ocean ? Ocean->SurfaceHeightM(Lat, Lon) : TOptional<double>();
		if (Water.IsSet() && (!R.bValid || *Water > R.HotM))
		{
			R.bValid = true; R.HotM = *Water; R.bWater = true;
			R.NormalNeu = Ocean->SurfaceNormalNeu(Lat, Lon).Get(FVector(0.0, 0.0, 1.0));
		}
		return R;
	}
}
```

In `ProcessHatHotRequests`: after the trace, build `TOptional<double> TerrainHot` (set when the trace hit and `WorldToGeo` succeeded), then `const CamSimOcean::FHotResult R = CamSimOcean::CombineHot(TerrainHot, Subsystem ? Subsystem->GetOceanSurface() : nullptr, Req.Lat, Req.Lon);` Use `R.bValid`, `HOT = R.HotM`, `HAT = Req.Alt - HOT`. For the extended response normal: if `R.bWater`, `CamSimFrames::NeuToAzEl(R.NormalNeu, NormalAz, NormalEl)`; otherwise keep the existing `SurfaceNormalAzEl` path from the trace hit. Keep everything else (entity-relative resolution, logging) unchanged. Find how the handler reaches the subsystem (it has `GeoProvider` and a receiver; add a `UCamSimSubsystem*` if it doesn't have one, set where the handler is constructed).

- [ ] **Step 4: Build and run** — `run_tests CamSim.Ocean.Queries+CamSim.CigiSender+CamSim.Cigi` → pass.

- [ ] **Step 5: Commit** — `git commit -m "feat(ocean): HAT/HOT returns the sea surface over water, with its normal"`

---

### Task 7: Warped ocean grid — pure mesh builder

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Ocean/OceanMeshBuilder.h/.cpp`
- Test: `Tests/OceanMeshTest.cpp`

**Interfaces:**
- Consumes: `CamSimFrames::OffsetGeodetic`, `CamSimFrames::GeodeticToEcef`, `CamSimFrames::EcefToGeodetic`, `CamSimFrames::GeodeticDeltaToNeu`, Task 2 `FOceanSurface::SeaLevelM`.
- Produces:
  ```cpp
  namespace CamSimOcean {
    constexpr int32 GridN = 256; constexpr double CentreCellM = 2.0;
    double SolveWarpAlpha(double RadiusM, int32 N = GridN, double CentreCellM = 2.0); // 0 → uniform
    double WarpDistance(double U, double RadiusM, double Alpha);                     // u ∈ [-1,1] → metres
    double HorizonRadiusM(double CentreToNadirM, double AltAboveSeaM, double MaxRadiusKm);
    struct FOceanMeshData { FVector OriginWorld; TArray<FVector> Positions; TArray<int32> Triangles; TArray<FVector2D> CellSize; TArray<FVector> Normals; double CentreLat, CentreLon, RadiusM, Alpha; };
    using FGeoToWorldFn = TFunction<FVector(double Lat, double Lon, double AltM)>;  // UE world, cm
    bool BuildOceanMesh(double CentreLat, double CentreLon, double RadiusM, const FOceanSurface& Ocean, const FGeoToWorldFn& GeoToWorld, FOceanMeshData& Out);
    struct FRebuildPolicy { double LastLat, LastLon, LastRadiusM; bool bHasMesh; };
    bool NeedsRebuild(const FRebuildPolicy& Last, double Lat, double Lon, double RadiusM);
  }
  ```

- [ ] **Step 1: Failing tests** — `Tests/OceanMeshTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanMeshBuilder.h"
#include "Ocean/OceanSurface.h"
#include "Geospatial/EcefFrames.h"
#include "Geospatial/CigiFrames.h"

using namespace CamSimOcean;

namespace
{
	// UE world stand-in: ECEF in centimetres (any rigid transform would do).
	FVector EcefCm(double Lat, double Lon, double Alt) { return CamSimFrames::GeodeticToEcef(Lat, Lon, Alt) * 100.0; }
	double GeoidAt(double Lat, double Lon) { return -30.0 + 0.001 * Lat * Lon; }   // smooth, non-constant
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshWarpTest, "CamSim.Ocean.Mesh.WarpCentreCellAndMonotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshWarpTest::RunTest(const FString& Parameters)
{
	for (double R : { 400000.0, 25000.0, 1000.0 })
	{
		const double A = SolveWarpAlpha(R);
		const double Cell = WarpDistance(1.0 / (GridN / 2), R, A);
		const double Expected = FMath::Max(CentreCellM, R / (GridN / 2));   // small R: uniform grid
		TestEqual(*FString::Printf(TEXT("R %.0f: centre cell"), R), Cell, Expected, Expected * 0.05);
		TestEqual(*FString::Printf(TEXT("R %.0f: edge"), R), WarpDistance(1.0, R, A), R, 1e-6 * R);
		TestEqual(*FString::Printf(TEXT("R %.0f: odd"), R), WarpDistance(-0.3, R, A), -WarpDistance(0.3, R, A), 1e-9);
		double Prev = -1.0;
		bool bMono = true;
		for (int32 i = 0; i <= GridN / 2; ++i) { const double D = WarpDistance(double(i) / (GridN / 2), R, A); bMono &= D > Prev; Prev = D; }
		TestTrue(*FString::Printf(TEXT("R %.0f: monotonic"), R), bMono);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshRadiusTest, "CamSim.Ocean.Mesh.HorizonRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshRadiusTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("1000 m alt, centre at nadir"), HorizonRadiusM(0.0, 1000.0, 400.0), 3570.0 * FMath::Sqrt(1000.0) * 1.1, 1.0);
	TestEqual(TEXT("adds centre offset"), HorizonRadiusM(5000.0, 1000.0, 400.0), 5000.0 + 3570.0 * FMath::Sqrt(1000.0) * 1.1, 1.0);
	TestEqual(TEXT("cap"), HorizonRadiusM(0.0, 20000.0, 400.0), 400000.0, 1e-6);
	TestEqual(TEXT("sea level floor 2 m"), HorizonRadiusM(0.0, -5.0, 400.0), 3570.0 * FMath::Sqrt(2.0) * 1.1, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshGeoidTest, "CamSim.Ocean.Mesh.VerticesOnGeoid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshGeoidTest::RunTest(const FString& Parameters)
{
	FOceanSurface S([](double Lat, double Lon) { return TOptional<double>(GeoidAt(Lat, Lon)); });
	FOceanMeshData M;
	TestTrue(TEXT("built"), BuildOceanMesh(37.8, -122.45, 200000.0, S, &EcefCm, M));
	TestEqual(TEXT("vertex count"), M.Positions.Num(), (GridN + 1) * (GridN + 1));
	TestEqual(TEXT("triangle indices"), M.Triangles.Num(), GridN * GridN * 6);
	TestEqual(TEXT("cell sizes"), M.CellSize.Num(), M.Positions.Num());
	const int32 Mid = GridN / 2;
	for (const int32 Idx : { Mid * (GridN + 1) + Mid, (Mid + 64) * (GridN + 1) + Mid + 30, GridN * (GridN + 1) + GridN })
	{
		const FVector Ecef = (M.OriginWorld + M.Positions[Idx]) / 100.0;
		double Lat, Lon, Alt;
		CamSimFrames::EcefToGeodetic(Ecef, Lat, Lon, Alt);
		TestEqual(*FString::Printf(TEXT("vertex %d on geoid"), Idx), Alt, GeoidAt(Lat, Lon), 0.01);
	}
	TestTrue(TEXT("centre vertex at the origin"), M.Positions[Mid * (GridN + 1) + Mid].Size() < 1.0);

	FOceanSurface NoGrid([](double, double) { return TOptional<double>(); });
	TestFalse(TEXT("no geoid → no mesh"), BuildOceanMesh(37.8, -122.45, 200000.0, NoGrid, &EcefCm, M));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshRebuildTest, "CamSim.Ocean.Mesh.RebuildPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshRebuildTest::RunTest(const FString& Parameters)
{
	FRebuildPolicy L; L.bHasMesh = false;
	TestTrue(TEXT("first build"), NeedsRebuild(L, 37.8, -122.45, 100000.0));
	L = { 37.8, -122.45, 100000.0, true };
	double Lat, Lon, Alt;
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(400.0, 0.0, 0.0), Lat, Lon, Alt);   // 0.4% of R
	TestFalse(TEXT("small move"), NeedsRebuild(L, Lat, Lon, 100000.0));
	CamSimFrames::OffsetGeodetic(37.8, -122.45, 0.0, FVector(600.0, 0.0, 0.0), Lat, Lon, Alt);   // 0.6% of R
	TestTrue(TEXT("move > 0.5% R"), NeedsRebuild(L, Lat, Lon, 100000.0));
	TestFalse(TEXT("R +20%"), NeedsRebuild(L, 37.8, -122.45, 120000.0));
	TestTrue(TEXT("R +30%"), NeedsRebuild(L, 37.8, -122.45, 130000.0));
	TestTrue(TEXT("R -30%"), NeedsRebuild(L, 37.8, -122.45, 70000.0));
	return true;
}
```

- [ ] **Step 2: Verify failure** — build fails.

- [ ] **Step 3: Implement** `Ocean/OceanMeshBuilder.h` (declarations exactly as in Interfaces, with doc comments) and `Ocean/OceanMeshBuilder.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanMeshBuilder.h"
#include "Ocean/OceanSurface.h"
#include "Geospatial/CigiFrames.h"

namespace CamSimOcean
{
	namespace
	{
		// Centre cell for a given alpha: d(1/(N/2)).
		double CentreCell(double R, double A, int32 N)
		{
			const double U = 1.0 / (N / 2);
			return A < 1e-9 ? R * U : R * (FMath::Exp(A * U) - 1.0) / (FMath::Exp(A) - 1.0);
		}
	}

	double SolveWarpAlpha(double RadiusM, int32 N, double InCentreCellM)
	{
		if (RadiusM / (N / 2) <= InCentreCellM) return 0.0;        // uniform grid is already fine enough
		double Lo = 1e-6, Hi = 60.0;                               // centre cell decreases with alpha
		for (int32 It = 0; It < 100; ++It)
		{
			const double Mid = 0.5 * (Lo + Hi);
			if (CentreCell(RadiusM, Mid, N) > InCentreCellM) Lo = Mid; else Hi = Mid;
		}
		return 0.5 * (Lo + Hi);
	}

	double WarpDistance(double U, double RadiusM, double Alpha)
	{
		const double A = FMath::Abs(U);
		const double D = Alpha < 1e-9 ? RadiusM * A : RadiusM * (FMath::Exp(Alpha * A) - 1.0) / (FMath::Exp(Alpha) - 1.0);
		return U < 0.0 ? -D : D;
	}

	double HorizonRadiusM(double CentreToNadirM, double AltAboveSeaM, double MaxRadiusKm)
	{
		const double Horizon = 3570.0 * FMath::Sqrt(FMath::Max(AltAboveSeaM, 2.0)) * 1.1;
		return FMath::Min(CentreToNadirM + Horizon, MaxRadiusKm * 1000.0);
	}

	bool BuildOceanMesh(double CentreLat, double CentreLon, double RadiusM, const FOceanSurface& Ocean,
		const FGeoToWorldFn& GeoToWorld, FOceanMeshData& Out)
	{
		const TOptional<double> CentreSea = Ocean.SeaLevelM(CentreLat, CentreLon);
		if (!CentreSea.IsSet()) return false;

		Out = FOceanMeshData();
		Out.CentreLat = CentreLat; Out.CentreLon = CentreLon; Out.RadiusM = RadiusM;
		Out.Alpha = SolveWarpAlpha(RadiusM);
		Out.OriginWorld = GeoToWorld(CentreLat, CentreLon, *CentreSea);
		const FVector UpWorld = (GeoToWorld(CentreLat, CentreLon, *CentreSea + 1.0) - Out.OriginWorld).GetSafeNormal();

		const int32 V = GridN + 1;
		TArray<double> D; D.SetNumUninitialized(V);
		for (int32 i = 0; i < V; ++i) D[i] = WarpDistance(2.0 * i / GridN - 1.0, RadiusM, Out.Alpha);

		Out.Positions.SetNumUninitialized(V * V);
		Out.CellSize.SetNumUninitialized(V * V);
		Out.Normals.Init(UpWorld, V * V);
		for (int32 r = 0; r < V; ++r)            // r: north index
		for (int32 c = 0; c < V; ++c)            // c: east index
		{
			double Lat, Lon, Alt;
			CamSimFrames::OffsetGeodetic(CentreLat, CentreLon, 0.0, FVector(D[r], D[c], 0.0), Lat, Lon, Alt);
			const double Sea = Ocean.SeaLevelM(Lat, Lon).Get(*CentreSea);
			const int32 Idx = r * V + c;
			Out.Positions[Idx] = GeoToWorld(Lat, Lon, Sea) - Out.OriginWorld;
			const double CellN = D[FMath::Min(r + 1, GridN)] - D[FMath::Max(r - 1, 0)];
			const double CellE = D[FMath::Min(c + 1, GridN)] - D[FMath::Max(c - 1, 0)];
			Out.CellSize[Idx] = FVector2D(0.5 * FMath::Max(CellN, CellE), 0.0);   // metres; UV1.x in the material
		}
		Out.Triangles.Reserve(GridN * GridN * 6);
		for (int32 r = 0; r < GridN; ++r)
		for (int32 c = 0; c < GridN; ++c)
		{
			const int32 A = r * V + c, B = A + 1, C = A + V, Dd = C + 1;
			// Counter-clockwise seen from above in UE's left-handed frame: check winding once in Task 9 and flip here if the sea is back-face culled.
			Out.Triangles.Append({ A, C, B, B, C, Dd });
		}
		return true;
	}

	bool NeedsRebuild(const FRebuildPolicy& Last, double Lat, double Lon, double RadiusM)
	{
		if (!Last.bHasMesh) return true;
		const FVector Move = CamSimFrames::GeodeticDeltaToNeu(Last.LastLat, Last.LastLon, 0.0, Lat, Lon, 0.0);
		if (FMath::Sqrt(Move.X * Move.X + Move.Y * Move.Y) > 0.005 * Last.LastRadiusM) return true;
		return FMath::Abs(RadiusM - Last.LastRadiusM) > 0.25 * Last.LastRadiusM;
	}
}
```

- [ ] **Step 4: Build and run** — `run_tests CamSim.Ocean.Mesh` → 4 pass. Also add a timing log to the test: build at R = 400 km and `AddInfo` the elapsed ms (target < 10 ms; informational).

- [ ] **Step 5: Commit** — `git commit -m "feat(ocean): warped-grid ocean mesh builder on ellipsoid + geoid"`

---

### Task 8: Scripted `M_Ocean` and `MPC_Ocean`

**Files:**
- Create: `scripts/ocean/make_ocean_material.py` (UE editor Python)
- Create: `scripts/ocean/make_ocean_material.sh` (headless runner)
- Create (generated, committed): `unreal_project/CamSimTest/Content/Ocean/M_Ocean.uasset`, `unreal_project/CamSimTest/Content/Ocean/MPC_Ocean.uasset`
- Create: `unreal_project/CamSimTest/Shaders/Private/CamSimOcean.ush` (the Gerstner HLSL, included by the material's Custom nodes)

**Interfaces:**
- Consumes: Task 1 formulas (must mirror `FOceanWaves::Displacement` and `NormalAtParam`).
- Produces: MPC `/Game/Ocean/MPC_Ocean` vector parameters (all float4):
  `Wave0..Wave3` = (k [1/m], a [m], Q, phase [rad]); `Dir0..Dir3` = (dN, dE, 0, 0);
  `ComponentToAnchor` = (cm, xyz); `AxisN`, `AxisE`, `AxisU` = UE world unit vectors (xyz);
  `Water` = (absorption scale, scattering scale, ripple strength, 0).
  Material `/Game/Ocean/M_Ocean` reads UV channel 1 `.x` as the vertex cell size in metres.

- [ ] **Step 1: HLSL** — `Shaders/Private/CamSimOcean.ush`:

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// Gerstner sum mirroring FOceanWaves (Ocean/OceanWaves.cpp) expression for expression.
// P: plane coordinates (N, E) in metres. W[i] = (k, a, Q, phase), D[i] = (dN, dE).
// Fade: waves with lambda/4 < CellM are faded out (the grid can't carry them).
#pragma once

float CamSimOceanFade(float k, float CellM)
{
	const float Lambda = 6.28318530718 / max(k, 1e-6);
	return saturate(2.0 - 8.0 * CellM / Lambda);   // 1 below lambda/8, 0 at lambda/4
}

float3 CamSimOceanDisplacement(float2 P, float CellM, float4 W[4], float4 D[4])
{
	float3 Disp = 0;
	[unroll] for (int i = 0; i < 4; ++i)
	{
		const float Fade = CamSimOceanFade(W[i].x, CellM);
		const float Th = W[i].x * dot(D[i].xy, P) + W[i].w;
		const float S = sin(Th), C = cos(Th), Qa = W[i].z * W[i].y * Fade;
		Disp.x -= Qa * D[i].x * S;
		Disp.y -= Qa * D[i].y * S;
		Disp.z += W[i].y * Fade * C;
	}
	return Disp;   // (dN, dE, dZ) metres
}

float3 CamSimOceanNormal(float2 P, float CellM, float4 W[4], float4 D[4])
{
	float3 N = float3(0, 0, 1);
	[unroll] for (int i = 0; i < 4; ++i)
	{
		const float Fade = CamSimOceanFade(W[i].x, CellM);
		const float Th = W[i].x * dot(D[i].xy, P) + W[i].w;
		const float KA = W[i].x * W[i].y * Fade;
		N.x += D[i].x * KA * sin(Th);
		N.y += D[i].y * KA * sin(Th);
		N.z -= W[i].z * KA * cos(Th);
	}
	return normalize(N);   // (N, E, Up)
}
```

(`ComponentToAnchor` + `LocalPosition` are in cm; the Custom node code divides by 100 before projecting.) Material Custom node "OceanWPO" (inputs: `LocalPos` float3 = LocalPosition excluding material offsets, `CellM` = TexCoord[1].x, `ToAnchor`, `AxisN`, `AxisE`, `AxisU`, `W0..W3`, `D0..D3` from the MPC), code:

```hlsl
float4 W[4] = { W0, W1, W2, W3 }; float4 D[4] = { D0, D1, D2, D3 };
float3 Rel = (LocalPos + ToAnchor) * 0.01;
float2 P = float2(dot(Rel, AxisN), dot(Rel, AxisE));
float3 Disp = CamSimOceanDisplacement(P, CellM, W, D);
return (AxisN * Disp.x + AxisE * Disp.y + AxisU * Disp.z) * 100.0;
```

Custom node "OceanNormal" (same inputs; world-space normal; material has *Tangent Space Normal* off):

```hlsl
float4 W[4] = { W0, W1, W2, W3 }; float4 D[4] = { D0, D1, D2, D3 };
float3 Rel = (LocalPos + ToAnchor) * 0.01;
float2 P = float2(dot(Rel, AxisN), dot(Rel, AxisE));
float3 Nn = CamSimOceanNormal(P, CellM, W, D);
return normalize(AxisN * Nn.x + AxisE * Nn.y + AxisU * Nn.z);
```

Both Custom nodes set `IncludeFilePaths = ["/CamSim/Private/CamSimOcean.ush"]` (the `CamSimShaders` module maps `/CamSim` to `Shaders/`; confirm the virtual path in `Source/CamSimShaders` and use it). The normal node evaluates at the vertex's parameter point per pixel; the tiny mismatch with `NormalAtPlane` (which inverts) is visual only.

- [ ] **Step 2: Editor script** — `scripts/ocean/make_ocean_material.py`: uses `unreal.AssetToolsHelpers`, `unreal.MaterialParameterCollectionFactoryNew`, `unreal.MaterialFactoryNew`, `unreal.MaterialEditingLibrary`:
  1. Create/overwrite `/Game/Ocean/MPC_Ocean` with the vector parameters listed in Interfaces (defaults: all zeros except `AxisU` = (0,0,1), `Water` = (1, 1, 0.3, 0)).
  2. Create/overwrite `/Game/Ocean/M_Ocean`: `shading_model = MSM_SINGLE_LAYER_WATER`, `blend_mode = OPAQUE`, `tangent_space_normal = False`, two-sided off; add `MaterialExpressionCollectionParameter` nodes for each MPC parameter (component mask RGB for the xyz ones), `MaterialExpressionLocalPosition` (`included_offsets = EXCLUDING_MATERIAL_OFFSETS`), `MaterialExpressionTextureCoordinate` (coordinate_index 1, mask R), the two `MaterialExpressionCustom` nodes above (inputs named exactly as in the code; `output_type = CMOT_FLOAT3`), connected to `MP_WORLD_POSITION_OFFSET` and `MP_NORMAL`; base color (0.02, 0.05, 0.07), roughness 0.08, specular 0.5; a `MaterialExpressionSingleLayerWaterMaterialOutput` with scattering = (0.02, 0.06, 0.07) × `Water.y` and absorption = (0.35, 0.09, 0.03) × `Water.x`, phase G 0.1.
  3. `unreal.MaterialEditingLibrary.recompile_material(mat)`; `unreal.EditorAssetLibrary.save_asset(...)` for both.
  Print `OCEAN_MATERIAL_OK` at the end; raise on any failure.

  `scripts/ocean/make_ocean_material.sh`:

```bash
#!/usr/bin/env bash
# Regenerate Content/Ocean/M_Ocean + MPC_Ocean from scripts/ocean/make_ocean_material.py.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
UE_CMD="${UE_CMD:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
"$UE_CMD" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" -run=pythonscript \
  -script="$REPO/scripts/ocean/make_ocean_material.py" -unattended -nosplash -nosound -stdout -FullStdOutLogOutput \
  > "$REPO/.cache/make_ocean_material.log" 2>&1 || true
grep -q OCEAN_MATERIAL_OK "$REPO/.cache/make_ocean_material.log" || { echo "failed — see .cache/make_ocean_material.log" >&2; exit 1; }
echo "M_Ocean + MPC_Ocean written"
```

- [ ] **Step 3: Run it** — `mkdir -p .cache && scripts/ocean/make_ocean_material.sh`. Expected: `M_Ocean + MPC_Ocean written`, two `.uasset` files under `Content/Ocean/`. If the commandlet deadlocks (Python under a null RHI on macOS), rerun with `-ExecutePythonScript=<script>` on a normal editor launch plus `-unattended` and `-ExecCmds="Quit"` after the script, and record which form worked in the script header. If Single Layer Water fails to compile on Metal, switch `shading_model` to `MSM_DEFAULT_LIT` (opaque), drop the SLW output node, and note it for the ROADMAP (Task 11).

- [ ] **Step 4: Verify** — `grep -i "error" .cache/make_ocean_material.log` shows no material compile errors; `git status` shows the two assets.

- [ ] **Step 5: Commit** — `git add scripts/ocean unreal_project/CamSimTest/Shaders/Private/CamSimOcean.ush unreal_project/CamSimTest/Content/Ocean && git commit -m "feat(ocean): scripted Single Layer Water M_Ocean + MPC_Ocean mirroring FOceanWaves"`

---

### Task 9: Draw the ocean — mesh component, MPC writes, anchor policy

**Files:**
- Modify: `unreal_project/CamSimTest/CamSimTest.uproject` (enable `ProceduralMeshComponent`)
- Modify: `Source/CamSimTest/CamSimTest.Build.cs` (add `"ProceduralMeshComponent"`)
- Create: `Ocean/OceanMesh.h/.cpp` (thin wrapper over `UProceduralMeshComponent`)
- Modify: `Ocean/FOceanManager.h/.cpp` (rendering; anchor; MPC)
- Modify: `Environment/CamSimEnvironment.cpp` (pass camera telemetry to `Tick`)

**Interfaces:**
- Consumes: Task 7 `BuildOceanMesh`, `NeedsRebuild`, `HorizonRadiusM`; Task 8 MPC parameter names; Task 2 `FOceanSurface`; `ACamSimCamera::GetCurrentTelemetry()` (`Latitude`, `Longitude`, `Altitude`, `FrameCenterLat`, `FrameCenterLon`); `ACesiumGeoreference::ComputeEarthCenteredEarthFixedToUnrealTransformation()`.
- Produces: `void FOceanManager::Tick(const FCamSimTelemetry* Camera)`; `CamSimOcean::ChooseCentre(...)`; `CamSimOcean::NeedsReanchor(...)` (pure, tested); `CamSimOcean::WriteMpc(UWorld*, UMaterialParameterCollection*, const FOceanSurface&, const FVector& ComponentWorld, const FMatrix& EcefToUe)`.

- [ ] **Step 1: Failing tests for the pure policies** — add to `Tests/OceanMeshTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanMeshCentreTest, "CamSim.Ocean.Mesh.CentreAndAnchorPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanMeshCentreTest::RunTest(const FString& Parameters)
{
	double Lat, Lon;
	ChooseCentre(37.80, -122.45, 37.82, -122.40, /*bFrameCentreValid=*/true, Lat, Lon);
	TestEqual(TEXT("frame centre used"), Lat, 37.82, 1e-12);
	ChooseCentre(37.80, -122.45, 0.0, 0.0, /*bFrameCentreValid=*/false, Lat, Lon);
	TestEqual(TEXT("nadir fallback"), Lon, -122.45, 1e-12);
	ChooseCentre(37.80, -122.45, NAN, 0.0, true, Lat, Lon);
	TestEqual(TEXT("NaN frame centre → nadir"), Lat, 37.80, 1e-12);

	TestTrue (TEXT("no anchor yet"), NeedsReanchor(false, 0, 0, 37.8, -122.45, false));
	TestFalse(TEXT("near anchor"), NeedsReanchor(true, 37.8, -122.45, 37.9, -122.45, false));
	TestTrue (TEXT("> 200 km"), NeedsReanchor(true, 37.8, -122.45, 39.8, -122.45, false));
	TestTrue (TEXT("teleport"), NeedsReanchor(true, 37.8, -122.45, 37.8, -122.45, true));
	return true;
}
```

Declarations to add in `OceanMeshBuilder.h`:

```cpp
	/** Mesh centre: the frame centre when valid (finite, set), else the nadir. */
	CAMSIMTEST_API void ChooseCentre(double NadirLat, double NadirLon, double FcLat, double FcLon, bool bFrameCentreValid, double& OutLat, double& OutLon);
	/** Wave anchor moves only when unset, on a teleport, or > 200 km from the centre (a one-off re-phase). */
	CAMSIMTEST_API bool NeedsReanchor(bool bHasAnchor, double AnchorLat, double AnchorLon, double Lat, double Lon, bool bTeleport);
```

Implementations (in `OceanMeshBuilder.cpp`):

```cpp
	void ChooseCentre(double NadirLat, double NadirLon, double FcLat, double FcLon, bool bValid, double& OutLat, double& OutLon)
	{
		const bool bUse = bValid && FMath::IsFinite(FcLat) && FMath::IsFinite(FcLon) && FMath::Abs(FcLat) <= 90.0;
		OutLat = bUse ? FcLat : NadirLat;
		OutLon = bUse ? FcLon : NadirLon;
	}

	bool NeedsReanchor(bool bHasAnchor, double AnchorLat, double AnchorLon, double Lat, double Lon, bool bTeleport)
	{
		if (!bHasAnchor || bTeleport) return true;
		const FVector D = CamSimFrames::GeodeticDeltaToNeu(AnchorLat, AnchorLon, 0.0, Lat, Lon, 0.0);
		return D.X * D.X + D.Y * D.Y > FMath::Square(200000.0);
	}
```

Run: build + `run_tests CamSim.Ocean.Mesh` → the new test fails before the implementations exist, passes after.

- [ ] **Step 2: Plugin/module** — add to `CamSimTest.uproject` `"Plugins"`: `{ "Name": "ProceduralMeshComponent", "Enabled": true }`; add `"ProceduralMeshComponent"` to `PublicDependencyModuleNames` in `CamSimTest.Build.cs`.

- [ ] **Step 3: `Ocean/OceanMesh.h/.cpp`**:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanMeshBuilder.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class AActor;

/** The drawn sea: one UProceduralMeshComponent holding the warped grid (Task 7). */
class FOceanMesh
{
public:
	void Init(AActor* Owner, UMaterialInterface* Material);
	/** Replace the geometry; the component moves to Data.OriginWorld. */
	void Upload(const CamSimOcean::FOceanMeshData& Data);
	void SetWorldLocation(const FVector& World);
	FVector GetWorldLocation() const;
	bool IsValid() const;

private:
	TWeakObjectPtr<UProceduralMeshComponent> Component;
};
```

`.cpp`: `Init` creates `NewObject<UProceduralMeshComponent>(Owner, TEXT("OceanMesh"))`, `SetCollisionEnabled(NoCollision)`, `SetCastShadow(false)`, `bUseAsyncCooking = false`, `SetMaterial(0, Material)`, `RegisterComponent()`, identity rotation, `SetBoundsScale(1.0f)`. `Upload`: `CreateMeshSection(0, Positions, Triangles, Normals, UV0 (zeros), UV1 = CellSize, UV2/UV3 empty, VertexColors empty, Tangents empty, false)` — use the overload taking UV0..UV3; then `SetWorldLocation(Data.OriginWorld)`. The positions are `FVector` (double) — convert to the component's expected type as the API requires.

- [ ] **Step 4: `FOceanManager` rendering.** Members: `FOceanMesh Mesh; TWeakObjectPtr<UMaterialParameterCollection> Mpc; TWeakObjectPtr<UWorld> World; CamSimOcean::FRebuildPolicy Last{0, 0, 0, false}; double LastNadirLat = 0, LastNadirLon = 0; bool bHasNadir = false; TWeakObjectPtr<ACesiumGeoreference> Georeference; FCamSimConfig::FOceanConfig Cfg;` (copy `Cfg` from the subsystem config in `Init`).

`Init(World, Owner, Subsystem)`: if `Subsystem->GetOceanSurface()` is null, return (nothing drawn). Load `UMaterialInterface` from `Config.Ocean.MaterialPath` and `UMaterialParameterCollection` from `/Game/Ocean/MPC_Ocean.MPC_Ocean`; if either fails, log a warning ("Ocean: M_Ocean/MPC_Ocean missing — run scripts/ocean/make_ocean_material.sh; boats still float") and return (placement still works). Find the georeference with `ACesiumGeoreference::GetDefaultGeoreference(World)`. `Mesh.Init(Owner, Material)`.

`Tick(const FCamSimTelemetry* Cam)`:

```cpp
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean || !Cam || !Georeference.IsValid() || !Mesh.IsValid()) return;
	// Teleport: the nadir moved > 5 km since the last tick (no camera flies 150 km/s).
	const FVector Hop = CamSimFrames::GeodeticDeltaToNeu(LastNadirLat, LastNadirLon, 0.0, Cam->Latitude, Cam->Longitude, 0.0);
	const bool bTeleport = bHasNadir && Hop.X * Hop.X + Hop.Y * Hop.Y > FMath::Square(5000.0);
	LastNadirLat = Cam->Latitude; LastNadirLon = Cam->Longitude; bHasNadir = true;

	double CLat, CLon;
	CamSimOcean::ChooseCentre(Cam->Latitude, Cam->Longitude, Cam->FrameCenterLat, Cam->FrameCenterLon,
		Cam->FrameCenterLat != 0.0 || Cam->FrameCenterLon != 0.0, CLat, CLon);
	const FOceanWaves& W = Ocean->GetWaves();
	if (CamSimOcean::NeedsReanchor(W.HasAnchor(), W.GetAnchorLat(), W.GetAnchorLon(), CLat, CLon, bTeleport))
	{
		Ocean->SetAnchor(CLat, CLon);
	}
	const double SeaAtNadir = Ocean->SeaLevelM(Cam->Latitude, Cam->Longitude).Get(0.0);
	const FVector CN = CamSimFrames::GeodeticDeltaToNeu(Cam->Latitude, Cam->Longitude, 0.0, CLat, CLon, 0.0);
	const double R = CamSimOcean::HorizonRadiusM(FMath::Sqrt(CN.X * CN.X + CN.Y * CN.Y), Cam->Altitude - SeaAtNadir, Cfg.MaxRadiusKm);
	if (bTeleport || CamSimOcean::NeedsRebuild(Last, CLat, CLon, R))
	{
		const FMatrix EcefToUe = Georeference->ComputeEarthCenteredEarthFixedToUnrealTransformation();
		auto GeoToWorld = [&EcefToUe](double Lat, double Lon, double Alt)
			{ return EcefToUe.TransformPosition(CamSimFrames::GeodeticToEcef(Lat, Lon, Alt)); };
		const double T0 = FPlatformTime::Seconds();
		CamSimOcean::FOceanMeshData Data;
		if (CamSimOcean::BuildOceanMesh(CLat, CLon, R, *Ocean, GeoToWorld, Data))
		{
			Mesh.Upload(Data);
			Last = { CLat, CLon, R, true };
			const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
			UE_LOG(LogCamSim, Ms > 10.0 ? Warning : Verbose, TEXT("Ocean: mesh rebuilt in %.1f ms (R %.1f km, centre %.5f %.5f)"), Ms, R / 1000.0, CLat, CLon);
		}
	}
	CamSimOcean::WriteMpc(World.Get(), Mpc.Get(), *Ocean, Mesh.GetWorldLocation(),
		Georeference->ComputeEarthCenteredEarthFixedToUnrealTransformation());
```

Confirm the Cesium ECEF→UE matrix units (it maps metres ECEF → UE cm); if it expects/returns different units, scale accordingly and assert in a log once: `GeoToWorld(anchor, 0)` must equal `GeoProvider->GeoToWorld(anchor, 0)` within 1 cm. Between rebuilds, no component move is needed (the mesh is geographic); drop the "translate between rebuilds" idea from the spec if rebuild cost stays < 10 ms, and record which was done.

`CamSimOcean::WriteMpc(UWorld*, UMaterialParameterCollection*, const FOceanSurface&, const FVector& ComponentWorld, const FMatrix& EcefToUe)` — a free function declared in `FOceanManager.h` (Task 10's GPU test calls it): for i in 0..3 — if i < waves: `Wave{i}` = (k, a, Q, Phase(i)) as floats, `Dir{i}` = (dN, dE, 0, 0); else zeros. `ComponentToAnchor` = (Mesh.GetWorldLocation() − AnchorWorld) where AnchorWorld = `EcefToUe.TransformPosition(W.GetAnchorEcef())`; `AxisN/E/U` = `EcefToUe.TransformVector(W.GetAxis*Ecef()).GetSafeNormal()`; `Water` = (lerp(2.0, 0.5, Clarity), lerp(2.0, 0.7, Clarity), 0.3, 0). Use `UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, Name, FLinearColor(...))`.

Important — the plane coordinates on GPU use the anchor at altitude 0 (`AnchorEcef` in `SetAnchor` is at alt 0), matching the CPU.

In `ACamSimEnvironment::Tick`: `const FCamSimTelemetry Tel = CamSimCameraActor ? CamSimCameraActor->GetCurrentTelemetry() : FCamSimTelemetry(); OceanManager.Tick(CamSimCameraActor ? &Tel : nullptr);`

- [ ] **Step 5: Build, run unit tests, and look at it.** `scripts/run.sh --build-only && run_tests CamSim` → all pass. Then launch (`scripts/run.sh`), point the camera at SF Bay with `scripts/send_cigi_test.py` (nadir over 37.81, −122.42 at 1000 m) and grab a frame from the stream (`scripts/test_video_output.sh` or ffmpeg from the multicast/unicast output) — expected: blue water over the bay, land intact, no black/missing sea, no seams. If the sea is invisible from above, flip the triangle winding in `BuildOceanMesh` and add a comment saying which winding UE needs. Record the logged rebuild time.

- [ ] **Step 6: Commit** — `git commit -m "feat(ocean): draw the sea — warped-grid procedural mesh, MPC-driven Gerstner waves, fixed wave anchor"`

---

### Task 10: GPU test — the material matches the CPU waves

**Files:**
- Create: `Tests/OceanGpuTest.cpp`

**Interfaces:**
- Consumes: Task 8 material/MPC, Task 9 `FOceanMesh`, `CamSimOcean::WriteMpc(UWorld*, UMaterialParameterCollection*, const FOceanSurface&, const FVector& ComponentWorld, const FMatrix& EcefToUe)` (`Ocean/FOceanManager.h`).

- [ ] **Step 1: Write the test** — `CamSim.GPU.Ocean.MatchesCpu`, skipped under NullRHI like `SensorGpuTest` (`if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }`):
  1. Create a transient game world (`UWorld::CreateWorld(EWorldType::Game, false)`, `FWorldContext`, `InitializeActorsForPlay`) — follow `EntityPreloadGpuTest.cpp` if it already builds a world for rendering; reuse its helper.
  2. `FOceanSurface` with a constant geoid 0, anchor (0, 0), one host wave (H 2 m, L 40 m, from 180°), time 3.0 s. `EcefToUe` = a translation making ECEF(anchor) the UE origin and scaling metres → cm with axes (UE X = North, Y = East, Z = Up) — build it from `GetAxis*Ecef`.
  3. `BuildOceanMesh` at the anchor with R = 200 m (uniform 1.56 m cells — waves of 40 m are not faded), upload with `FOceanMesh`, `WriteMpc`.
  4. `ASceneCapture2D` straight down from 100 m, orthographic, width 100 m, capture `SCS_SceneDepth` into a 128×128 `RTF_R32f` render target; `CaptureScene()`; `FlushRenderingCommands()`; read pixels.
  5. For a 9×9 grid of pixels: world XY from the ortho mapping, expected height = `Ocean.GetWaves().HeightAtPlane(N, E)` (N = X/100, E = Y/100), rendered height = 100 m − depth/100. Assert max |diff| ≤ 0.02 m; `AddInfo` the max.
- [ ] **Step 2: Run** — `scripts/run_gpu_tests.sh CamSim.GPU.Ocean` → PASS (first run compiles shaders; minutes). If it fails, compare the HLSL to `FOceanWaves` term by term (sign of the horizontal term, phase, fade) before touching tolerances.
- [ ] **Step 3: Commit** — `git commit -m "test(ocean): GPU depth of M_Ocean matches FOceanWaves within 2 cm"`

---

### Task 11: Acceptance, defaults, ROADMAP and docs

**Files:**
- Create: `scripts/ocean_check.py`
- Modify: `ROADMAP.md` (new "### 2.6 Ocean surface for boats"; update 2.1 open items (ocean waves now sim time); 2.2 "ocean surface is still a flat UE-world plane (parked)" → resolved; 2.5 carry-overs; "Parked" list drops ocean)
- Modify: `docs/dis.md` (boats float at sea level; HOT over water), `docs/configuration.md` (final default), `CLAUDE.md` (one Gotcha line: ocean + `make_ocean_material.sh`; add `scripts/ocean_check.py` to Commands), `deploy/camsim_config.yaml` (final default)
- Create: `docs/images/ocean/*.jpg` (selected shots)

- [ ] **Step 1: Acceptance script** — `scripts/ocean_check.py OUTDIR [--beaufort 0,3,6] [--cigi]`, built on `scripts/dis_vehicle_check.py` (import its harness functions the same way it imports `run_bench`/`scenario`; do not copy them). For each sea state: launch CamSim headless with `CAMSIM_OCEAN_BEAUFORT=<b>`, DIS + ground truth on; run `send_dis_test.py boat-circle`; drive the camera over CIGI through: nadir wide (1000 m), oblique 12° FOV close-up following the boat (~250 m slant), waterline (6 m above sea level, 40 m away), Golden Gate coastline oblique, 10 km altitude horizon. For `--cigi`: send Wave Control (two waves: 1.5 m/45 m toward 60°, 0.8 m/20 m toward 100°) via a small CCL-free packer in the script (opcode 14, 32 bytes, CIGI 3.3 layout) and one HAT/HOT request (opcode 24) at the boat's current position; read the HOT response.
  Checks (print PASS/FAIL per line; exit non-zero on any FAIL):
  - COCO: exactly one `boat` entity_id across all frames.
  - Boat altitude from `frames.jsonl` ground truth vs EGM96 sea level at its position (sample with the geoid sampler in `scripts/klv_conformance` or `scripts/make_egm96_dac.py`'s reader): |alt − sea| ≤ 0.5 m + a_max (a_max = table Hs/2·1.5).
  - HOT at the boat (CIGI run): within 0.5 m + a_max of sea level (not ≈ −55 m).
  - Frame times over the settled windows: median ≤ 33 ms, none > 66 ms.
  - Log: max "Ocean: mesh rebuilt in X ms" ≤ 10 ms (report; FAIL only if any > 30 ms).
  Save the shots to `OUTDIR/shots/<beaufort>_<view>.png`.
- [ ] **Step 2: Run** — `uv run -q --with numpy --with pillow python scripts/ocean_check.py .cache/ocean_check` (all sea states + `--cigi`). Look at every shot yourself: water over the bay, boat at its draft (not floating above or sunk), no clipping through crests at Beaufort 6, clean coastline (note any beach shimmer), seabed visible in shallows, no mesh edge at 10 km. Measure the ocean GPU cost: one run with `CAMSIM_OCEAN_ENABLED=0` vs on, same views, using `scripts/bench/run_bench.py --smoke` and `compare.py` if they accept env overrides; otherwise compare the frame-time medians from the check.
- [ ] **Step 3: Decide the default.** If all checks pass, keep `enabled: true`. If frame times fail only because of the ocean, set `enabled: false` in `FOceanConfig`, `deploy/camsim_config.yaml`, `docs/configuration.md`, and update `CamSim.Ocean.Config.Defaults`; record why.
- [ ] **Step 4: ROADMAP 2.6** — write the section in the style of 2.5: intent, the rule-5 exception, what was built (files), acceptance results with numbers (frames, altitude error, HOT, frame-time medians/max, rebuild ms, GPU cost), a shot table (copy 4–6 shots to `docs/images/ocean/` as JPEG ≤ 200 KB each), findings, carry-overs (from the spec's Carry-overs list plus anything found), and any deviations (e.g. SLW → Default Lit, component translate dropped). Update the other ROADMAP lines listed under Files.
- [ ] **Step 5: Full test pass** — `run_tests CamSim` (NullRHI) and `scripts/run_gpu_tests.sh` → all pass; `node scripts/klv_conformance/check.js` on a capture from the check still passes (KLV unaffected). Update the test counts in `CLAUDE.md` ("N tests across M files") to the new totals from `index.json`.
- [ ] **Step 6: Commit** — `git add -A scripts/ocean_check.py ROADMAP.md docs CLAUDE.md deploy unreal_project/CamSimTest/Source && git commit -m "docs(ocean): ROADMAP 2.6 acceptance — boats on a globe-conforming sea with sea state"`
