# ATR Ground Truth (Tight Boxes, OBBs, Occlusion) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** COCO labels fit each vehicle's rendered pixels in the encoded frame, with modal/amodal boxes, rotated boxes, the projected 3D box, visibility, truncation and an RLE mask.

**Architecture:** Entities render custom depth with a per-entity stencil value. A compute pass in the sensor graph (same distortion resample as `SensorCS`) writes an output-space `uint16` per pixel (`visible | amodal << 8`) that is read back into the readback-ring slot. A pure-C++ analyzer on the task thread turns it into per-entity boxes, OBBs (rotating calipers), visibility and RLE; the game-thread snapshot adds the oriented 3D box projected through pinhole + forward distortion, which also gives truncation.

**Tech Stack:** UE 5.8 C++ (RDG, global compute shaders, automation tests), HLSL (`/CamSim` virtual dir), Python 3.10+ (acceptance script, pytest).

**Spec:** `docs/superpowers/specs/2026-09-30-atr-ground-truth-design.md`

## Global Constraints

- Shader: plain compute, integer ops only — no float atomics, no wave intrinsics (Metal + Vulkan portable).
- The distortion inverse in the new shader must be the same recurrence as `SensorCS` (`NEWTON_ITERATIONS` = `FSensorFrameParams::NewtonIterations` = 3, Newton from r = Rd); `CamSim.GPU.Sensor.*` must keep passing after the refactor (Y ≤ 1 DN, UV ≤ 2 DN).
- Stencil values 1..255; 0 = no entity. A released value is reused only ≥ 4 frames later.
- With `ml_training.enabled` false or `bounding_boxes` false: nothing is tagged, no pass, no readback.
- COCO: `bbox`/`area` keep COCO semantics (modal, mask area); every other change is an added field. RLE is pycocotools' compressed string, column-major; JSON-escape `\` and `"`.
- OBB format `[cx, cy, w, h, angle_deg]`, `w ≥ h`, angle of the `w` axis from image +x toward +y in [-90, 90); squares in [-45, 45).
- 3D corner order: bottom face then top; each rear-left, rear-right, front-right, front-left (body X fwd, Y right, Z up).
- Pixel coordinates are continuous with pixel `i` spanning [i, i+1) (centre i + 0.5), the convention of `FEntityProjection::ProjectAABB` and `SensorCS` (`Xd = (P.x + 0.5 - W/2) / FocalPx`).
- Copyright header `// Copyright CamSim Contributors. All Rights Reserved.`; `CoreMinimal.h` first; UE naming.
- New config keys: `ml_training.min_visible_pixels` (default 1, `CAMSIM_ML_MIN_VISIBLE_PIXELS`), `ml_training.segmentation` (default true, `CAMSIM_ML_SEGMENTATION_ENABLED`).

## Commands

- Build: `scripts/run.sh --build-only` (never pipe through `tee` without `set -o pipefail`).
- **`run_tests <Filter>`** = `"$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject -ExecCmds="Automation RunTests <Filter>+Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath=.cache/automation-report -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput` with `UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"`; read `.cache/automation-report/index.json` with `encoding="utf-8-sig"` (`succeeded`, `failed`, per-test `state`).
- GPU tests: `scripts/run_gpu_tests.sh <Filter>` (Metal; prints FAIL lines and counts).
- Python tests: `cd scripts && uv run -q --with pytest --with numpy --with pycocotools pytest tests/<file> -q`.

## Review Focus

