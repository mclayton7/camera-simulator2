// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/SurfaceClamp.h"
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

	// Water: hit, then no hit → EGM96 sea level, then neither → sender.
	FClampState W;
	P = ClampWater(Sender(10.0, 0.0), -31.5, -32.0, 0.0, W);
	TestEqual(TEXT("water hit"), P.Alt, -31.5, 1e-9);
	P = ClampWater(Sender(10.0, 0.0), {}, -32.0, 10.0, W);
	TestEqual(TEXT("water miss → sea level"), P.Alt, -32.0, 1e-3);
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
