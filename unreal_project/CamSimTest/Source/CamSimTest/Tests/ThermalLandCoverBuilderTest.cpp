// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Time/SimClock.h"
#include "Tests/ThermalTestScene.h"

#include <limits>

// CamSim.Thermal.Builder.LandCover*: land-cover fields of FThermalFrameParams (ROADMAP 4B).

namespace
{
	FThermalFrameInputs SanFrancisco()
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, 20, 10));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		In.AirTempC = 15.0; In.WaterTempC = 15.0;
		In.bHasSea = true; In.SeaLevelHaeM = -32.0; In.MaxWaveAmplitudeM = 0.4;
		In.SunIlluminanceLux = 100000.0;
		return In;
	}

	FThermalLandCoverInput Window(double Lat, double Lon)
	{
		FThermalLandCoverInput L;
		L.bValid = true; L.WindowId = 5; L.CentreLatDeg = Lat; L.CentreLonDeg = Lon; L.Texels = 2048; L.TexelM = 10.0f;
		L.EastWorld = FVector(0.6, 0.8, 0.0);
		L.NorthWorld = FVector(0.8, -0.6, 0.0);
		return L;
	}

	FThermalFrameBuilder MakeBuilder(const FCamSimConfig::FThermalConfig& Cfg = FCamSimConfig::FThermalConfig())
	{
		FThermalFrameBuilder B;
		B.Configure(Cfg, 3.0f, 5.0f);
		return B;
	}

	/** CamSimThermalRef radiance of MakeScene's pixels with land-cover codes bound, under P's thermal values. */
	TArray<float> RenderScene(const FThermalFrameParams& P)
	{
		CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(64, 36, 64, 36);
		FThermalFrameParams Q = P;
		Q.ClipToTranslatedWorld = S.P.ClipToTranslatedWorld;
		Q.Up = S.P.Up;
		S.LandCover = CamSimThermalTest::MakeLandCoverCodes(Q.LandCoverTexels);
		TArray<float> Out;
		for (const CamSimThermalRef::FPixelResult& R : CamSimThermalRef::Run(S.Images(true), Q)) Out.Add(R.Radiance);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverMappingTest, "CamSim.Thermal.Builder.LandCoverMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverMappingTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco();
	In.LandCover = Window(37.79, -122.475);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("on"), P.bLandCover, 1u);
	TestEqual(TEXT("window id"), P.LandCoverWindowId, 5u);
	CamSimLandCover::FWindowSpec Spec;
	Spec.CentreLatDeg = 37.79; Spec.CentreLonDeg = -122.475; Spec.Texels = 2048; Spec.TexelM = 10.0f;
	const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(Spec, In.CamLatDeg, In.CamLonDeg);
	TestNearlyEqual(TEXT("camera offset E (doubles, same mapping as the resample)"), P.LandCoverCamOffsetM.X, static_cast<float>(Off.X), 1e-3f);
	TestNearlyEqual(TEXT("camera offset N"), P.LandCoverCamOffsetM.Y, static_cast<float>(Off.Y), 1e-3f);
	TestTrue(TEXT("camera is north-east of the centre, ~0.8 / 1.0 km"), Off.X > 700.0 && Off.X < 850.0 && Off.Y > 950.0 && Off.Y < 1030.0);
	TestTrue(TEXT("axes copied"), P.LandCoverEast.Equals(FVector3f(0.6f, 0.8f, 0.0f), 1e-6f) && P.LandCoverNorth.Equals(FVector3f(0.8f, -0.6f, 0.0f), 1e-6f));
	TestEqual(TEXT("texels"), P.LandCoverTexels, 2048u);
	TestEqual(TEXT("texel size"), P.LandCoverTexelM, 10.0f);
	TestEqual(TEXT("code 10 -> tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	TestEqual(TEXT("code 50 family built-up"), P.LandCoverFamily[50], FThermalFrameParams::LandCoverFamilyBuiltUp);
	TestEqual(TEXT("refinement on with base colour"), P.bLandCoverRefine, 1u);
	TestEqual(TEXT("vegetation target"), P.VegetationClass, static_cast<uint32>(FThermalMaterialTable::Vegetation));
	TestEqual(TEXT("bare target"), P.BareSoilClass, static_cast<uint32>(FThermalMaterialTable::BareSoil));
	TestEqual(TEXT("asphalt target"), P.AsphaltClass, static_cast<uint32>(FThermalMaterialTable::Asphalt));
	TestEqual(TEXT("concrete target"), P.ConcreteClass, static_cast<uint32>(FThermalMaterialTable::Concrete));
	TestEqual(TEXT("thresholds from config"), P.VegIndexLo, FCamSimConfig::FThermalConfig::FLandCoverConfig().VegIndexLo);
	TestEqual(TEXT("ramp"), P.AsphaltRampLuma, FThermalFrameBuilder::AsphaltRampLuma);
	TestEqual(TEXT("veg blur from config (fix round 1)"), P.VegBlurM, 2.0f);
	TestEqual(TEXT("base texel angle left for the render thread"), P.BaseTexelAngle, 0.0f);
	In.bBaseColorAvailable = false;
	B.Build(In, P);
	TestEqual(TEXT("no base colour: refinement off"), P.bLandCoverRefine, 0u);
	In.bBaseColorAvailable = true;
	In.SunIlluminanceLux = 0.0;   // night: fast term off, refinement stays on
	B.Build(In, P);
	TestTrue(TEXT("night: KFastScale 0 but refinement on"), P.KFastScale == 0.0f && P.bLandCoverRefine == 1u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverOffTest, "CamSim.Thermal.Builder.LandCoverDisabledIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverOffTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams NoWindow;
	{
		FThermalFrameBuilder B = MakeBuilder();
		B.Build(SanFrancisco(), NoWindow);
	}
	TestEqual(TEXT("no window: off"), NoWindow.bLandCover, 0u);
	const TArray<float> Reference = RenderScene(NoWindow);
	auto Check = [&](const FThermalFrameParams& P, const TCHAR* Why)
	{
		TestEqual(*FString::Printf(TEXT("%s: off"), Why), P.bLandCover, 0u);
		TestEqual(*FString::Printf(TEXT("%s: no window id"), Why), P.LandCoverWindowId, 0u);
		const TArray<float> R = RenderScene(P);
		TestTrue(*FString::Printf(TEXT("%s: bit for bit the no-window output"), Why),
			R.Num() == Reference.Num() && FMemory::Memcmp(R.GetData(), Reference.GetData(), R.Num() * sizeof(float)) == 0);
	};
	{
		FCamSimConfig::FThermalConfig Cfg;
		Cfg.LandCover.bEnabled = false;
		FThermalFrameBuilder B = MakeBuilder(Cfg);
		FThermalFrameInputs In = SanFrancisco();
		In.LandCover = Window(37.79, -122.475);
		FThermalFrameParams P;
		B.Build(In, P);
		Check(P, TEXT("thermal.land_cover.enabled false with a window"));
	}
	{
		FThermalFrameBuilder B = MakeBuilder();
		FThermalFrameInputs In = SanFrancisco();
		FThermalFrameParams P;
		In.LandCover = Window(37.79, -122.475);
		B.Build(In, P);                       // on ...
		In.LandCover.bValid = false;
		B.Build(In, P);                       // ... then the window goes away: every mapping field back to its default
		Check(P, TEXT("window lost, params reused"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverInvalidTest, "CamSim.Thermal.Builder.LandCoverPoleAndInvalidWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverInvalidTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	auto Off = [&](TFunction<void(FThermalLandCoverInput&)> Break)
	{
		FThermalFrameInputs In = SanFrancisco();
		In.LandCover = Window(37.79, -122.475);
		Break(In.LandCover);
		FThermalFrameParams P;
		B.Build(In, P);
		return P.bLandCover == 0u;
	};
	TestTrue(TEXT("window at 89.5 N"), Off([](FThermalLandCoverInput& L) { L.CentreLatDeg = 89.5; }));
	TestTrue(TEXT("zero texels"), Off([](FThermalLandCoverInput& L) { L.Texels = 0; }));
	TestTrue(TEXT("zero texel size"), Off([](FThermalLandCoverInput& L) { L.TexelM = 0.0f; }));
	TestTrue(TEXT("window id 0"), Off([](FThermalLandCoverInput& L) { L.WindowId = 0; }));
	TestFalse(TEXT("a valid window is on"), Off([](FThermalLandCoverInput&) {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverClassesTest, "CamSim.Thermal.Builder.LandCoverClassesInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverClassesTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FThermalConfig Cfg;
	FThermalMaterialSpec Gravel;
	Gravel.Name = TEXT("gravel");
	Cfg.Materials.Add(Gravel);
	FLandCoverClassSpec ToGravel; ToGravel.Key = TEXT("60"); ToGravel.Code = 60; ToGravel.Material = TEXT("gravel");
	FLandCoverClassSpec ToLava;   ToLava.Key = TEXT("10");   ToLava.Code = 10;   ToLava.Material = TEXT("lava");
	Cfg.LandCover.Classes = { ToGravel, ToLava };
	FThermalFrameBuilder B = MakeBuilder(Cfg);
	FThermalFrameInputs In = SanFrancisco();
	In.LandCover = Window(37.79, -122.475);
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("unknown class material warned once"), Warnings.FilterByPredicate([](const FString& W) { return W.Contains(TEXT("lava")); }).Num(), 1);
	TestEqual(TEXT("code 60 -> gravel"), static_cast<int32>(P.LandCoverClass[60]), B.GetMaterials().Find(TEXT("gravel")));
	TestEqual(TEXT("code 10 keeps tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	int32 Bad = 0;
	for (int32 C = 0; C < FThermalFrameParams::NumLandCoverCodes; ++C) Bad += P.LandCoverClass[C] >= P.NumClasses ? 1 : 0;
	Bad += (P.VegetationClass >= P.NumClasses || P.BareSoilClass >= P.NumClasses || P.AsphaltClass >= P.NumClasses || P.ConcreteClass >= P.NumClasses) ? 1 : 0;
	TestEqual(TEXT("no class index >= NumClasses reaches the shader"), Bad, 0);
	Cfg.LandCover.Classes = { ToGravel };
	ToGravel.Code = 50;
	ToGravel.Key = TEXT("50");
	Cfg.LandCover.Classes.Add(ToGravel);
	B.Configure(Cfg, 3.0f, 5.0f);   // reconfigure the classes
	B.Build(In, P);
	TestEqual(TEXT("reconfigure: code 50 -> gravel"), static_cast<int32>(P.LandCoverClass[50]), B.GetMaterials().Find(TEXT("gravel")));
	TestEqual(TEXT("reconfigure: code 10 back to tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderLandCoverWarpTest, "CamSim.Thermal.Builder.LandCoverWarpAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderLandCoverWarpTest::RunTest(const FString& Parameters)
{
	FCamSimConfig::FThermalConfig Cfg;
	Cfg.LandCover.WarpAmplitudeM = 4.5f;
	Cfg.LandCover.WarpCellM = 30.0f;
	FThermalFrameBuilder B = MakeBuilder(Cfg);
	FThermalFrameInputs In = SanFrancisco();
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("no window: anchor 0"), P.LandCoverAnchorM, FVector2f::ZeroVector);
	TestEqual(TEXT("no window: warp off"), P.LandCoverWarpAmpM, 0.0f);

	In.LandCover = Window(37.79, -122.475);   // the session's first window: it is the anchor
	B.Build(In, P);
	TestEqual(TEXT("warp amplitude from config"), P.LandCoverWarpAmpM, 4.5f);
	TestEqual(TEXT("warp cell from config"), P.LandCoverWarpCellM, 30.0f);
	TestEqual(TEXT("first window: anchor offset 0"), P.LandCoverAnchorM, FVector2f::ZeroVector);
	In.CamLatDeg += 0.002;                     // the camera moves inside the same window: the anchor does not
	B.Build(In, P);
	TestEqual(TEXT("same window, camera moved: anchor 0"), P.LandCoverAnchorM, FVector2f::ZeroVector);

	In.LandCover = Window(37.83, -122.43);     // re-centred ~4.4 km north, ~4 km east
	In.LandCover.WindowId = 6;
	B.Build(In, P);
	CamSimLandCover::FWindowSpec First;
	First.CentreLatDeg = 37.79; First.CentreLonDeg = -122.475; First.Texels = 2048; First.TexelM = 10.0f;
	const FVector2D Expect = CamSimLandCover::GeodeticToWindowEN(First, 37.83, -122.43);   // doubles
	TestNearlyEqual(TEXT("re-centred: anchor = new centre from the first (E)"), P.LandCoverAnchorM.X, static_cast<float>(Expect.X), 1e-3f);
	TestNearlyEqual(TEXT("re-centred: anchor N"), P.LandCoverAnchorM.Y, static_cast<float>(Expect.Y), 1e-3f);
	TestTrue(TEXT("re-centred: ~4 km E, ~4.4 km N"), Expect.X > 3800.0 && Expect.X < 4100.0 && Expect.Y > 4300.0 && Expect.Y < 4600.0);
	// Ground-fixed: a ground point's warp coordinates G = anchor + (sE * E, sN * N) are the same in either window (the
	// window mapping is linear in lat/lon, so the anchor scale makes this exact up to float rounding).
	CamSimLandCover::FWindowSpec Second = First;
	Second.CentreLatDeg = 37.83; Second.CentreLonDeg = -122.43;
	const double CosA = FMath::Cos(FMath::DegreesToRadians(37.79)), CosC = FMath::Cos(FMath::DegreesToRadians(37.83));
	TestNearlyEqual(TEXT("anchor scale E = N(lat_a) cos(lat_a) / (N(lat_c) cos(lat_c))"), static_cast<double>(P.LandCoverAnchorScale.X),
		CamSimLandCover::PrimeVerticalRadiusM(37.79) * CosA / (CamSimLandCover::PrimeVerticalRadiusM(37.83) * CosC), 1e-6);
	TestNearlyEqual(TEXT("anchor scale N = M(lat_a) / M(lat_c)"), static_cast<double>(P.LandCoverAnchorScale.Y),
		CamSimLandCover::MeridionalRadiusM(37.79) / CamSimLandCover::MeridionalRadiusM(37.83), 1e-6);
	double Worst = 0.0;
	for (const FVector2D Off : { FVector2D(-2000.0, -2200.0), FVector2D(4900.0, 4800.0), FVector2D(-5000.0, 5000.0), FVector2D(0.0, 0.0) })
	{
		double PtLat = 0.0, PtLon = 0.0;
		CamSimLandCover::WindowENToGeodetic(Second, Off.X, Off.Y, PtLat, PtLon);
		const FVector2D G1 = CamSimLandCover::GeodeticToWindowEN(First, PtLat, PtLon);   // first window: anchor 0, scale 1
		const FVector2D En = CamSimLandCover::GeodeticToWindowEN(Second, PtLat, PtLon);
		const FVector2D G2(P.LandCoverAnchorM.X + P.LandCoverAnchorScale.X * static_cast<float>(En.X),
			P.LandCoverAnchorM.Y + P.LandCoverAnchorScale.Y * static_cast<float>(En.Y));
		Worst = FMath::Max(Worst, FVector2D::Distance(G1, G2));
	}
	AddInfo(FString::Printf(TEXT("warp ground coordinates across the re-centre differ by %.4f m at most"), Worst));
	TestTrue(TEXT("ground points up to 7 km from the centre: warp coordinates agree across the re-centre (< 0.01 m)"), Worst < 0.01);

	In.LandCover = Window(37.79, -122.475);    // back: anchor offset 0 again
	In.LandCover.WindowId = 7;
	B.Build(In, P);
	TestTrue(TEXT("back at the first centre: anchor ~0, scale 1"), P.LandCoverAnchorM.Size() < 1e-3f && P.LandCoverAnchorScale.Equals(FVector2f(1.0f), 1e-6f));

	In.LandCover.bValid = false;               // window lost: warp fields reset, anchor kept for the session
	B.Build(In, P);
	TestTrue(TEXT("window lost: defaults"), P.LandCoverAnchorM == FVector2f::ZeroVector && P.LandCoverWarpAmpM == 0.0f);
	In.LandCover = Window(37.83, -122.43);
	B.Build(In, P);
	TestNearlyEqual(TEXT("window back: same session anchor"), P.LandCoverAnchorM.X, static_cast<float>(Expect.X), 1e-3f);

	{
		FThermalFrameBuilder Far = MakeBuilder(Cfg);
		FThermalFrameParams Q;
		Far.Build(In, Q);
		In.LandCover = Window(40.0, -120.0);       // ~330 km away: past MaxWarpAnchorM, a new session anchor
		Far.Build(In, Q);
		TestEqual(TEXT("far window: re-latched"), Q.LandCoverAnchorM, FVector2f::ZeroVector);
		In.LandCover = Window(37.83, -122.43);
	}
	B.Configure(Cfg, 3.0f, 5.0f);              // reconfigure, same dir: anchor kept
	B.Build(In, P);
	TestNearlyEqual(TEXT("reload, same dir: anchor kept"), P.LandCoverAnchorM.X, static_cast<float>(Expect.X), 1e-3f);
	Cfg.LandCover.Dir = TEXT("Content/NonUFS/OtherLandCover");
	B.Configure(Cfg, 3.0f, 5.0f);              // new land-cover data: a new session anchor at the next window
	B.Build(In, P);
	TestEqual(TEXT("new dir: re-latched at this window"), P.LandCoverAnchorM, FVector2f::ZeroVector);

	Cfg.LandCover.WarpAmplitudeM = 0.0f;
	B.Configure(Cfg, 3.0f, 5.0f);
	B.Build(In, P);
	TestEqual(TEXT("warp_amplitude_m 0: off"), P.LandCoverWarpAmpM, 0.0f);
	// Validate() rejects these; the builder still never hands the shader a non-finite or out-of-range value.
	Cfg.LandCover.WarpAmplitudeM = std::numeric_limits<float>::quiet_NaN();
	Cfg.LandCover.WarpCellM = std::numeric_limits<float>::quiet_NaN();
	B.Configure(Cfg, 3.0f, 5.0f);
	B.Build(In, P);
	TestTrue(TEXT("NaN amplitude -> 0, NaN cell -> default"), P.LandCoverWarpAmpM == 0.0f && P.LandCoverWarpCellM == 20.0f);
	Cfg.LandCover.WarpAmplitudeM = 1e9f;
	Cfg.LandCover.WarpCellM = 0.0f;
	B.Configure(Cfg, 3.0f, 5.0f);
	B.Build(In, P);
	TestTrue(TEXT("out of range: clamped to [0, 20] m and [5, 200] m"), P.LandCoverWarpAmpM == 20.0f && P.LandCoverWarpCellM == 5.0f);
	Cfg.LandCover.VegBlurM = std::numeric_limits<float>::quiet_NaN();
	B.Configure(Cfg, 3.0f, 5.0f);
	B.Build(In, P);
	TestEqual(TEXT("NaN veg blur -> 0 (off)"), P.VegBlurM, 0.0f);
	Cfg.LandCover.VegBlurM = 100.0f;
	B.Configure(Cfg, 3.0f, 5.0f);
	B.Build(In, P);
	TestEqual(TEXT("veg blur clamped to 32 m"), P.VegBlurM, 32.0f);
	return true;
}