1. **Entity destroyed while its frames are in flight** — its stencil value must not be handed to a new entity until the frames that saw it have been analysed (else the new entity inherits the old one's pixels). Test: `CamSim.GroundTruth.StencilAllocator.DelayedReuse` (Task 4).
2. **ID image missing or the wrong size** (readback failed, capture size hot-reloaded) — annotations fall back to the projected box (`mask_source: projection`), never crash or read out of bounds. Test: `CamSim.GroundTruth.Analyzer.SizeMismatchFallsBack` (Task 2) and `CamSim.GroundTruth.Collector.FallbackWithoutIds` (Task 6).
3. **Vehicle partly behind the near plane / camera** — `corners_px` null, truncation falls back to "amodal mask touches the edge". Tests: `CamSim.GroundTruth.Box3D.BehindCamera` (Task 3), `CamSim.GroundTruth.Analyzer.EdgeTouchTruncation` (Task 2).
4. **RLE strings containing `\`** — the JSONL line must still parse. Test: `CamSim.GroundTruth.Coco.RleEscaped` (Task 6) using the pycocotools fixture `a03\a1`.
5. **Two tagged vehicles overlapping** — the front one's pixels must not count for the rear one, and the rear one's amodal equals its own stencil coverage (documented limitation). Test: `CamSim.GroundTruth.Analyzer.TwoEntitiesOverlap` (Task 2).

---

## File Structure

| File | Responsibility |
|---|---|
| Create `Source/CamSimTest/GroundTruth/MaskGeometry.h/.cpp` | `CamSimMask::` convex hull, min-area rect, polygon area, rect clip, COCO RLE string encoding |
| Create `Source/CamSimTest/GroundTruth/InstanceMaskAnalyzer.h/.cpp` | `FInstanceIdImage`, `FInstanceMaskAnalyzer::Analyze` |
| Modify `Source/CamSimTest/GroundTruth/AnnotationTypes.h` | new fields on `FEntityAnnotationData`; optics on `FViewProjectionData` |
| Modify `Source/CamSimTest/GroundTruth/FEntityProjection.h/.cpp` | `ProjectOrientedBox`, `DistortPixel` |
| Create `Source/CamSimTest/Entity/StencilSlotAllocator.h` | header-only `FStencilSlotAllocator` |
| Modify `Source/CamSimTest/Entity/CamSimEntity.h/.cpp` | `SetGroundTruthStencil`, reapply after mesh loads |
| Modify `Source/CamSimTest/Entity/CamSimEntityManager.h/.cpp` | allocate/release stencil values; snapshot fills stencil + 3D box |
| Modify `Shaders/Private/CamSimSensorCommon.ush`, `CamSimSensor.usf` | shared `UndistortScale` |
| Create `Shaders/Private/CamSimInstanceId.usf` | `InstanceIdCS` |
| Create `Source/CamSimShaders/Public/InstanceIdPass.h`, `Private/InstanceIdPass.cpp` | `AddInstanceIdPass` |
| Modify `Source/CamSimTest/Camera/FrameGrabRequestQueue.h`, `CamSimFrameGrabExtension.h/.cpp`, `CamSimCaptureComponent.h/.cpp` | request flag, second readback per slot, slot `InstanceIds` |
| Modify `Source/CamSimTest/GroundTruth/FGroundTruthCollector.h/.cpp`, `FCocoAnnotationWriter.cpp`, `FVocAnnotationWriter.cpp` | run the analyzer; new output fields |
| Modify `Source/CamSimTest/Config/CamSimConfig.h/.cpp`, `deploy/camsim_config.yaml`, `docs/configuration.md` | two new keys |
| Modify `Config/DefaultEngine.ini` | `r.CustomDepth=3` |
| Create `Tests/MaskGeometryTest.cpp`, `Tests/InstanceMaskAnalyzerTest.cpp`, `Tests/InstanceIdGpuTest.cpp`; modify `Tests/GroundTruthTest.cpp` | automation tests |
| Create `scripts/gt_occlusion_check.py`, `scripts/gt_check_lib.py`, `scripts/tests/test_gt_check_lib.py` | acceptance |
| Create `docs/ground-truth.md`; modify `ROADMAP.md`, `CLAUDE.md` | docs |

All `Source/...`, `Shaders/...`, `Config/...` paths are under `unreal_project/CamSimTest/`.

---

### Task 1: Mask geometry helpers

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/MaskGeometry.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/MaskGeometry.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/MaskGeometryTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace CamSimMask {
    struct FOrientedBox { double Cx = 0, Cy = 0, W = 0, H = 0, AngleDeg = 0; };
    TArray<FVector2D> ConvexHull(TArray<FVector2D> Points);              // CCW in a y-up sense, no repeats, collinear dropped
    FOrientedBox      MinAreaRect(const TArray<FVector2D>& Hull);          // conventions in Global Constraints
    double            PolygonArea(const TArray<FVector2D>& Poly);          // absolute
    TArray<FVector2D> ClipToRect(const TArray<FVector2D>& Poly, const FBox2D& Rect);  // Sutherland–Hodgman
    FString           EncodeCocoRle(const TArray<uint32>& Runs);           // Runs alternate 0s,1s,0s… starting with zeros
  }
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/MaskGeometryTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/MaskGeometry.h"

// CamSim.GroundTruth.Mask.*: pure geometry and COCO RLE (fixtures from pycocotools 2.0,
// `mask.encode(np.asfortranarray(a))["counts"]`).

namespace
{
	TArray<FVector2D> RectCorners(double X0, double Y0, double X1, double Y1)
	{
		return { {X0, Y0}, {X1, Y0}, {X1, Y1}, {X0, Y1} };
	}
	TArray<FVector2D> Rotated(double Cx, double Cy, double W, double H, double Deg)
	{
		const double T = FMath::DegreesToRadians(Deg), C = FMath::Cos(T), S = FMath::Sin(T);
		TArray<FVector2D> P;
		for (const FVector2D& L : { FVector2D(-W/2, -H/2), FVector2D(W/2, -H/2), FVector2D(W/2, H/2), FVector2D(-W/2, H/2) })
			P.Add(FVector2D(Cx + L.X * C - L.Y * S, Cy + L.X * S + L.Y * C));
		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskMinAreaRectAxisTest, "CamSim.GroundTruth.Mask.MinAreaRectAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskMinAreaRectAxisTest::RunTest(const FString&)
{
	const CamSimMask::FOrientedBox B = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(10, 20, 50, 30)));
	TestNearlyEqual(TEXT("cx"), B.Cx, 30.0, 1e-6);
	TestNearlyEqual(TEXT("cy"), B.Cy, 25.0, 1e-6);
	TestNearlyEqual(TEXT("w (long side)"), B.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("h"), B.H, 10.0, 1e-6);
	TestNearlyEqual(TEXT("angle"), B.AngleDeg, 0.0, 1e-6);
	// Tall rect: w is still the long side, axis along +y => angle -90
	const CamSimMask::FOrientedBox T = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(0, 0, 10, 40)));
	TestNearlyEqual(TEXT("tall w"), T.W, 40.0, 1e-6);
	TestNearlyEqual(TEXT("tall angle"), T.AngleDeg, -90.0, 1e-6);
	// Square: angle folded into [-45, 45)
	const CamSimMask::FOrientedBox Q = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(RectCorners(0, 0, 8, 8)));
	TestNearlyEqual(TEXT("square angle"), Q.AngleDeg, 0.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskMinAreaRectRotatedTest, "CamSim.GroundTruth.Mask.MinAreaRectRotated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskMinAreaRectRotatedTest::RunTest(const FString&)
{
	for (const double Deg : { 30.0, -60.0, 89.0, -89.0 })
	{
		TArray<FVector2D> Pts = Rotated(100, 80, 60, 20, Deg);
		Pts.Add(FVector2D(100, 80));  // interior points must not matter
		const CamSimMask::FOrientedBox B = CamSimMask::MinAreaRect(CamSimMask::ConvexHull(Pts));
		TestNearlyEqual(*FString::Printf(TEXT("w @%g"), Deg), B.W, 60.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("h @%g"), Deg), B.H, 20.0, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("angle @%g"), Deg), B.AngleDeg, Deg, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("cx @%g"), Deg), B.Cx, 100.0, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskHullDegenerateTest, "CamSim.GroundTruth.Mask.HullDegenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskHullDegenerateTest::RunTest(const FString&)
{
	TestEqual(TEXT("empty"), CamSimMask::ConvexHull({}).Num(), 0);
	TestEqual(TEXT("one point"), CamSimMask::ConvexHull({ {1, 1}, {1, 1} }).Num(), 1);
	const TArray<FVector2D> Line = CamSimMask::ConvexHull({ {0, 0}, {1, 1}, {2, 2}, {3, 3} });
	TestEqual(TEXT("collinear -> 2 ends"), Line.Num(), 2);
	const CamSimMask::FOrientedBox L = CamSimMask::MinAreaRect(Line);
	TestNearlyEqual(TEXT("line h"), L.H, 0.0, 1e-9);
	TestNearlyEqual(TEXT("line angle"), L.AngleDeg, 45.0, 1e-6);
	const CamSimMask::FOrientedBox P = CamSimMask::MinAreaRect(CamSimMask::ConvexHull({ {5, 6} }));
	TestNearlyEqual(TEXT("point w"), P.W, 0.0, 1e-9);
	TestNearlyEqual(TEXT("point cx"), P.Cx, 5.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskClipAreaTest, "CamSim.GroundTruth.Mask.ClipArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskClipAreaTest::RunTest(const FString&)
{
	const TArray<FVector2D> Sq = RectCorners(-10, 0, 10, 10);  // half outside x < 0
	TestNearlyEqual(TEXT("area"), CamSimMask::PolygonArea(Sq), 200.0, 1e-9);
	const TArray<FVector2D> Clipped = CamSimMask::ClipToRect(Sq, FBox2D(FVector2D(0, 0), FVector2D(100, 100)));
	TestNearlyEqual(TEXT("clipped area"), CamSimMask::PolygonArea(Clipped), 100.0, 1e-9);
	const TArray<FVector2D> Outside = CamSimMask::ClipToRect(RectCorners(200, 200, 210, 210), FBox2D(FVector2D(0, 0), FVector2D(100, 100)));
	TestNearlyEqual(TEXT("fully outside"), CamSimMask::PolygonArea(Outside), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaskRleTest, "CamSim.GroundTruth.Mask.CocoRle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMaskRleTest::RunTest(const FString&)
{
	// 4x5 (H x W) image, rows 1..2, cols 1..3 set: column-major runs 5,2,2,2,2,2,5
	TestEqual(TEXT("rect"), CamSimMask::EncodeCocoRle({ 5, 2, 2, 2, 2, 2, 5 }), FString(TEXT("5220003")));
	TestEqual(TEXT("corner"), CamSimMask::EncodeCocoRle({ 0, 1, 19 }), FString(TEXT("01c0")));
	TestEqual(TEXT("empty"), CamSimMask::EncodeCocoRle({ 20 }), FString(TEXT("d0")));
	TestEqual(TEXT("full"), CamSimMask::EncodeCocoRle({ 0, 20 }), FString(TEXT("0d0")));
	// 40x40, a 3-pixel column-major run starting at index 17: the string holds a backslash
	TestEqual(TEXT("backslash"), CamSimMask::EncodeCocoRle({ 17, 3, 1580 }), FString(TEXT("a03\\a1")));
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `scripts/run.sh --build-only` → Expected: compile error, `GroundTruth/MaskGeometry.h` not found.

- [ ] **Step 3: Implement** — `MaskGeometry.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Pure 2D helpers for mask-based ground truth (sub-project 2 of "boats and trucks for ATR").
 * Image coordinates: x right, y down, pixel i spans [i, i + 1).
 */
namespace CamSimMask
{
	/** Rotated rectangle: w >= h; AngleDeg is the w axis from +x toward +y, in [-90, 90) ([-45, 45) when w == h). */
	struct FOrientedBox { double Cx = 0, Cy = 0, W = 0, H = 0, AngleDeg = 0; };

	/** Andrew's monotone chain. Duplicates and collinear points dropped; 0, 1 or 2 points for degenerate input. */
	TArray<FVector2D> ConvexHull(TArray<FVector2D> Points);

	/** Minimum-area enclosing rectangle of a convex hull (rotating calipers over hull edges). Ties prefer the smaller |angle|. */
	FOrientedBox MinAreaRect(const TArray<FVector2D>& Hull);

	/** Absolute shoelace area. */
	double PolygonArea(const TArray<FVector2D>& Poly);

	/** Sutherland–Hodgman clip of a convex or simple polygon against an axis-aligned rect. */
	TArray<FVector2D> ClipToRect(const TArray<FVector2D>& Poly, const FBox2D& Rect);

	/** pycocotools rleToString: Runs are column-major run lengths, alternating, starting with a (possibly 0) zero run. */
	FString EncodeCocoRle(const TArray<uint32>& Runs);
}
```

`MaskGeometry.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/MaskGeometry.h"

namespace CamSimMask
{
	namespace
	{
		double Cross(const FVector2D& O, const FVector2D& A, const FVector2D& B)
		{
			return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
		}

		double FoldAngle(double Deg, bool bSquare)
		{
			const double Period = bSquare ? 90.0 : 180.0, Lo = bSquare ? -45.0 : -90.0;
			while (Deg >= Lo + Period) Deg -= Period;
			while (Deg < Lo) Deg += Period;
			return Deg;
		}
	}

	TArray<FVector2D> ConvexHull(TArray<FVector2D> P)
	{
		P.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });
		TArray<FVector2D> U;
		for (const FVector2D& Q : P) { if (U.Num() == 0 || U.Last() != Q) U.Add(Q); }
		if (U.Num() < 3) return U;
		TArray<FVector2D> H; H.SetNum(2 * U.Num());
		int32 K = 0;
		for (int32 I = 0; I < U.Num(); ++I)
		{
			while (K >= 2 && Cross(H[K - 2], H[K - 1], U[I]) <= 0) --K;
			H[K++] = U[I];
		}
		for (int32 I = U.Num() - 2, T = K + 1; I >= 0; --I)
		{
			while (K >= T && Cross(H[K - 2], H[K - 1], U[I]) <= 0) --K;
			H[K++] = U[I];
		}
		H.SetNum(K - 1);
		return H;
	}

	FOrientedBox MinAreaRect(const TArray<FVector2D>& Hull)
	{
		FOrientedBox Best;
		if (Hull.Num() == 0) return Best;
		if (Hull.Num() == 1) { Best.Cx = Hull[0].X; Best.Cy = Hull[0].Y; return Best; }
		double BestArea = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Hull.Num(); ++I)
		{
			const FVector2D E = Hull[(I + 1) % Hull.Num()] - Hull[I];
			const double Len = E.Size();
			if (Len <= 0.0) continue;
			const FVector2D U = E / Len, V(-U.Y, U.X);
			double MinU = TNumericLimits<double>::Max(), MaxU = -MinU, MinV = MinU, MaxV = -MinU;
			for (const FVector2D& P : Hull)
			{
				const double Pu = FVector2D::DotProduct(P, U), Pv = FVector2D::DotProduct(P, V);
				MinU = FMath::Min(MinU, Pu); MaxU = FMath::Max(MaxU, Pu);
				MinV = FMath::Min(MinV, Pv); MaxV = FMath::Max(MaxV, Pv);
			}
			FOrientedBox B;
			double Wu = MaxU - MinU, Hv = MaxV - MinV;
			const double Cu = 0.5 * (MinU + MaxU), Cv = 0.5 * (MinV + MaxV);
			B.Cx = Cu * U.X + Cv * V.X;
			B.Cy = Cu * U.Y + Cv * V.Y;
			double Angle = FMath::RadiansToDegrees(FMath::Atan2(U.Y, U.X));
			if (Wu < Hv) { Swap(Wu, Hv); Angle += 90.0; }
			B.W = Wu; B.H = Hv;
			B.AngleDeg = FoldAngle(Angle, FMath::IsNearlyEqual(Wu, Hv, 1e-9));
			const double Area = Wu * Hv;
			if (Area < BestArea - 1e-9 || (FMath::Abs(Area - BestArea) <= 1e-9 && FMath::Abs(B.AngleDeg) < FMath::Abs(Best.AngleDeg)))
			{
				BestArea = Area;
				Best = B;
			}
		}
		return Best;
	}

	double PolygonArea(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		for (int32 I = 0; I < P.Num(); ++I)
		{
			const FVector2D& Q = P[I]; const FVector2D& R = P[(I + 1) % P.Num()];
			A += Q.X * R.Y - R.X * Q.Y;
		}
		return FMath::Abs(0.5 * A);
	}

	TArray<FVector2D> ClipToRect(const TArray<FVector2D>& Poly, const FBox2D& Rect)
	{
		// Edges: x >= Min.X, x <= Max.X, y >= Min.Y, y <= Max.Y
		auto Inside = [&](const FVector2D& P, int32 E)
		{
			switch (E) { case 0: return P.X >= Rect.Min.X; case 1: return P.X <= Rect.Max.X; case 2: return P.Y >= Rect.Min.Y; default: return P.Y <= Rect.Max.Y; }
		};
		auto Hit = [&](const FVector2D& A, const FVector2D& B, int32 E)
		{
			const double T = (E < 2)
				? ((E == 0 ? Rect.Min.X : Rect.Max.X) - A.X) / (B.X - A.X)
				: ((E == 2 ? Rect.Min.Y : Rect.Max.Y) - A.Y) / (B.Y - A.Y);
			return A + (B - A) * T;
		};
		TArray<FVector2D> Out = Poly;
		for (int32 E = 0; E < 4 && Out.Num() > 0; ++E)
		{
			TArray<FVector2D> In = MoveTemp(Out);
			Out.Reset();
			for (int32 I = 0; I < In.Num(); ++I)
			{
				const FVector2D& Cur = In[I]; const FVector2D& Prev = In[(I + In.Num() - 1) % In.Num()];
				const bool bCur = Inside(Cur, E), bPrev = Inside(Prev, E);
				if (bCur) { if (!bPrev) Out.Add(Hit(Prev, Cur, E)); Out.Add(Cur); }
				else if (bPrev) { Out.Add(Hit(Prev, Cur, E)); }
			}
		}
		return Out;
	}

	FString EncodeCocoRle(const TArray<uint32>& Runs)
	{
		FString S;
		for (int32 I = 0; I < Runs.Num(); ++I)
		{
			int64 X = Runs[I];
			if (I > 2) X -= static_cast<int64>(Runs[I - 2]);
			bool bMore = true;
			while (bMore)
			{
				int64 C = X & 0x1f;
				X >>= 5;  // arithmetic shift: int64 is signed
				bMore = (C & 0x10) ? X != -1 : X != 0;
				if (bMore) C |= 0x20;
				S.AppendChar(static_cast<TCHAR>(C + 48));
			}
		}
		return S;
	}
}
```

- [ ] **Step 4: Run tests** — Run: `scripts/run.sh --build-only && run_tests CamSim.GroundTruth.Mask` → Expected: 5 succeeded, 0 failed.
- [ ] **Step 5: Commit** — `git add unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/MaskGeometry.* unreal_project/CamSimTest/Source/CamSimTest/Tests/MaskGeometryTest.cpp && git commit -m "feat(gt): mask geometry — convex hull, min-area rect, rect clip, COCO RLE"`

---

### Task 2: Annotation fields and the instance-mask analyzer

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/AnnotationTypes.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/InstanceMaskAnalyzer.h/.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/InstanceMaskAnalyzerTest.cpp`

