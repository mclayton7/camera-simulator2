// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/SurfaceClamp.h"
#include "Entity/SurfaceProbe.h"
#include "Geospatial/CigiFrames.h"

#include <limits>

using namespace CamSimSurface;

namespace
{
	CamSimFrames::FGeoPose Sender(double HeadingDeg = 30.0, double Alt = 0.0)
	{
		CamSimFrames::FGeoPose P;
		P.Lat = 37.795; P.Lon = -122.46; P.Alt = Alt;
		P.Neu = CamSimFrames::CigiToNeu(HeadingDeg, 0.0, 0.0);
		return P;
	}
	FGroundHits Hits(double Bow, double Stern, double Port, double Stbd)
	{
		FGroundHits H; H.Bow = Bow; H.Stern = Stern; H.Port = Port; H.Stbd = Stbd; return H;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampGroundTest, "CamSim.Entity.SurfaceClamp.Ground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampGroundTest::RunTest(const FString& Parameters)
{
	FClampState S;
	// Bow 1 m above stern over 10 m; starboard 1 m below port over 2 m.
	const CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(101.0, 99.0, 100.5, 99.5), 5.0, 1.0, 0.0, S);
	const FRotator R = P.Neu.Rotator();
	TestEqual(TEXT("height = mean of hits"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("heading kept"), R.Yaw, 30.0, 1e-6);
	TestEqual(TEXT("nose up"), R.Pitch, FMath::RadiansToDegrees(FMath::Atan(2.0 / 10.0)), 1e-6);
	TestEqual(TEXT("right side down = +roll"), R.Roll, FMath::RadiansToDegrees(FMath::Atan(1.0 / 2.0)), 1e-6);
	TestEqual(TEXT("lat/lon kept"), P.Lat, 37.795, 1e-12);
	TestTrue(TEXT("state has surface"), S.bHasSurface);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampMissTest, "CamSim.Entity.SurfaceClamp.Misses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampMissTest::RunTest(const FString& Parameters)
{
	FClampState S;
	// Before any hit: the sender's pose.
	CamSimFrames::FGeoPose P = ClampGround(Sender(30.0, 12.0), FGroundHits(), 5.0, 1.0, 0.1, S);
	TestEqual(TEXT("no hit yet → sender alt"), P.Alt, 12.0, 1e-9);
	TestFalse(TEXT("no surface yet"), S.bHasSurface);

	ClampGround(Sender(), Hits(101.0, 99.0, 100.0, 100.0), 5.0, 1.0, 0.0, S);
	const double Pitch = S.PitchDeg;

	// Partial hits: height from the hits (small step eases), tilt held.
	FGroundHits Partial; Partial.Bow = 100.5; Partial.Port = 100.5;
	P = ClampGround(Sender(), Partial, 5.0, 1.0, 10.0, S);  // Dt >> tau → fully eased
	TestEqual(TEXT("partial: mean of hits"), P.Alt, 100.5, 1e-3);
	TestEqual(TEXT("partial: pitch held"), S.PitchDeg, Pitch, 1e-9);

	// No hits: hold.
	P = ClampGround(Sender(), FGroundHits(), 5.0, 1.0, 0.1, S);
	TestEqual(TEXT("miss: height held"), P.Alt, 100.5, 1e-3);
	TestEqual(TEXT("miss: pitch held"), P.Neu.Rotator().Pitch, Pitch, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampEaseTest, "CamSim.Entity.SurfaceClamp.EaseAndSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampEaseTest::RunTest(const FString& Parameters)
{
	FClampState S;
	ClampGround(Sender(), Hits(100, 100, 100, 100), 5.0, 1.0, 0.0, S);
	// 2 m step over one 30 fps frame: eases by 1 - exp(-dt/tau).
	const double Dt = 1.0 / 30.0;
	CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(102, 102, 102, 102), 5.0, 1.0, Dt, S);
	TestEqual(TEXT("small step eases"), P.Alt, 100.0 + 2.0 * (1.0 - FMath::Exp(-Dt / EaseTimeConstantSec)), 1e-9);
	// 50 m step snaps.
	P = ClampGround(Sender(), Hits(150, 150, 150, 150), 5.0, 1.0, Dt, S);
	TestEqual(TEXT("big step snaps"), P.Alt, 150.0, 1e-9);

	// Second commit in the same frame (Dt = 0): a 2 m / steeper change keeps the eased state…
	FClampState D;
	ClampGround(Sender(), Hits(101.0, 99.0, 100.5, 99.5), 5.0, 1.0, 0.0, D);
	const FClampState Before = D;
	P = ClampGround(Sender(), Hits(104.0, 100.0, 103.0, 101.0), 5.0, 1.0, 0.0, D);
	TestEqual(TEXT("dt 0: height kept"), D.Height, Before.Height, 1e-12);
	TestEqual(TEXT("dt 0: pitch kept"), D.PitchDeg, Before.PitchDeg, 1e-12);
	TestEqual(TEXT("dt 0: roll kept"), D.RollDeg, Before.RollDeg, 1e-12);
	TestEqual(TEXT("dt 0: pose at kept height"), P.Alt, Before.Height, 1e-12);
	// …and a jump of >= SnapThresholdM still snaps.
	P = ClampGround(Sender(), Hits(110.0, 110.0, 110.0, 110.0), 5.0, 1.0, 0.0, D);
	TestEqual(TEXT("dt 0: 10 m jump snaps"), P.Alt, 110.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampEdgeTest, "CamSim.Entity.SurfaceClamp.EdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampEdgeTest::RunTest(const FString& Parameters)
{
	// ZeroFootprint: no half sizes → no NaN, height from hits, tilt held (zero).
	FClampState S;
	CamSimFrames::FGeoPose P = ClampGround(Sender(), Hits(100, 100, 100, 100), 0.0, 0.0, 0.0, S);
	TestEqual(TEXT("zero footprint height"), P.Alt, 100.0, 1e-9);
	TestFalse(TEXT("zero footprint: finite pose"), P.Neu.ContainsNaN());
	TestEqual(TEXT("zero footprint: level"), P.Neu.Rotator().Pitch, 0.0, 1e-6);

	// NonFiniteHitIsMiss: NaN/inf heights are ignored.
	FClampState S2;
	const double Nan = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();
	P = ClampGround(Sender(30.0, 7.0), Hits(Nan, Inf, Nan, Nan), 5.0, 1.0, 0.0, S2);
	TestEqual(TEXT("non-finite → miss"), P.Alt, 7.0, 1e-9);
	TestFalse(TEXT("non-finite → no surface"), S2.bHasSurface);

	// Water: miss before any hit → EGM96 sea level (no surface yet); hit; miss after a hit → held.
	FClampState W;
	P = ClampWater(Sender(10.0, 0.0), {}, -32.0, 0.0, W);
	TestEqual(TEXT("water miss before any hit → sea level"), P.Alt, -32.0, 1e-9);
	TestFalse(TEXT("sea level is not a surface hit"), W.bHasSurface);
	P = ClampWater(Sender(10.0, 0.0), -31.5, -32.0, 0.1, W);
	TestEqual(TEXT("water hit"), P.Alt, -31.5, 1e-9);
	P = ClampWater(Sender(10.0, 0.0), {}, -32.0, 10.0, W);
	TestEqual(TEXT("water miss after a hit → held"), P.Alt, -31.5, 1e-9);
	TestEqual(TEXT("water keeps heading"), P.Neu.Rotator().Yaw, 10.0, 1e-6);
	FClampState W2;
	P = ClampWater(Sender(10.0, 3.0), {}, {}, 0.0, W2);
	TestEqual(TEXT("no water, no geoid → sender"), P.Alt, 3.0, 1e-9);

	// Trace span: first from 9 km, then around the last height.
	FClampState Fresh;
	TestEqual(TEXT("first top"), GetTraceSpan(Fresh).TopM, FirstTraceTopM);
	TestEqual(TEXT("first bottom"), GetTraceSpan(Fresh).BottomM, FirstTraceBottomM);
	TestEqual(TEXT("later top"), GetTraceSpan(S).TopM, 100.0 + TraceAboveM, 1e-9);
	TestEqual(TEXT("later bottom"), GetTraceSpan(S).BottomM, 100.0 - TraceBelowM, 1e-9);

	// Footprint: heading north, bow is north of stern, starboard east of port.
	const FFootprint F = GetFootprint(37.795, -122.46, 0.0, 5.0, 1.0);
	TestTrue(TEXT("bow north"), F.Lat[0] > F.Lat[1]);
	TestTrue(TEXT("stbd east"), F.Lon[3] > F.Lon[2]);
	return true;
}

namespace
{
	/** Flat surfaces at fixed heights: a downward trace returns the highest one inside its span; records the spans. */
	class FFakeProbe final : public ISurfaceProbe
	{
	public:
		TArray<double> Surfaces = { 100.0 };
		mutable TArray<TPair<double, double>> Spans;
		virtual TOptional<double> TraceHeight(double, double, double Top, double Bottom) const override
		{
			Spans.Add({ Top, Bottom });
			TOptional<double> Best;
			for (double H : Surfaces)
			{
				if (H <= Top && H >= Bottom && (!Best.IsSet() || H > *Best)) Best = H;
			}
			return Best;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfacePlaceTest, "CamSim.Entity.SurfaceClamp.PlaceOnSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfacePlaceTest::RunTest(const FString& Parameters)
{
	FFakeProbe Probe;
	FClampState S;
	CamSimFrames::FGeoPose P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.0, Probe, S);
	TestEqual(TEXT("ground height"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("four traces"), Probe.Spans.Num(), 4);
	TestEqual(TEXT("first span top"), Probe.Spans[0].Key, FirstTraceTopM);

	// An overpass 60 m up doesn't capture the truck: the next span tops out at +50 m, so it hits the road below.
	Probe.Spans.Reset();
	Probe.Surfaces = { 100.0, 160.0 };
	P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.1, Probe, S);
	TestEqual(TEXT("overpass ignored → stays on the road"), P.Alt, 100.0, 1e-9);
	TestEqual(TEXT("later span top"), Probe.Spans[0].Key, 150.0, 1e-9);
	TestEqual(TEXT("overpass: no retry"), Probe.Spans.Num(), 4);

	// Buried: the ground rose to 200 m (above the narrow span) → every trace misses → one full-span retry.
	Probe.Spans.Reset();
	Probe.Surfaces = { 200.0 };
	P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.1, Probe, S);
	TestEqual(TEXT("buried: retried → snapped to 200 m"), P.Alt, 200.0, 1e-9);
	if (TestEqual(TEXT("buried: 4 narrow + 4 full-span traces"), Probe.Spans.Num(), 8))
	{
		TestEqual(TEXT("narrow first"), Probe.Spans[0].Key, 150.0, 1e-9);
		TestEqual(TEXT("retry top"), Probe.Spans[4].Key, FirstTraceTopM);
		TestEqual(TEXT("retry bottom"), Probe.Spans[4].Value, FirstTraceBottomM);
	}

	// Nothing anywhere: retry misses too → held.
	Probe.Spans.Reset();
	Probe.Surfaces.Reset();
	P = PlaceOnSurface(ESurfaceMode::Ground, Sender(30.0, 0.0), 5.0, 1.0, 0.1, Probe, S);
	TestEqual(TEXT("all miss → held"), P.Alt, 200.0, 1e-9);

	// None: untouched, no traces.
	Probe.Spans.Reset();
	FClampState N;
	P = PlaceOnSurface(ESurfaceMode::None, Sender(30.0, 42.0), 5.0, 1.0, 0.0, Probe, N);
	TestEqual(TEXT("none keeps alt"), P.Alt, 42.0, 1e-9);
	TestEqual(TEXT("none: no traces"), Probe.Spans.Num(), 0);

	// Water: one trace.
	Probe.Spans.Reset();
	Probe.Surfaces = { -30.0 };
	FClampState W;
	P = PlaceOnSurface(ESurfaceMode::Water, Sender(30.0, 0.0), 5.0, 1.0, 0.0, Probe, W);
	TestEqual(TEXT("water height"), P.Alt, -30.0, 1e-9);
	TestEqual(TEXT("one water trace"), Probe.Spans.Num(), 1);

	// Water above the narrow span: one full-span retry.
	Probe.Spans.Reset();
	Probe.Surfaces = { 40.0 };
	P = PlaceOnSurface(ESurfaceMode::Water, Sender(30.0, 0.0), 5.0, 1.0, 0.1, Probe, W);
	TestEqual(TEXT("water retry → 40 m"), P.Alt, 40.0, 1e-9);
	TestEqual(TEXT("water: narrow + retry"), Probe.Spans.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceClampResetTest, "CamSim.Entity.SurfaceClamp.JumpResets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSurfaceClampResetTest::RunTest(const FString& Parameters)
{
	const CamSimFrames::FGeoPose A = Sender();
	CamSimFrames::FGeoPose Near, Far;
	double Alt;
	CamSimFrames::OffsetGeodetic(A.Lat, A.Lon, 0.0, FVector(60.0, 60.0, 0.0), Near.Lat, Near.Lon, Alt);  // ~85 m
	CamSimFrames::OffsetGeodetic(A.Lat, A.Lon, 0.0, FVector(80.0, 80.0, 0.0), Far.Lat, Far.Lon, Alt);    // ~113 m
	TestFalse(TEXT("85 m: no reset"), IsHorizontalJump(A, Near));
	TestTrue(TEXT("113 m: reset"), IsHorizontalJump(A, Far));
	CamSimFrames::FGeoPose Up = A; Up.Alt += 1000.0;
	TestFalse(TEXT("vertical only: no reset"), IsHorizontalJump(A, Up));
	return true;
}
