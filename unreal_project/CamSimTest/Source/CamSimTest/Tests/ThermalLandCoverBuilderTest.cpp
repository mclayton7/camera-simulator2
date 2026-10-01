// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Time/SimClock.h"
#include "Tests/ThermalTestScene.h"

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
	B.Configure(Cfg, 3.0f, 5.0f);   // hot reload of the classes
	B.Build(In, P);
	TestEqual(TEXT("hot reload: code 50 -> gravel"), static_cast<int32>(P.LandCoverClass[50]), B.GetMaterials().Find(TEXT("gravel")));
	TestEqual(TEXT("hot reload: code 10 back to tree_canopy"), static_cast<int32>(P.LandCoverClass[10]), FThermalMaterialTable::TreeCanopy);
	return true;
}