**Interfaces:**
- Consumes: `CamSimMask::ConvexHull`, `MinAreaRect`, `EncodeCocoRle`, `FOrientedBox` (Task 1).
- Produces (in `AnnotationTypes.h`, added to `FEntityAnnotationData` after `bTruncated`):
  ```cpp
  #include "GroundTruth/MaskGeometry.h"   // at the top
  uint8   StencilValue  = 0;      // custom-depth stencil 1..255; 0 = untagged (projection fallback)
  // Oriented 3D box (game thread, Task 4)
  bool    bHasBox3D     = false;
  FVector Box3DSizeM    = FVector::ZeroVector;   // body X (length), Y (width), Z (height), metres
  double  YawDeg = 0.0, PitchDeg = 0.0, RollDeg = 0.0;  // CIGI convention (heading from true north)
  bool    bCornersValid = false;                 // false: a corner is behind the near plane
  FVector2D CornersPx[8];                         // output pixels, order in Global Constraints
  double  Truncation    = -1.0;                  // [0,1]; < 0 = unknown
  // Mask analysis (task thread, FInstanceMaskAnalyzer)
  bool    bMaskMeasured = false;                 // true: ScreenBBox and below come from the rendered mask
  int32   VisiblePixels = 0;
  int32   AmodalPixels  = 0;
  FBox2D  AmodalBBox    = FBox2D(ForceInit);     // pixel-edge coords, like ScreenBBox when measured
  CamSimMask::FOrientedBox Obb, ObbAmodal;
  FString SegmentationRle;                        // empty when segmentation is off
  ```
  and in `AnnotationTypes.h`:
  ```cpp
  /** Output-space instance IDs (ROADMAP 2.7): two pixels per word, pixel 2k in the low 16 bits;
   *  each pixel = visible stencil | (amodal stencil << 8). */
  struct FInstanceIdImage
  {
      int32 Width = 0, Height = 0;
      TArray<uint32> Words;
      bool IsValid() const { return Width > 0 && Height > 0 && (Width % 2) == 0 && Words.Num() == Width * Height / 2; }
      uint16 At(int32 X, int32 Y) const { const int32 I = Y * Width + X; const uint32 W = Words[I >> 1]; return static_cast<uint16>((I & 1) ? (W >> 16) : (W & 0xFFFF)); }
  };
  ```
  `InstanceMaskAnalyzer.h`:
  ```cpp
  struct FInstanceMaskAnalyzer
  {
      /** Fills the mask fields of every entity with StencilValue != 0, sets ScreenBBox to the modal box and
       *  bTruncated, and removes tagged entities with VisiblePixels < MinVisiblePixels. Invalid Ids: no-op. */
      static void Analyze(const FInstanceIdImage& Ids, TArray<FEntityAnnotationData>& Entities,
                          int32 MinVisiblePixels, bool bSegmentation);
  };
  ```

- [ ] **Step 1: Write failing tests** — `Tests/InstanceMaskAnalyzerTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/InstanceMaskAnalyzer.h"

namespace
{
	FInstanceIdImage MakeImage(int32 W, int32 H) { FInstanceIdImage I; I.Width = W; I.Height = H; I.Words.Init(0u, W * H / 2); return I; }
	void Set(FInstanceIdImage& I, int32 X, int32 Y, uint8 Visible, uint8 Amodal)
	{
		const int32 Idx = Y * I.Width + X; uint32& W = I.Words[Idx >> 1];
		const uint32 V = uint32(Visible) | (uint32(Amodal) << 8);
		W = (Idx & 1) ? ((W & 0x0000FFFFu) | (V << 16)) : ((W & 0xFFFF0000u) | V);
	}
	FEntityAnnotationData Tagged(uint8 Stencil, uint32 Id)
	{
		FEntityAnnotationData E; E.StencilValue = Stencil; E.EntityId = Id; E.bVisible = true;
		E.ScreenBBox = FBox2D(FVector2D(0, 0), FVector2D(99, 99)); E.Truncation = 0.0; return E;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerPartlyHiddenTest, "CamSim.GroundTruth.Analyzer.PartlyHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerPartlyHiddenTest::RunTest(const FString&)
{
	// 20x10 image; entity 3 amodal = cols 4..11, rows 2..5 (32 px); visible = cols 4..7 (16 px)
	FInstanceIdImage I = MakeImage(20, 10);
	for (int32 Y = 2; Y <= 5; ++Y) for (int32 X = 4; X <= 11; ++X) Set(I, X, Y, X <= 7 ? 3 : 0, 3);
	TArray<FEntityAnnotationData> E = { Tagged(3, 42) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	if (!TestEqual(TEXT("kept"), E.Num(), 1)) return false;
	TestTrue(TEXT("measured"), E[0].bMaskMeasured);
	TestEqual(TEXT("visible px"), E[0].VisiblePixels, 16);
	TestEqual(TEXT("amodal px"), E[0].AmodalPixels, 32);
	TestEqual(TEXT("modal min"), E[0].ScreenBBox.Min, FVector2D(4, 2));
	TestEqual(TEXT("modal max (edge)"), E[0].ScreenBBox.Max, FVector2D(8, 6));
	TestEqual(TEXT("amodal max"), E[0].AmodalBBox.Max, FVector2D(12, 6));
	TestNearlyEqual(TEXT("obb w"), E[0].Obb.W, 4.0, 1e-6);   // 4x4 visible square
	TestNearlyEqual(TEXT("obb amodal w"), E[0].ObbAmodal.W, 8.0, 1e-6);
	TestNearlyEqual(TEXT("obb amodal cx"), E[0].ObbAmodal.Cx, 8.0, 1e-6);
	TestFalse(TEXT("rle present"), E[0].SegmentationRle.IsEmpty());
	TestFalse(TEXT("not truncated"), E[0].bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerRleMatchesCocoTest, "CamSim.GroundTruth.Analyzer.RleMatchesCoco",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerRleMatchesCocoTest::RunTest(const FString&)
{
	// Same mask as the Mask.CocoRle "rect" fixture: H=4, W=6 (even), rows 1..2, cols 1..3 -> runs 5,2,2,2,2,2,9
	FInstanceIdImage I = MakeImage(6, 4);
	for (int32 Y = 1; Y <= 2; ++Y) for (int32 X = 1; X <= 3; ++X) Set(I, X, Y, 7, 7);
	TArray<FEntityAnnotationData> E = { Tagged(7, 1) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestEqual(TEXT("rle"), E[0].SegmentationRle, CamSimMask::EncodeCocoRle({ 5, 2, 2, 2, 2, 2, 9 }));
	TArray<FEntityAnnotationData> NoSeg = { Tagged(7, 1) };
	FInstanceMaskAnalyzer::Analyze(I, NoSeg, 1, false);
	TestTrue(TEXT("segmentation off"), NoSeg[0].SegmentationRle.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerTwoEntitiesTest, "CamSim.GroundTruth.Analyzer.TwoEntitiesOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerTwoEntitiesTest::RunTest(const FString&)
{
	// Entity 1 at cols 0..5, entity 2 in front at cols 4..9 (custom depth: front one owns 4..5 in both channels)
	FInstanceIdImage I = MakeImage(10, 2);
	for (int32 X = 0; X < 10; ++X) { const uint8 S = X >= 4 ? 2 : 1; Set(I, X, 0, S, S); }
	TArray<FEntityAnnotationData> E = { Tagged(1, 10), Tagged(2, 20) };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestEqual(TEXT("rear visible"), E[0].VisiblePixels, 4);
	TestEqual(TEXT("rear amodal = own coverage"), E[0].AmodalPixels, 4);
	TestEqual(TEXT("front visible"), E[1].VisiblePixels, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerDropsHiddenTest, "CamSim.GroundTruth.Analyzer.DropsHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerDropsHiddenTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 2);
	for (int32 X = 0; X < 3; ++X) Set(I, X, 0, 0, 5);   // fully hidden entity 5
	Set(I, 6, 1, 6, 6);                                 // entity 6: 1 visible px
	FEntityAnnotationData Untagged; Untagged.EntityId = 99; Untagged.bVisible = true;
	Untagged.ScreenBBox = FBox2D(FVector2D(1, 1), FVector2D(3, 3));
	TArray<FEntityAnnotationData> E = { Tagged(5, 1), Tagged(6, 2), Untagged, Tagged(9, 3) /* not in image */ };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	if (!TestEqual(TEXT("kept two"), E.Num(), 2)) return false;
	TestEqual(TEXT("entity 6 kept"), E[0].EntityId, 2u);
	TestEqual(TEXT("untagged kept as projection"), E[1].EntityId, 99u);
	TestFalse(TEXT("untagged not measured"), E[1].bMaskMeasured);
	TArray<FEntityAnnotationData> E2 = { Tagged(6, 2) };
	FInstanceMaskAnalyzer::Analyze(I, E2, 2, true);
	TestEqual(TEXT("min_visible_pixels=2 drops 1-px entity"), E2.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerEdgeTruncationTest, "CamSim.GroundTruth.Analyzer.EdgeTouchTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerEdgeTruncationTest::RunTest(const FString&)
{
	FInstanceIdImage I = MakeImage(8, 4);
	for (int32 Y = 0; Y < 2; ++Y) for (int32 X = 5; X < 8; ++X) Set(I, X, Y, 4, 4);  // touches right edge
	FEntityAnnotationData Unknown = Tagged(4, 1); Unknown.Truncation = -1.0;           // corners behind camera
	FEntityAnnotationData Known = Tagged(4, 2);   Known.Truncation = 0.0;              // 3D box says inside
	TArray<FEntityAnnotationData> E = { Unknown };
	FInstanceMaskAnalyzer::Analyze(I, E, 1, true);
	TestTrue(TEXT("unknown truncation: edge touch -> truncated"), E[0].bTruncated);
	TArray<FEntityAnnotationData> E2 = { Known };
	FInstanceMaskAnalyzer::Analyze(I, E2, 1, true);
	TestFalse(TEXT("known truncation wins"), E2[0].bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnalyzerSizeMismatchTest, "CamSim.GroundTruth.Analyzer.SizeMismatchFallsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnalyzerSizeMismatchTest::RunTest(const FString&)
{
	FInstanceIdImage Bad; Bad.Width = 8; Bad.Height = 4; Bad.Words.Init(0u, 3);  // too short
	TArray<FEntityAnnotationData> E = { Tagged(1, 1) };
	FInstanceMaskAnalyzer::Analyze(Bad, E, 1, true);
	TestEqual(TEXT("kept"), E.Num(), 1);
	TestFalse(TEXT("not measured"), E[0].bMaskMeasured);
	FInstanceIdImage Empty;
	FInstanceMaskAnalyzer::Analyze(Empty, E, 1, true);
	TestEqual(TEXT("still kept"), E.Num(), 1);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — `scripts/run.sh --build-only` → compile error (`InstanceMaskAnalyzer.h` missing).

- [ ] **Step 3: Implement.** Add the fields and `FInstanceIdImage` to `AnnotationTypes.h` exactly as in Interfaces. `InstanceMaskAnalyzer.h` as in Interfaces (`#include "CoreMinimal.h"`, `#include "GroundTruth/AnnotationTypes.h"`, doc comment naming the spec). `InstanceMaskAnalyzer.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/InstanceMaskAnalyzer.h"
#include "GroundTruth/MaskGeometry.h"

