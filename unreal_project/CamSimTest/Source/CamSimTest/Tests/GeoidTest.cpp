// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Geospatial/Geoid.h"

// -------------------------------------------------------------------------
// EGM96 undulations match NGA's reference output (OUTINTPT.DAT from the
// EGM96 interpolation package). NGA interpolates with a spline; the bilinear
// grid lookup agrees to well under the 0.3 m resolution of KLV Tags 15/25.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoidReferenceTest,
	"CamSim.Geoid.MatchesNgaReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeoidReferenceTest::RunTest(const FString& Parameters)
{
	struct FPoint { double Lat, Lon, Undulation; };
	static const FPoint Reference[] = {
		{  38.6281550, 269.7791550, -31.628 },
		{ -14.6212170, 305.0211140,  -2.969 },
		{  46.8743190, 102.4487290, -43.575 },
		{ -23.6174460, 133.8747120,  15.871 },
		{  38.6254730, 359.9995000,  50.066 },
		{  -0.4667440,   0.0023000,  17.329 },
	};

	for (const FPoint& P : Reference)
	{
		// Also exercise the western-hemisphere form of the same longitude.
		for (const double Lon : { P.Lon, P.Lon - 360.0 })
		{
			const TOptional<double> N = CamSim::Geospatial::GetGeoidUndulation(P.Lat, Lon);
			if (!TestTrue(TEXT("Geoid grid loaded"), N.IsSet()))
			{
				return false;
			}
			TestTrue(FString::Printf(TEXT("Undulation at (%.4f, %.4f) = %.3f, NGA %.3f"), P.Lat, Lon, *N, P.Undulation),
				FMath::IsNearlyEqual(*N, P.Undulation, 0.1));
		}
	}
	return true;
}
