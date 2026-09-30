// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanQueries.h"
#include "Ocean/OceanSurface.h"

#include <limits>

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
	TestFalse(TEXT("miss over sea: invalid (water only raises a valid hit)"), R.bValid);
	TestFalse(TEXT("miss over sea: not water"), R.bWater);
	R = CombineHot(TOptional<double>(std::numeric_limits<double>::quiet_NaN()), &S, 37.8, -122.45);
	TestFalse(TEXT("non-finite hit over sea: invalid"), R.bValid);

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