namespace
{
	/** Per stencil value: counts, boxes, and each row's extreme x (the hull of a pixel set is the hull of its row extremes). */
	struct FAcc
	{
		int32 Count = 0;
		int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = -1, MaxY = -1;
		TArray<int32> RowMin, RowMax;
		void Init(int32 H) { RowMin.Init(MAX_int32, H); RowMax.Init(-1, H); }
		void Add(int32 X, int32 Y)
		{
			++Count;
			MinX = FMath::Min(MinX, X); MaxX = FMath::Max(MaxX, X);
			MinY = FMath::Min(MinY, Y); MaxY = FMath::Max(MaxY, Y);
			RowMin[Y] = FMath::Min(RowMin[Y], X); RowMax[Y] = FMath::Max(RowMax[Y], X);
		}
		FBox2D Box() const { return FBox2D(FVector2D(MinX, MinY), FVector2D(MaxX + 1, MaxY + 1)); }
		CamSimMask::FOrientedBox Obb() const
		{
			TArray<FVector2D> Pts;
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				if (RowMax[Y] < 0) continue;
				for (const int32 X : { RowMin[Y], RowMax[Y] })
				{
					Pts.Add(FVector2D(X, Y)); Pts.Add(FVector2D(X + 1, Y)); Pts.Add(FVector2D(X, Y + 1)); Pts.Add(FVector2D(X + 1, Y + 1));
				}
			}
			return CamSimMask::MinAreaRect(CamSimMask::ConvexHull(MoveTemp(Pts)));
		}
	};

	/** Column-major runs of (visible == V) over the image, visiting only the modal box's columns/rows. */
	FString EncodeModal(const FInstanceIdImage& Ids, uint8 V, const FAcc& M)
	{
		TArray<uint32> Runs;
		bool bCur = false;
		auto Append = [&](bool bValue, uint32 Len)
		{
			if (Len == 0) return;
			if (Runs.Num() == 0) { if (bValue) Runs.Add(0); Runs.Add(Len); bCur = bValue; return; }
			if (bValue == bCur) { Runs.Last() += Len; } else { Runs.Add(Len); bCur = bValue; }
		};
		const uint32 H = static_cast<uint32>(Ids.Height);
		Append(false, static_cast<uint32>(M.MinX) * H);
		for (int32 X = M.MinX; X <= M.MaxX; ++X)
		{
			Append(false, static_cast<uint32>(M.MinY));
			for (int32 Y = M.MinY; Y <= M.MaxY; ++Y) Append((Ids.At(X, Y) & 0xFF) == V, 1);
			Append(false, H - 1 - static_cast<uint32>(M.MaxY));
		}
		Append(false, static_cast<uint32>(Ids.Width - 1 - M.MaxX) * H);
		return CamSimMask::EncodeCocoRle(Runs);
	}
}

void FInstanceMaskAnalyzer::Analyze(const FInstanceIdImage& Ids, TArray<FEntityAnnotationData>& Entities,
	int32 MinVisiblePixels, bool bSegmentation)
{
	if (!Ids.IsValid()) return;

	// Only stencil values some entity in this frame's snapshot carries get accumulators.
	TArray<int32> EntityOf; EntityOf.Init(INDEX_NONE, 256);
	for (int32 I = 0; I < Entities.Num(); ++I)
	{
		if (Entities[I].StencilValue != 0) EntityOf[Entities[I].StencilValue] = I;
	}
	TMap<uint8, FAcc> Modal, Amodal;
	for (int32 V = 1; V < 256; ++V)
	{
		if (EntityOf[V] == INDEX_NONE) continue;
		Modal.Add(V).Init(Ids.Height);
		Amodal.Add(V).Init(Ids.Height);
	}
	if (Modal.Num() == 0) return;

	for (int32 Y = 0; Y < Ids.Height; ++Y)
	{
		for (int32 X = 0; X < Ids.Width; ++X)
		{
			const uint16 P = Ids.At(X, Y);
			if (P == 0) continue;
			if (FAcc* A = Amodal.Find(static_cast<uint8>(P >> 8))) A->Add(X, Y);
			if (FAcc* M = Modal.Find(static_cast<uint8>(P & 0xFF))) M->Add(X, Y);
		}
	}

	TArray<FEntityAnnotationData> Kept;
	Kept.Reserve(Entities.Num());
	for (FEntityAnnotationData& E : Entities)
	{
		const FAcc* M = E.StencilValue ? Modal.Find(E.StencilValue) : nullptr;
		if (!M) { Kept.Add(MoveTemp(E)); continue; }   // untagged: projection fallback
		const FAcc& A = Amodal[E.StencilValue];
		if (M->Count < FMath::Max(1, MinVisiblePixels)) continue;   // fully (or nearly) hidden: not labelled
		E.bMaskMeasured = true;
		E.VisiblePixels = M->Count;
		E.AmodalPixels  = A.Count;
		E.ScreenBBox    = M->Box();
		E.AmodalBBox    = A.Box();
		E.Obb           = M->Obb();
		E.ObbAmodal     = A.Obb();
		if (bSegmentation) E.SegmentationRle = EncodeModal(Ids, E.StencilValue, *M);
		if (E.Truncation < 0.0)
		{
			E.bTruncated = A.MinX == 0 || A.MinY == 0 || A.MaxX == Ids.Width - 1 || A.MaxY == Ids.Height - 1;
		}
		else
		{
			E.bTruncated = E.Truncation > 0.01;
		}
		Kept.Add(MoveTemp(E));
	}
	Entities = MoveTemp(Kept);
}
```

- [ ] **Step 4: Run tests** — `scripts/run.sh --build-only && run_tests CamSim.GroundTruth` → Expected: all `CamSim.GroundTruth.*` pass (existing + 6 new analyzer + 5 mask).
- [ ] **Step 5: Commit** — `git commit -m "feat(gt): instance-mask analyzer — modal/amodal boxes, OBBs, visibility, RLE"` (stage the four files).

---

### Task 3: Projected oriented 3D box and truncation

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/FEntityProjection.h/.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/AnnotationTypes.h` (`FViewProjectionData` optics)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/GroundTruthTest.cpp` (append)

**Interfaces:**
- Consumes: `CamSimMask::ConvexHull`, `ClipToRect`, `PolygonArea` (Task 1); `CamSimOptics::FocalPx`, `CamSimOptics::UndistortRadius` (`Sensor/SensorOptics.h`).
- Produces:
  ```cpp
  // FViewProjectionData gains:
  float FocalPx = 0.0f;   // output pixels; 0 = pinhole (no distortion)
  float K1 = 0.0f, K2 = 0.0f;

  struct FProjectedBox3D { bool bValid = false; FVector2D Corners[8]; double Truncation = -1.0; };

  // FEntityProjection gains:
  /** Pinhole pixel (continuous, pixel i spans [i, i+1)) -> distorted output pixel: rd = ru (1 + K1 ru^2 + K2 ru^4)
   *  about the image centre, normalised by FocalPx. FocalPx <= 0 returns the input. */
  static FVector2D DistortPixel(const FVector2D& Pinhole, int32 W, int32 H, float FocalPx, float K1, float K2);
  /** LocalBox (actor space, cm) through ActorToWorld and ViewProj, then DistortPixel. bValid false (Truncation -1) when
   *  any corner has clip w <= 0. Truncation = 1 - area(hull ∩ [0,W]x[0,H]) / area(hull), clamped to [0, 1]. */
  static FProjectedBox3D ProjectOrientedBox(const FBox& LocalBox, const FTransform& ActorToWorld,
      const FMatrix& ViewProjectionMatrix, int32 W, int32 H, float FocalPx, float K1, float K2);
  ```

- [ ] **Step 1: Write failing tests** (append to `Tests/GroundTruthTest.cpp`; add `#include "Sensor/SensorOptics.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DCentredTest, "CamSim.GroundTruth.Box3D.Centred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DCentredTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	const FBox Local(FVector(-300, -100, 0), FVector(300, 100, 250));   // 6 x 2 x 2.5 m
	const FTransform At(FRotator::ZeroRotator, FVector(10000, 0, -125));  // 100 m ahead, centred
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, At, VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestTrue(TEXT("valid"), P.bValid);
	TestNearlyEqual(TEXT("no truncation"), P.Truncation, 0.0, 1e-9);
	// corner 0 = bottom rear-left (X min, Y min, Z min) is left of and below the centre
	TestTrue(TEXT("rear-left is left"), P.Corners[0].X < 960.0);
	TestTrue(TEXT("bottom is below"), P.Corners[0].Y > 540.0);
	TestTrue(TEXT("top above bottom"), P.Corners[4].Y < P.Corners[0].Y);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DHalfOffTest, "CamSim.GroundTruth.Box3D.HalfOffEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DHalfOffTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	// Flat face-on box (thin in X) centred on the right image edge: x_ndc = 1 at y = tan(30°) * depth
	const double Depth = 10000.0, EdgeY = FMath::Tan(FMath::DegreesToRadians(30.0)) * Depth;
	const FBox Local(FVector(-1, -200, -200), FVector(1, 200, 200));
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, FTransform(FVector(Depth, EdgeY, 0)), VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestTrue(TEXT("valid"), P.bValid);
	TestNearlyEqual(TEXT("half truncated"), P.Truncation, 0.5, 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBox3DBehindTest, "CamSim.GroundTruth.Box3D.BehindCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthBox3DBehindTest::RunTest(const FString&)
{
	const FMatrix VP = FEntityProjection::BuildViewProjectionMatrix(FVector::ZeroVector, FRotator::ZeroRotator, 60.0f, 1920, 1080);
	const FBox Local(FVector(-500, -100, -100), FVector(500, 100, 100));   // straddles the camera plane
	const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(Local, FTransform(FVector(200, 0, 0)), VP, 1920, 1080, 0.0f, 0.0f, 0.0f);
	TestFalse(TEXT("invalid"), P.bValid);
	TestTrue(TEXT("truncation unknown"), P.Truncation < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthDistortPixelTest, "CamSim.GroundTruth.Box3D.DistortPixel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthDistortPixelTest::RunTest(const FString&)
{
	const int32 W = 1920, H = 1080;
	const float F = CamSimOptics::FocalPx(W, 60.0f), K1 = -0.15f, K2 = 0.02f;
	const FVector2D Pin(1700.0, 900.0);
	const FVector2D D = FEntityProjection::DistortPixel(Pin, W, H, F, K1, K2);
	TestTrue(TEXT("barrel (k1<0) pulls inward"), FVector2D::Distance(D, FVector2D(960, 540)) < FVector2D::Distance(Pin, FVector2D(960, 540)));
	// Round trip with the sensor's inverse
	const double Xd = (D.X - 0.5 * W) / F, Yd = (D.Y - 0.5 * H) / F, Rd = FMath::Sqrt(Xd * Xd + Yd * Yd);
	float Ru = 0.0f;
	TestTrue(TEXT("converges"), CamSimOptics::UndistortRadius(static_cast<float>(Rd), K1, K2, Ru));
	const FVector2D Back(0.5 * W + Xd * (Ru / Rd) * F, 0.5 * H + Yd * (Ru / Rd) * F);
	TestNearlyEqual(TEXT("round trip x"), Back.X, Pin.X, 0.05);
	TestNearlyEqual(TEXT("round trip y"), Back.Y, Pin.Y, 0.05);
	TestEqual(TEXT("no focal -> identity"), FEntityProjection::DistortPixel(Pin, W, H, 0.0f, K1, K2), Pin);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — build fails (`ProjectOrientedBox` undeclared).

- [ ] **Step 3: Implement** in `FEntityProjection.cpp` (add `#include "GroundTruth/MaskGeometry.h"`):

```cpp
FVector2D FEntityProjection::DistortPixel(const FVector2D& Pinhole, int32 W, int32 H, float FocalPx, float K1, float K2)
{
	if (!(FocalPx > 0.0f)) return Pinhole;
	const double Cx = 0.5 * W, Cy = 0.5 * H;
	const double Xu = (Pinhole.X - Cx) / FocalPx, Yu = (Pinhole.Y - Cy) / FocalPx;
	const double R2 = Xu * Xu + Yu * Yu;
	const double S = 1.0 + K1 * R2 + K2 * R2 * R2;
	return FVector2D(Cx + Xu * S * FocalPx, Cy + Yu * S * FocalPx);
}

FProjectedBox3D FEntityProjection::ProjectOrientedBox(const FBox& L, const FTransform& ActorToWorld,
	const FMatrix& VP, int32 W, int32 H, float FocalPx, float K1, float K2)
{
	// bottom face then top; each rear-left, rear-right, front-right, front-left (body X fwd, Y right)
	const FVector Local[8] = {
		{L.Min.X, L.Min.Y, L.Min.Z}, {L.Min.X, L.Max.Y, L.Min.Z}, {L.Max.X, L.Max.Y, L.Min.Z}, {L.Max.X, L.Min.Y, L.Min.Z},
		{L.Min.X, L.Min.Y, L.Max.Z}, {L.Min.X, L.Max.Y, L.Max.Z}, {L.Max.X, L.Max.Y, L.Max.Z}, {L.Max.X, L.Min.Y, L.Max.Z} };
	FProjectedBox3D Out;
	for (int32 I = 0; I < 8; ++I)
	{
		const FVector4 Clip = VP.TransformFVector4(FVector4(ActorToWorld.TransformPosition(Local[I]), 1.0));
		if (Clip.W <= 0.0) return FProjectedBox3D();   // behind the camera: invalid, truncation unknown
		const FVector2D Pin((Clip.X / Clip.W + 1.0) * 0.5 * W, (1.0 - Clip.Y / Clip.W) * 0.5 * H);
		Out.Corners[I] = DistortPixel(Pin, W, H, FocalPx, K1, K2);
	}
	Out.bValid = true;
	const TArray<FVector2D> Hull = CamSimMask::ConvexHull(TArray<FVector2D>(Out.Corners, 8));
	const double Area = CamSimMask::PolygonArea(Hull);
	const double Inside = CamSimMask::PolygonArea(CamSimMask::ClipToRect(Hull, FBox2D(FVector2D(0, 0), FVector2D(W, H))));
	Out.Truncation = Area > 0.0 ? FMath::Clamp(1.0 - Inside / Area, 0.0, 1.0) : 0.0;
	return Out;
}
```

Add `FocalPx/K1/K2` to `FViewProjectionData` and `FProjectedBox3D` to `AnnotationTypes.h`; declare both statics in `FEntityProjection.h`.

- [ ] **Step 4: Run tests** — `scripts/run.sh --build-only && run_tests CamSim.GroundTruth` → all pass.
- [ ] **Step 5: Commit** — `git commit -m "feat(gt): oriented 3D box projection with lens distortion and truncation"`.

---

### Task 4: Stencil tagging and the snapshot

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Entity/StencilSlotAllocator.h`
- Modify: `Entity/CamSimEntity.h/.cpp`, `Entity/CamSimEntityManager.h/.cpp`, `Camera/CamSimCaptureComponent.cpp` (`BuildGroundTruthSnapshot`), `unreal_project/CamSimTest/Config/DefaultEngine.ini`
- Test: append to `Tests/GroundTruthTest.cpp`

**Interfaces:**
- Consumes: `FEntityProjection::ProjectOrientedBox`, `FViewProjectionData::{FocalPx,K1,K2}` (Task 3); annotation fields (Task 2).
- Produces:
  ```cpp
  class FStencilSlotAllocator
  {
  public:
      static constexpr int32  MaxValue = 255;
      static constexpr uint64 ReuseDelayFrames = 4;   // > FReadbackRing::NumSlots: frames in flight never see a reused value
      uint8 Allocate(uint64 Frame);                   // lowest free value whose release is >= ReuseDelayFrames old; 0 = none
      void  Release(uint8 Value, uint64 Frame);       // 0 ignored
  };
  // ACamSimEntity:
  void  SetGroundTruthStencil(uint8 Value);   // stores it and applies it to every UMeshComponent (0 = custom depth off)
  uint8 GetGroundTruthStencil() const;
  ```

- [ ] **Step 1: Failing tests** (append to `GroundTruthTest.cpp`; `#include "Entity/StencilSlotAllocator.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthStencilAllocTest, "CamSim.GroundTruth.StencilAllocator.Order",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthStencilAllocTest::RunTest(const FString&)
{
	FStencilSlotAllocator A;
	TestEqual(TEXT("first"), A.Allocate(0), uint8(1));
	TestEqual(TEXT("second"), A.Allocate(0), uint8(2));
	for (int32 I = 3; I <= 255; ++I) A.Allocate(0);
	TestEqual(TEXT("exhausted"), A.Allocate(0), uint8(0));
	A.Release(0, 0);  // ignored
	TestEqual(TEXT("still exhausted"), A.Allocate(1), uint8(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthStencilReuseTest, "CamSim.GroundTruth.StencilAllocator.DelayedReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGroundTruthStencilReuseTest::RunTest(const FString&)
{
	FStencilSlotAllocator A;
	const uint8 V1 = A.Allocate(10), V2 = A.Allocate(10);
	A.Release(V1, 100);
	TestEqual(TEXT("released value not reused within the delay"), A.Allocate(101), uint8(3));
	TestEqual(TEXT("reused once the delay has passed"), A.Allocate(100 + FStencilSlotAllocator::ReuseDelayFrames), V1);
	TestTrue(TEXT("delay covers the ring"), FStencilSlotAllocator::ReuseDelayFrames > 3);
	(void)V2;
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — build fails.

- [ ] **Step 3: Implement.**

`Entity/StencilSlotAllocator.h`:
```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FStencilSlotAllocator (ROADMAP 2.7)
 *
 * Custom-depth stencil values 1..255 for ground-truth entities, lowest free first. A released
 * value is held for ReuseDelayFrames so frames still in the readback ring (which map stencil
 * values to the entities in their own snapshot) never see it on a different entity. Game thread.
 */
class FStencilSlotAllocator
{
public:
	static constexpr int32  MaxValue = 255;
	static constexpr uint64 ReuseDelayFrames = 4;

	uint8 Allocate(uint64 Frame)
	{
		for (int32 V = 1; V <= MaxValue; ++V)
		{
			if (!bInUse[V] && Frame >= FreeFrom[V])
			{
				bInUse[V] = true;
				return static_cast<uint8>(V);
			}
		}
		return 0;
	}

	void Release(uint8 Value, uint64 Frame)
	{
		if (Value == 0 || !bInUse[Value]) return;
		bInUse[Value] = false;
		FreeFrom[Value] = Frame + ReuseDelayFrames;
	}

private:
	bool   bInUse[MaxValue + 1] = {};
	uint64 FreeFrom[MaxValue + 1] = {};
};
```

`ACamSimEntity` (header: `uint8 GroundTruthStencil = 0;` private, the two public methods):
```cpp
void ACamSimEntity::SetGroundTruthStencil(uint8 Value)
{
	GroundTruthStencil = Value;
	// Meshes only: particle systems (smoke, wakes) must not join the vehicle's mask.
	TInlineComponentArray<UMeshComponent*> Meshes(this);
	for (UMeshComponent* M : Meshes)
	{
		M->SetRenderCustomDepth(Value != 0);
		M->SetCustomDepthStencilValue(Value);
	}
}
```
Call `SetGroundTruthStencil(GroundTruthStencil)` at the end of `ApplyLoadedStaticMesh`, `ApplyLoadedSkeletalMesh` and `InitAnimatedCharacter` (mesh components can be created or swapped there) — a no-op re-apply when the value is 0.

`FCamSimEntityManager`: member `FStencilSlotAllocator StencilSlots;` and `bool bTagGroundTruth = false;` set in the constructor/init from `Cfg.MLTraining.bEnabled && Cfg.MLTraining.bBoundingBoxes` (find where `TypeTable`/config is first available; if config is only reachable via `Subsystem->GetConfig()`, read it in `SpawnEntity`). In `SpawnEntity` after `AnnotationId`:
```cpp
	if (Subsystem && Subsystem->GetConfig().MLTraining.bEnabled && Subsystem->GetConfig().MLTraining.bBoundingBoxes)
	{
		const uint8 Stencil = StencilSlots.Allocate(GFrameCounter);
		if (Stencil == 0 && !bLoggedStencilExhausted)
		{
			bLoggedStencilExhausted = true;
			UE_LOG(LogCamSim, Warning, TEXT("EntityManager: more than 255 ground-truth entities; extra entities get projected boxes (logged once)"));
		}
		Entity->SetGroundTruthStencil(Stencil);
	}
```
Release in every place an entity leaves `EntityMap` with its actor: the `Remove` branch of `ApplyEntityCommand` (before `Destroy`), `PurgeStaleEntities` (the actor is gone: keep a `TMap<FEntityKey, uint8> StencilOf` alongside, filled at spawn, so the value can be released without the actor), and the destructor/shutdown loop. Simplest: always release through `ForgetEntity(Key)` using `StencilOf` — `ForgetEntity` is called on every removal path; check `PurgeStaleEntities` and the shutdown loop call it, and add the release there if not.

Snapshot (`GetEntitySnapshot`, after the `ScreenBBox` assignment):
```cpp
		Data.StencilValue = Entity->GetGroundTruthStencil();
		const FBox LocalBox = Entity->CalculateComponentsBoundingBoxInLocalSpace(/*bNonColliding=*/true);
		if (LocalBox.IsValid)
		{
			const FProjectedBox3D P = FEntityProjection::ProjectOrientedBox(LocalBox, Entity->GetActorTransform(),
				ViewProj.ViewProjectionMatrix, ViewProj.ImageWidth, ViewProj.ImageHeight, ViewProj.FocalPx, ViewProj.K1, ViewProj.K2);
			Data.bHasBox3D     = true;
			Data.Box3DSizeM    = LocalBox.GetSize() * Entity->GetActorScale3D() / 100.0;
			Data.bCornersValid = P.bValid;
			FMemory::Memcpy(Data.CornersPx, P.Corners, sizeof(P.Corners));
			Data.Truncation    = P.Truncation;
			if (P.bValid) Data.bTruncated = P.Truncation > 0.01;
		}
```
and inside the existing `GetGeoPose` block: `const FRotator R = Geo.Neu.Rotator(); Data.YawDeg = R.Yaw; Data.PitchDeg = R.Pitch; Data.RollDeg = R.Roll;` (`CigiToNeu` builds `FRotator(Pitch, Heading, Roll)`). `CalculateComponentsBoundingBoxInLocalSpace(bNonColliding = true)` includes non-colliding components (the entity meshes have no collision) — verify the UE 5.8 signature in `Actor.h` and pass the argument that includes non-colliding components.

`UCamSimCaptureComponent::BuildGroundTruthSnapshot`: after the matrix, `if (OpticsCache.bValid) { ViewProj.FocalPx = OpticsCache.Params.FocalPx; ViewProj.K1 = OpticsCache.Params.K1; ViewProj.K2 = OpticsCache.Params.K2; }` — the same params this frame's sensor graph uses.

`Config/DefaultEngine.ini`, in the `[/Script/Engine.RendererSettings]` section next to `r.ScreenPercentage=100`: `r.CustomDepth=3`.

- [ ] **Step 4: Run tests** — `scripts/run.sh --build-only && run_tests CamSim` → Expected: whole suite passes (the snapshot is exercised by `CamSim.GroundTruth.PerFrameSnapshot`).
- [ ] **Step 5: Commit** — `git commit -m "feat(gt): per-entity custom-depth stencil and oriented 3D box in the snapshot"`.

---

### Task 5: Instance-ID compute pass

**Files:**
- Modify: `unreal_project/CamSimTest/Shaders/Private/CamSimSensorCommon.ush`, `CamSimSensor.usf`
- Create: `unreal_project/CamSimTest/Shaders/Private/CamSimInstanceId.usf`
- Create: `unreal_project/CamSimTest/Source/CamSimShaders/Public/InstanceIdPass.h`, `Private/InstanceIdPass.cpp`
- Test: create `unreal_project/CamSimTest/Source/CamSimTest/Tests/InstanceIdGpuTest.cpp`

**Interfaces:**
- Consumes: `FSensorFrameParams::{FocalPx, K1, K2, NewtonIterations}`.
- Produces:
  ```cpp
  struct FInstanceIdInputs
  {
      FRDGTextureRef    SceneDepth  = nullptr;   // device Z (reversed: 1 near, 0 far)
      FRDGTextureRef    CustomDepth = nullptr;   // device Z of the tagged entities only
      FRDGTextureSRVRef CustomStencil = nullptr; // uint; value replicated or read via STENCIL_COMPONENT_SWIZZLE
      FIntRect DepthViewRect;                    // region of the depth textures the view covers (ignored when ViewUniformBuffer is set)
      FRHIUniformBuffer* ViewUniformBuffer = nullptr;  // runtime: the rect comes from View.ViewRectMin / ViewSizeAndInvSize
      FIntPoint OutputSize = FIntPoint::ZeroValue;     // = sensor output, X even
  };
  /** One uint32 per two output pixels (pixel 2k low 16 bits): visible | amodal << 8. FocalPx/K1/K2 from Params. */
  CAMSIMSHADERS_API FRDGBufferRef AddInstanceIdPass(FRDGBuilder& GraphBuilder, const FInstanceIdInputs& In, const FSensorFrameParams& Params);
  ```

- [ ] **Step 1: Shared distortion inverse.** In `CamSimSensorCommon.ush` add (guard on `NEWTON_ITERATIONS`):

```hlsl
/** CamSimOptics::UndistortRadius: ideal / distorted radius for distorted radius Rd (Newton from r = Rd,
 *  NEWTON_ITERATIONS steps, final iterate). 1 at the centre. Shared by SensorCS and InstanceIdCS. */
float UndistortScale(float Rd, float K1, float K2)
{
	if (!(Rd > 0.0)) return 1.0;
	float R = Rd;
	[unroll] for (int It = 0; It < NEWTON_ITERATIONS; ++It)
	{
		const float R2 = R * R;
		R -= (R * (1.0 + K1 * R2 + K2 * R2 * R2) - Rd) / (1.0 + 3.0 * K1 * R2 + 5.0 * K2 * R2 * R2);
	}
	return R / Rd;
}
```
and in `CamSimSensor.usf` `OpticsAt` replace the `float S = 1.0; if (Rd > 0.0) { … }` block with `const float S = UndistortScale(Rd, K1, K2);`. Check `CamSimSensorCommon.ush` is included after `NEWTON_ITERATIONS` is defined (it comes from the compile environment, so yes).

- [ ] **Step 2: Run sensor GPU tests** — `scripts/run.sh --build-only && scripts/run_gpu_tests.sh CamSim.GPU.Sensor` → all pass (refactor is expression-identical).

- [ ] **Step 3: Write failing GPU tests** — `Tests/InstanceIdGpuTest.cpp`. Follow `Tests/SensorGpuTest.cpp` for upload/readback helpers (`ENQUEUE_RENDER_COMMAND` + `FRDGBuilder` + `FRHIGPUBufferReadback` + `FlushRenderingCommands`, and the NullRHI skip it uses). Textures: scene depth and custom depth `PF_R32_FLOAT`; stencil `PF_R8G8B8A8_UINT` with the value in all four channels (so the shader's swizzle reads it whichever channel it is), created with an SRV.

```cpp
// CamSim.GPU.GroundTruth.InstanceId.Synthetic — 8x4, optics off (FocalPx 0), rect = whole texture
//   stencil 5 at (1,1) and (2,1); 7 at (5,2); custom depth 0.5 there, 0 elsewhere
//   scene depth: 0.5 at (1,1) and (5,2) (entity is the front surface), 0.9 at (2,1) (an occluder in front), 0.1 elsewhere
//   expect pixel(1,1) = 5 | 5<<8, pixel(2,1) = 0 | 5<<8, pixel(5,2) = 7 | 7<<8, every other pixel 0, buffer = 16 words
// CamSim.GPU.GroundTruth.InstanceId.ViewRectOffset — textures 10x6, DepthViewRect (1,1)-(9,5), output 8x4;
//   stencil 3 at texel (1,1) -> output pixel (0,0) = 3 | 3<<8; stencil at texel (0,0) never appears
// CamSim.GPU.GroundTruth.InstanceId.DistortionMatchesSensor — 64x48 output and textures, FocalPx = CamSimOptics::FocalPx(64, 60),
//   K1 = -0.2, K2 = 0.05; stencil 9 at texel T = (52, 40), custom = scene = 0.5 there.
//   For every output pixel P compute on the CPU, with the expressions of CamSimSensorRef::Optics (Sensor/SensorReference.cpp:173-182):
//     Xd = (P.x + 0.5 - 32) / F, Yd = (P.y + 0.5 - 24) / F, S = UndistortRadius(Rd)/Rd (1 at Rd = 0),
//     Sx = (Xd*S*F + 32) * 1 - 0.5, Sy likewise; texel = (floor(Sx + 0.5), floor(Sy + 0.5)), out of [-0.5, Size-0.5] -> none
//   Expect: the set of output pixels with amodal 9 equals the set whose CPU texel is T, and it is non-empty.
```
Write these three as `IMPLEMENT_SIMPLE_AUTOMATION_TEST` with `EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter` and names `CamSim.GPU.GroundTruth.InstanceId.{Synthetic,ViewRectOffset,DistortionMatchesSensor}`, sharing one `RunInstanceIdOnGpu(SceneDepth, CustomDepth, Stencil, Extent, Rect, OutSize, Params) -> TArray<uint32>` helper.

- [ ] **Step 4: Run to verify failure** — build fails (`InstanceIdPass.h` missing).

- [ ] **Step 5: Implement the shader** — `Shaders/Private/CamSimInstanceId.usf`:

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// Ground truth (ROADMAP 2.7): InstanceIdCS — per output pixel, which tagged entity is visible and which entity's
// silhouette covers it, sampled at the same distorted source position as SensorCS. Integer ops only (Metal + Vulkan).

#include "/Engine/Public/Platform.ush"
#if USE_VIEW_RECT
#include "/Engine/Private/Common.ush"   // View.ViewRectMin, View.ViewSizeAndInvSize
#endif
#include "CamSimSensorCommon.ush"

int2   OutputSize;
int2   DepthViewMin;
int2   DepthViewSize;
float2 HalfOut;
float  FocalPx;
float  K1;
float  K2;
Texture2D<float> SceneDepth;
Texture2D<float> CustomDepth;
Texture2D<uint4> CustomStencil;
RWStructuredBuffer<uint> OutIds;

void ViewRect(out int2 Min, out int2 Size)
{
#if USE_VIEW_RECT
	Min = int2(View.ViewRectMin.xy);
	Size = int2(View.ViewSizeAndInvSize.xy);
#else
	Min = DepthViewMin;
	Size = DepthViewSize;
#endif
}

uint IdAt(int2 P)
{
	int2 Min, Size;
	ViewRect(Min, Size);
	const float2 SrcScale = float2(Size) / float2(OutputSize);
	float2 S;
	if (!(FocalPx > 0.0))
	{
		S = (float2(P) + 0.5) * SrcScale - 0.5;
	}
	else
	{
		const float Xd = (float(P.x) + 0.5 - HalfOut.x) / FocalPx, Yd = (float(P.y) + 0.5 - HalfOut.y) / FocalPx;
		const float Sc = UndistortScale(sqrt(Xd * Xd + Yd * Yd), K1, K2);
		S = float2((Xd * Sc * FocalPx + HalfOut.x) * SrcScale.x - 0.5, (Yd * Sc * FocalPx + HalfOut.y) * SrcScale.y - 0.5);
	}
	if (!(S.x >= -0.5 && S.x <= float(Size.x) - 0.5 && S.y >= -0.5 && S.y <= float(Size.y) - 0.5)) return 0;
	const int2 T = Min + clamp(int2(floor(S + 0.5)), int2(0, 0), Size - 1);
	const uint Amodal = CustomStencil.Load(int3(T, 0)).STENCIL_COMPONENT_SWIZZLE & 0xFF;
	if (Amodal == 0) return 0;
	const float SceneZ = SceneDepth.Load(int3(T, 0));
	const float CustomZ = CustomDepth.Load(int3(T, 0));
	// Reversed Z: larger is nearer. The entity is visible when nothing is nearer than its own surface.
	const uint Visible = (CustomZ >= SceneZ * (1.0 - 1e-4)) ? Amodal : 0;
	return Visible | (Amodal << 8);
}

[numthreads(8, 8, 1)]
void InstanceIdCS(uint3 Id : SV_DispatchThreadID)
{
	const int2 Pair = int2(Id.xy);   // one thread per two horizontal pixels
	if (Pair.x * 2 >= OutputSize.x || Pair.y >= OutputSize.y) return;
	const int2 P0 = int2(Pair.x * 2, Pair.y);
	OutIds[Pair.y * (OutputSize.x / 2) + Pair.x] = IdAt(P0) | (IdAt(P0 + int2(1, 0)) << 16);
}
```
If `STENCIL_COMPONENT_SWIZZLE` is not defined by `Platform.ush` in UE 5.8, include the engine header that defines it (search `Engine/Shaders` for `#define STENCIL_COMPONENT_SWIZZLE`); with the test's replicated texture any channel works.

- [ ] **Step 6: Implement the pass** — `InstanceIdPass.cpp` mirrors `SensorGraph.cpp`: `BEGIN_SHADER_PARAMETER_STRUCT(FCamSimInstanceIdParameters, )` with `SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)`, the scalars above, `SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)`, `SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepth)`, `SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint4>, CustomStencil)`, `SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutIds)`; `class FCamSimInstanceIdCS : public FGlobalShader` with permutation `class FUseViewRect : SHADER_PERMUTATION_BOOL("USE_VIEW_RECT")`, `ShouldCompilePermutation` as `FCamSimSensorCS`, `ModifyCompilationEnvironment` setting `NEWTON_ITERATIONS` to `FSensorFrameParams::NewtonIterations`; `IMPLEMENT_GLOBAL_SHADER(FCamSimInstanceIdCS, "/CamSim/Private/CamSimInstanceId.usf", "InstanceIdCS", SF_Compute)`. `AddInstanceIdPass`: `check(In.OutputSize.X % 2 == 0)`; buffer `CreateStructuredDesc(sizeof(uint32), Out.X * Out.Y / 2)` named `CamSimInstanceIds`; `HalfOut = 0.5 * Out`; `DepthViewMin/Size` from `In.DepthViewRect`; group count `GetGroupCount(FIntPoint(Out.X / 2, Out.Y), FIntPoint(8, 8))`; event name `RDG_EVENT_NAME("CamSimInstanceIds %dx%d", ...)`. Add the shader to `IsSensorGraphSupported`'s shader-map check so a missing permutation disables the graph cleanly rather than crashing.

- [ ] **Step 7: Run GPU tests** — `scripts/run.sh --build-only && scripts/run_gpu_tests.sh CamSim.GPU` → all `CamSim.GPU.*` pass, including the 3 new ones.
- [ ] **Step 8: Commit** — `git commit -m "feat(gt): InstanceIdCS — output-space visible/amodal stencil IDs with the sensor's distortion resample"`.

---

### Task 6: Config, collector and writers

**Files:**
- Modify: `Source/CamSimTest/Config/CamSimConfig.h/.cpp`, `deploy/camsim_config.yaml`, `docs/configuration.md`
- Modify: `Source/CamSimTest/GroundTruth/FGroundTruthCollector.h/.cpp`, `FCocoAnnotationWriter.cpp`, `FVocAnnotationWriter.cpp`
- Test: append to `Tests/GroundTruthTest.cpp`; add the keys to whatever known-key list `Tests/ConfigUnknownKeysTest.cpp` / the loader's unknown-key check uses (grep `"depth_far_plane_m"` across `Source/` and add the two keys next to every hit)

**Interfaces:**
- Consumes: `FInstanceIdImage`, `FInstanceMaskAnalyzer::Analyze` (Task 2); annotation fields (Tasks 2–4).
- Produces:
  ```cpp
  // FMLTrainingConfig:
  int32 MinVisiblePixels = 1;   // ml_training.min_visible_pixels, CAMSIM_ML_MIN_VISIBLE_PIXELS (clamped >= 1)
  bool  bSegmentation    = true; // ml_training.segmentation,       CAMSIM_ML_SEGMENTATION_ENABLED
  // FGroundTruthCollector:
  void WriteAnnotationFrame(TArray<FEntityAnnotationData> Entities, const FInstanceIdImage* Ids,
                            const FCamSimTelemetry& Telemetry, uint64 FrameIdx);   // Ids null/invalid: projection fallback
  void WriteAnnotationFrame(const TArray<FEntityAnnotationData>& Entities, const FCamSimTelemetry& Telemetry, uint64 FrameIdx)
  { WriteAnnotationFrame(Entities, nullptr, Telemetry, FrameIdx); }   // existing callers/tests
  ```

- [ ] **Step 1: Failing tests** (append to `GroundTruthTest.cpp`). Use the existing collector tests in that file as the template for opening a collector on a temp `OutputDir` and reading `camsim_coco.jsonl` back; parse each line with `FJsonSerializer` (`#include "Serialization/JsonSerializer.h"`, `"Dom/JsonObject.h"`).

```cpp
// CamSim.GroundTruth.Collector.MeasuredFields — collector with coco on, a 6x4 FInstanceIdImage with stencil 7
//   at rows 1..2 cols 1..3 (visible = amodal), one entity StencilValue 7, bHasBox3D with 8 corners and Truncation 0.
//   Parse the line: annotations[0] has mask_source "render", bbox [1,1,3,2], area 6, visibility 1.0, truncation 0,
//   bbox_amodal [1,1,3,2], obb with 5 numbers (w 3, h 2, angle 0), segmentation.size [4,6] and
//   segmentation.counts == CamSimMask::EncodeCocoRle({5,2,2,2,2,2,9}), box3d.corners_px with 8 pairs.
// CamSim.GroundTruth.Collector.FallbackWithoutIds — same entity, Ids = nullptr: mask_source "projection",
//   bbox from ScreenBBox, no visibility / segmentation / obb fields; and with an FInstanceIdImage of the wrong size, the same.
// CamSim.GroundTruth.Coco.RleEscaped — 40x40 image, one entity whose visible mask is the column-major run 17..19
//   (pixels (0,17),(0,18),(0,19)); the line parses as JSON and segmentation.counts == TEXT("a03\\a1").
// CamSim.GroundTruth.Collector.HiddenDropped — stencil present only in the amodal channel: the frame line has "annotations":[]
// CamSim.GroundTruth.Config.NewKeys — YAML ml_training: {min_visible_pixels: 25, segmentation: false} loads;
//   CAMSIM_ML_MIN_VISIBLE_PIXELS=0 clamps to 1 (follow CamSimConfigTest.cpp's YAML/env helpers).
```
Write them in full as `IMPLEMENT_SIMPLE_AUTOMATION_TEST`s with the assertions listed. The collector only analyses an ID image whose size equals `Config.CaptureWidth × CaptureHeight` (Step 4), so each test sets the config's capture size to its image size (6×4, 40×40) before constructing the collector.

- [ ] **Step 2: Run to verify failure** — build fails (new overload / fields missing).

- [ ] **Step 3: Implement config** — fields in `FMLTrainingConfig`; YAML in the `ml_training` block of `CamSimConfig.cpp` (`YamlInt(MLNode, "min_visible_pixels", …)` then `FMath::Max(1, …)`, `YamlBool(MLNode, "segmentation", Cfg.MLTraining.bSegmentation)`); env next to line ~1225 (`GetEnvInt(TEXT("CAMSIM_ML_MIN_VISIBLE_PIXELS"), …)` clamped ≥ 1, `CAMSIM_ML_SEGMENTATION_ENABLED`). `deploy/camsim_config.yaml` `ml_training:` gains, after `voc_export`:
```yaml
  # Drop annotations with fewer visible (unoccluded, in-frame) pixels than this.
  # Fully hidden vehicles are never labelled. Env: CAMSIM_ML_MIN_VISIBLE_PIXELS
  min_visible_pixels: 1

  # COCO RLE "segmentation" (modal mask) per annotation; the bulk of each line.
  # Env: CAMSIM_ML_SEGMENTATION_ENABLED
  segmentation: true
```
and `docs/configuration.md` gets both rows in the `ml_training` table (same columns as its neighbours).

- [ ] **Step 4: Implement collector** — `WriteAnnotationFrame(TArray<…> Entities, const FInstanceIdImage* Ids, …)`: same early-outs and interval check as now; then `if (Ids && Ids->IsValid() && Ids->Width == Config.CaptureWidth && Ids->Height == Config.CaptureHeight) FInstanceMaskAnalyzer::Analyze(*Ids, Entities, Config.MLTraining.MinVisiblePixels, Config.MLTraining.bSegmentation);` (the size check guards a hot-reloaded capture size); then the existing `bVisible` filter and writers. `VocAnnotationWriter::SetImageSize` and the COCO writer need the image size: add `SetImageSize(W, H)` to the COCO writer too (called in `Open` like VOC) for `segmentation.size`.

- [ ] **Step 5: Implement COCO writer** — add a local `JsonEscape(const FString&)` (`\` → `\\`, `"` → `\"`) and use it for every string (class, source, source id, RLE). Per annotation, after the existing fields:
```cpp
		if (E.bMaskMeasured)
		{
			// bbox/area above already come from the mask (ScreenBBox = modal box; area = VisiblePixels, not W*H)
			Line += FString::Printf(TEXT(",\"mask_source\":\"render\",\"visibility\":%.4f"),
				E.AmodalPixels > 0 ? double(E.VisiblePixels) / E.AmodalPixels : 0.0);
			Line += FString::Printf(TEXT(",\"bbox_amodal\":[%.1f,%.1f,%.1f,%.1f]"), E.AmodalBBox.Min.X, E.AmodalBBox.Min.Y,
				E.AmodalBBox.Max.X - E.AmodalBBox.Min.X, E.AmodalBBox.Max.Y - E.AmodalBBox.Min.Y);
			auto Obb = [](const CamSimMask::FOrientedBox& B) { return FString::Printf(TEXT("[%.2f,%.2f,%.2f,%.2f,%.2f]"), B.Cx, B.Cy, B.W, B.H, B.AngleDeg); };
			Line += TEXT(",\"obb\":") + Obb(E.Obb) + TEXT(",\"obb_amodal\":") + Obb(E.ObbAmodal);
			if (!E.SegmentationRle.IsEmpty())
				Line += FString::Printf(TEXT(",\"segmentation\":{\"size\":[%d,%d],\"counts\":\"%s\"}"), ImageHeight, ImageWidth, *JsonEscape(E.SegmentationRle));
		}
		else
		{
			Line += TEXT(",\"mask_source\":\"projection\"");
		}
		if (E.Truncation >= 0.0) Line += FString::Printf(TEXT(",\"truncation\":%.4f"), E.Truncation);
		if (E.bHasBox3D)
		{
			Line += FString::Printf(TEXT(",\"box3d\":{\"size_m\":[%.3f,%.3f,%.3f],\"yaw_deg\":%.3f,\"pitch_deg\":%.3f,\"roll_deg\":%.3f,\"corners_px\":"),
				E.Box3DSizeM.X, E.Box3DSizeM.Y, E.Box3DSizeM.Z, E.YawDeg, E.PitchDeg, E.RollDeg);
			if (E.bCornersValid)
			{
				Line += TEXT("[");
				for (int32 C = 0; C < 8; ++C) Line += FString::Printf(TEXT("%s[%.1f,%.1f]"), C ? TEXT(",") : TEXT(""), E.CornersPx[C].X, E.CornersPx[C].Y);
				Line += TEXT("]}");
			}
			else
			{
				Line += TEXT("null}");
			}
		}
```
and change `area` to `E.bMaskMeasured ? double(E.VisiblePixels) : W * H`. The `geo` block stays last.

- [ ] **Step 6: Implement VOC** — after `<truncated>`: `if (E.bMaskMeasured) Xml += FString::Printf(TEXT("\t\t<occluded>%d</occluded>\n"), (E.AmodalPixels > 0 && double(E.VisiblePixels) / E.AmodalPixels < 0.95) ? 1 : 0);`. VOC `bndbox` now reads the modal pixel-edge box: `xmax = Max.X` stays (VOC is 1-based inclusive in the original; CamSim has always written 0-based and keeps doing so — note it in `docs/ground-truth.md`).

- [ ] **Step 7: Run tests** — `scripts/run.sh --build-only && run_tests CamSim` → whole suite passes.
- [ ] **Step 8: Commit** — `git commit -m "feat(gt): COCO/VOC write measured boxes, OBBs, visibility, truncation, box3d, RLE; new ml_training keys"`.

---

### Task 7: Render-path wiring (request flag, second readback, slot)

**Files:**
- Modify: `Source/CamSimTest/Camera/FrameGrabRequestQueue.h`, `CamSimFrameGrabExtension.h/.cpp`, `CamSimCaptureComponent.h/.cpp`
- Test: `Tests/ReadbackRingTest.cpp` / `CameraWiringTest.cpp` (extend if they cover `DecidePoll`); the end-to-end check is Task 8.

**Interfaces:**
- Consumes: `AddInstanceIdPass`, `FInstanceIdInputs` (Task 5); `FGroundTruthCollector::WriteAnnotationFrame(TArray, const FInstanceIdImage*, …)` (Task 6).
- Produces: `FFrameGrabRequest::bInstanceIds` (bool, default false); `FCamSimFrameGrabExtension::PushRequest_RenderThread(const FFrameGrabRequest&, FRHIGPUBufferReadback* Nv12Readback, FRHIGPUBufferReadback* IdReadback, TAtomic<uint32>* GrabbedGeneration)`.

- [ ] **Step 1: Request + targets.** `FFrameGrabRequest` gains `bool bInstanceIds = false;`. `FTargets` gains `FRHIGPUBufferReadback* IdReadback = nullptr;`; `PushRequest_RenderThread` stores it.

- [ ] **Step 2: Run the pass in `RunSensor_RenderThread`.** Inside the existing `if (Requests.PopLatest(Req))` / `if (T && …)` block, **before** the NV12 `AddReadbackBufferPass` (that lambda publishes the generation, so the ID copy must be queued first):
```cpp
			if (Req.bInstanceIds && T->IdReadback)
			{
				const FSceneTextureUniformParameters* St = Inputs.SceneTextures.SceneTextures ? Inputs.SceneTextures.SceneTextures->GetParameters() : nullptr;
				if (St && St->SceneDepthTexture && St->CustomDepthTexture && St->CustomStencilTexture)
				{
					FInstanceIdInputs Ii;
					Ii.SceneDepth = St->SceneDepthTexture;
					Ii.CustomDepth = St->CustomDepthTexture;
					Ii.CustomStencil = St->CustomStencilTexture;
					Ii.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
					Ii.OutputSize = CaptureSize;
					const FRDGBufferRef Ids = AddInstanceIdPass(GraphBuilder, Ii, Params);
					FRHIGPUBufferReadback* IdRb = T->IdReadback;
					const uint32 IdBytes = static_cast<uint32>(CaptureSize.X * CaptureSize.Y * 2);
					AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CamSimInstanceIdReadback"), Ids,
						[IdRb, Ids, IdBytes](FRHICommandListImmediate& RHICmdList) { IdRb->EnqueueCopy(RHICmdList, Ids->GetRHI(), IdBytes); });
				}
			}
```
(The scene-texture uniform parameter struct is `FSceneTextureUniformParameters` in `SceneTexturesConfig.h` / `SceneRenderTargetParameters.h` — include whichever declares it in UE 5.8; field names may be `CustomStencilTexture` (SRV). If the post-process inputs do not carry scene textures in the `ReplacingTonemapper` callback, read them from `FPostProcessMaterialInputs::SceneTextures` / `GetSceneTextureShaderParameters` — the spec's Risk 2 — and report it rather than guessing.) If the instance pass couldn't be added, the poll must not wait for an ID copy that never comes: record per slot whether the copy was issued — add `TAtomic<uint32>* IdGrabbed` next to `GrabbedGeneration` (store `Gen` in the ID lambda) and have the poll treat "Grabbed == Gen but IdGrabbed != Gen" as "no IDs this frame".

- [ ] **Step 3: Capture component.**
  - Pool: `TArray<TUniquePtr<FRHIGPUBufferReadback>> IdReadbackPool;` and `TAtomic<uint32> IdGrabbedGeneration[FReadbackRing::NumSlots]`, created where `Nv12ReadbackPool` is (one per slot) when `Cfg.MLTraining.bEnabled && Cfg.MLTraining.bBoundingBoxes`; reset where it is reset.
  - `FSlot` gains `TArray<uint32> InstanceIds;` and `bool bWantIds = false;` (game thread writes at capture, render thread reads in the poll — it is captured by value into the poll lambda, so no atomic needed).
  - `Capture()`: `S.bWantIds = IdReadbackPool.IsValidIndex(Slot) && (FrameIdx % FMath::Max(1, Cfg.MLTraining.AnnotationIntervalFrames)) == 0;` push `{ FrameIdx, Gen, Slot, S.bWantIds }` and the slot's ID readback.
  - `EnqueuePoll`: capture `bWantIds`, the ID readback, `&IdGrabbedGeneration[Slot]`, and `IdBytes = GpuSensorSize.X * GpuSensorSize.Y * 2`. Fence-ready lambda: `Nv12Readback->IsReady() && (!bWantIds || IdGrabbed->Load() != CaptureGen || IdReadback->IsReady())`. After the NV12 copy: `S.InstanceIds.Reset(); if (bWantIds && IdGrabbed->Load() == CaptureGen) { if (const void* Raw = IdReadback->Lock(IdBytes)) { S.InstanceIds.SetNumUninitialized(IdBytes / 4); FMemory::Memcpy(S.InstanceIds.GetData(), Raw, IdBytes); IdReadback->Unlock(); } }` — a failed lock leaves the slot without IDs (projection fallback), it does not fail the frame.
  - `Poll()`: reset `S.InstanceIds` wherever `S.Depth` is reset; pass `MoveTemp(S.InstanceIds)` to `SubmitFrameToEncoder`, which gains a `TArray<uint32> InstanceIds` parameter and, in the task, builds `FInstanceIdImage Ids{ GpuSensorSize.X, GpuSensorSize.Y, MoveTemp(InstanceIds) }` (capture `GpuSensorSize` by value) and calls `Collector->WriteAnnotationFrame(MoveTemp(Entities), Ids.Words.Num() ? &Ids : nullptr, Telemetry, FrameIdx)`.

- [ ] **Step 4: Build and run the full suites** — `scripts/run.sh --build-only && run_tests CamSim && scripts/run_gpu_tests.sh CamSim.GPU` → all pass.

- [ ] **Step 5: Smoke run.** `CAMSIM_ML_ENABLED=1 scripts/run.sh --headless` with `scripts/send_dis_test.py both` and the nadir view used by `scripts/dis_vehicle_check.py` (or run that script, which enables ML ground truth); after ~30 s stop, and check `ml_output/camsim_coco.jsonl` lines contain `"mask_source":"render"` for the truck and boat. If they're all `projection`, debug before continuing (custom depth not rendering, stencil not set, scene textures missing).

- [ ] **Step 6: Commit** — `git commit -m "feat(gt): instance-ID readback rides in the ring slot; collector gets the frame's ID image"`.

---

### Task 8: Acceptance script, live run, docs

**Files:**
- Create: `scripts/gt_check_lib.py`, `scripts/tests/test_gt_check_lib.py`, `scripts/gt_occlusion_check.py`
- Create: `docs/ground-truth.md`; modify `ROADMAP.md`, `CLAUDE.md` (and its command table)

**Interfaces:**
- Consumes: COCO JSONL from Tasks 6–7; helpers in `scripts/dis_vehicle_check.py` (`vehicle`, `nadir_on`, `side_on`, `wait_tiles`, `shoot`, the CamSim launch via `bench.run_bench`) and `scripts/ocean_check.py` (Beaufort / Wave Control over CIGI).
- Produces: `gt_check_lib.py` pure functions:
  ```python
  def load_coco(path: Path) -> list[dict]                       # one dict per frame line
  def ann_for(frames: list[dict], cls: str) -> list[tuple[dict, dict]]   # (frame, annotation) pairs for a class
  def rle_area(seg: dict) -> int                                 # via pycocotools.mask.area
  def check_rle(frames) -> list[str]                             # problems: undecodable or area != ann["area"]
  def obb_heading_error_deg(ann: dict, expected_image_angle_deg: float) -> float   # mod 180
  def draw_overlay(png_in: Path, png_out: Path, anns: list[dict]) -> None         # bbox (green), obb (yellow), box3d (cyan), mask tint (magenta)
  ```

- [ ] **Step 1: pytest for the lib** — `scripts/tests/test_gt_check_lib.py`: `rle_area` on the pycocotools encoding of a 4×5 mask equals 6; `check_rle` flags an annotation whose `area` disagrees; `obb_heading_error_deg` wraps (`[0,0,10,2,89]` vs `-89` → 2.0); `load_coco` on a two-line temp file. Run: `cd scripts && uv run -q --with pytest --with numpy --with pycocotools --with pillow pytest tests/test_gt_check_lib.py -q` → fail (module missing), then implement `gt_check_lib.py`, then pass.

- [ ] **Step 2: Acceptance script** — `scripts/gt_occlusion_check.py OUTDIR` (docstring in the style of `dis_vehicle_check.py`, run with `uv run -q --with numpy --with pillow --with pycocotools python …`). Launch CamSim headless with `CAMSIM_ML_ENABLED=1` (and depth off: `CAMSIM_ML_DEPTH_ENABLED=0`) the way `dis_vehicle_check.py` does, start `send_dis_test.py both`, then views, each recording its frame-id range from the COCO timestamps:
  1. `nadir_truck`, `nadir_boat` (as `dis_vehicle_check.py`): pass if, for the class, median visibility ≥ 0.95, max truncation ≤ 0.01, and median OBB heading error vs the vehicle heading mapped into the image (nadir, camera yaw known) ≤ 10°.
  2. `edge_truck`: nadir framing offset so the predicted truck position sits on the right image edge (offset the look-at point by half the ground footprint width): pass if some frames have 0.3 ≤ truncation ≤ 0.7 and their modal bbox reaches x ≥ W − 1.
  3. `crest_boat`: Beaufort 6 (CIGI Wave Control / the env var `ocean_check.py` uses), camera ~300 m from the boat at ~3° depression: pass if ≥ 10 % of boat frames have visibility < 0.9 (crests hide the hull). If no frame dips, this is the spec's Risk 1 — stop and report.
  4. `terrain_truck`: low view (~20 m above ground, ~400 m away) across the Presidio loop: report the fraction of frames with visibility < 0.9 (informational, not pass/fail).
  5. Every annotation: `check_rle` empty.
  6. Frame time: median of `frames.jsonl` frame times in the nadir views vs. a 30 s run with `CAMSIM_ML_ENABLED=0`, same view: difference < 2 ms.
  Save `OUTDIR/shots/<view>_*.png` via the snapshot helper and `OUTDIR/overlays/<view>_*.png` with `draw_overlay` using the annotations of the matching frame (nearest timestamp). Print a summary table and exit 0 only when 1, 2, 3, 5, 6 pass.

- [ ] **Step 3: Live run** — `uv run -q --with numpy --with pillow --with pycocotools python scripts/gt_occlusion_check.py .cache/gt_check` → fix failures (systematic-debugging) until it exits 0. Look at the overlays yourself: the green box must hug the vehicle, the mask must cover it, the cyan 3D box must wrap it.

- [ ] **Step 4: Docs.**
  - `docs/ground-truth.md`: output files; every COCO field with units and conventions (pixel-edge boxes, OBB format/angle, `box3d` corner order and attitude convention, RLE as pycocotools, `visibility` = in-frame visible / in-frame silhouette pixels, `truncation` from the 3D box, `mask_source`), VOC (`occluded` < 0.95, 0-based coordinates), config keys, limitations (vehicle-on-vehicle amodal, ≤ 1 px edge error under TSR, water counts as an occluder, > 255 entities fall back, depth map and KLV corners still pinhole), and a small Python snippet that reads a line and decodes the mask with pycocotools.
  - `ROADMAP.md`: new section `### 2.7 Ground truth for ATR: tight boxes, OBBs, occlusion (done 2026-MM-DD)` after 2.6 in the style of 2.5/2.6 (what was built, acceptance numbers from Step 3 with overlay images copied to `docs/images/gt/`, deviations, carry-overs: vehicle-on-vehicle amodal, instance PNG, depth/KLV distortion); in 2.5's carry-overs strike the sub-project 2 line with "done in 2.7"; in 3B.3 "Distortion-aware ground truth" note boxes are done in 2.7 (depth and KLV corners still pinhole).
  - `CLAUDE.md`: Architecture/GroundTruth line mentions instance masks; a Gotchas bullet: "**Ground-truth masks** (ROADMAP 2.7): entities render custom depth with a stencil value 1..255 (`FStencilSlotAllocator`, reuse delayed 4 frames); `r.CustomDepth=3`; `InstanceIdCS` runs in the sensor graph only on annotated frames and shares `UndistortScale` with `SensorCS` — change both together; `FInstanceMaskAnalyzer` turns the readback into boxes/OBBs/visibility/RLE on the task thread"; command table row for `scripts/gt_occlusion_check.py`; update the test count line (count `IMPLEMENT_SIMPLE_AUTOMATION_TEST` + `IMPLEMENT_COMPLEX_AUTOMATION_TEST` in `Tests/` and the file count).

- [ ] **Step 5: Final verification** — `run_tests CamSim`, `scripts/run_gpu_tests.sh CamSim.GPU`, the pytest suite (`cd scripts && uv run -q --with pytest --with numpy --with pycocotools --with pillow pytest tests -q`), all green.
- [ ] **Step 6: Commit** — `git commit -m "docs(gt): ROADMAP 2.7 acceptance, ground-truth guide; gt_occlusion_check.py"`.
