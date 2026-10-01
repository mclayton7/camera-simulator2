// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include <limits>
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalReference.h"
#include "Time/SimClock.h"
#include "Tests/ThermalTestScene.h"

// CamSim.Thermal.Builder.*: per-frame FThermalFrameParams from clock, environment, camera and entities (ROADMAP 4A).

namespace
{
	/** San Francisco (Presidio), 21 Dec 2026, UTC hh:mm: 20:10 is local solar noon, 10:10 is 02:00. */
	FThermalFrameInputs SanFrancisco(int32 UtcHour, int32 UtcMinute)
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, UtcHour, UtcMinute));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		In.AirTempC = 15.0; In.WaterTempC = 15.0;
		In.bHasSea = true; In.SeaLevelHaeM = -32.0; In.MaxWaveAmplitudeM = 0.4;
		In.SunIlluminanceLux = 100000.0;
		return In;
	}

	FThermalFrameBuilder MakeBuilder(float Lo = 3.0f, float Hi = 5.0f)
	{
		FThermalFrameBuilder B;
		B.Configure(FCamSimConfig::FThermalConfig(), Lo, Hi);
		return B;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderSignsTest, "CamSim.Thermal.Builder.NightAndNoonSigns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderSignsTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameParams Noon, Night;
	B.Build(SanFrancisco(20, 10), Noon);
	B.Build(SanFrancisco(10, 10), Night);
	const int32 T = FThermalMaterialTable::TerrainDefault, W = FThermalMaterialTable::Water;
	TestTrue(*FString::Printf(TEXT("noon: terrain %.1f K > water %.1f K"), Noon.ClassTempK[T], Noon.ClassTempK[W]), Noon.ClassTempK[T] > Noon.ClassTempK[W]);
	TestTrue(*FString::Printf(TEXT("night: terrain %.1f K < water %.1f K"), Night.ClassTempK[T], Night.ClassTempK[W]), Night.ClassTempK[T] < Night.ClassTempK[W]);
	TestTrue(TEXT("noon reference flux > 0"), Noon.ClassSAbsRef[T] > 100.0f && Noon.EClampWm2 > 100.0f);
	TestEqual(TEXT("night reference flux 0"), Night.ClassSAbsRef[T], 0.0f);
	TestEqual(TEXT("night E clamp 0"), Night.EClampWm2, 0.0f);
	TestEqual(TEXT("classes"), static_cast<int32>(Noon.NumClasses), B.GetMaterials().Num());
	TestEqual(TEXT("terrain class"), Noon.TerrainClass, static_cast<uint32>(FThermalMaterialTable::TerrainDefault));
	TestEqual(TEXT("water class"), Noon.WaterClass, static_cast<uint32>(FThermalMaterialTable::Water));
	TestTrue(TEXT("sky colder than air"), Noon.SkyHemiRadiance < B.GetBand().Radiance(Noon.TairK));
	TestNearlyEqual(TEXT("signal scale = 1 / B(300 K)"), B.GetSignalScale(), 1.0f / B.GetBand().Radiance(300.0f), 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderStencilTest, "CamSim.Thermal.Builder.StencilTableReuseAndUnknownMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderStencilTest::RunTest(const FString& Parameters)
{
	// Review focus 4.
	FThermalFrameBuilder B = MakeBuilder();
	const FThermalMaterialTable& M = B.GetMaterials();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalStencilEntity Truck; Truck.Stencil = 3; Truck.bSurfaceVehicle = true;
	FThermalStencilEntity Jet;   Jet.Stencil = 4;
	FThermalStencilEntity Paved; Paved.Stencil = 5; Paved.ThermalMaterial = TEXT("asphalt"); Paved.ThermalOffsetK = -2.0f;
	FThermalStencilEntity Typo;  Typo.Stencil = 6; Typo.ThermalMaterial = TEXT("unobtainium");
	In.Entities = { Truck, Jet, Paved, Typo };
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("truck: vehicle_paint"), static_cast<int32>(P.StencilClass[3]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("truck: +8 K (surface vehicle)"), P.StencilOffsetK[3], 8.0f);
	TestEqual(TEXT("jet: 0 K"), P.StencilOffsetK[4], 0.0f);
	TestEqual(TEXT("asphalt class"), static_cast<int32>(P.StencilClass[5]), M.Find(TEXT("asphalt")));
	TestEqual(TEXT("explicit offset"), P.StencilOffsetK[5], -2.0f);
	TestEqual(TEXT("unknown material: vehicle_paint"), static_cast<int32>(P.StencilClass[6]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("warning names it"), Warnings.Num() == 1 && Warnings[0].Contains(TEXT("unobtainium")));
	int32 OutOfRange = 0;
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) OutOfRange += P.StencilClass[S] < P.NumClasses ? 0 : 1;
	TestEqual(TEXT("no class index >= NumClasses"), OutOfRange, 0);

	// Stencil 3 released and reused 4 frames later by a different type; stencil 5 gone.
	FThermalStencilEntity Boat; Boat.Stencil = 3; Boat.ThermalMaterial = TEXT("concrete");
	In.Entities = { Boat, Typo };
	Warnings.Reset();
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("reused stencil: the new entity's class"), static_cast<int32>(P.StencilClass[3]), M.Find(TEXT("concrete")));
	TestEqual(TEXT("reused stencil: the new entity's offset"), P.StencilOffsetK[3], 0.0f);
	TestEqual(TEXT("released stencil back to the default"), static_cast<int32>(P.StencilClass[5]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("released stencil offset 0"), P.StencilOffsetK[5], 0.0f);
	TestEqual(TEXT("the unknown name warns once per session"), Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderSeaTest, "CamSim.Thermal.Builder.SeaGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderSeaTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	In.UpWorld = FVector(0.0, 0.1, 1.0);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("water on"), P.bWater, 1u);
	TestNearlyEqual(TEXT("camera height above the sea"), P.CamHeightCm, (342.0f + 32.0f) * 100.0f, 0.5f);
	TestNearlyEqual(TEXT("band = 0.5 m + max amplitude"), P.WaterBandCm, 90.0f, 1e-3f);
	TestNearlyEqual(TEXT("Gaussian radius at 37.8 deg"), P.SeaRadiusCm, static_cast<float>(FThermalFrameBuilder::GaussianRadiusM(37.7989) * 100.0), 1.0f);
	TestTrue(TEXT("radius ~ 6.37e8 cm"), P.SeaRadiusCm > 6.35e8f && P.SeaRadiusCm < 6.39e8f);
	TestNearlyEqual(TEXT("up normalised"), P.Up.Size(), 1.0f, 1e-6f);
	In.bHasSea = false;
	B.Build(In, P);
	TestEqual(TEXT("no sea: no water class"), P.bWater, 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderKLumTest, "CamSim.Thermal.Builder.KLumReferenceSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderKLumTest::RunTest(const FString& Parameters)
{
	// An unshadowed horizontal surface of the class albedo lit by the sun light (lux * sin el) gets S_abs,pix = S_abs,ref:
	// no fast term, through the real reference.
	FThermalFrameBuilder B = MakeBuilder();
	for (const double Cloud : { 0.0, 0.6 })
	{
		FThermalFrameInputs In = SanFrancisco(20, 10);
		In.bHasSea = false;
		In.CloudCover01 = Cloud;
		FThermalFrameParams P;
		B.Build(In, P);
		const FRotator Rot(-30.0, 0.0, 0.0);
		P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(Rot, 60.0f, 64, 36, CamSimThermalTest::NearCm);
		const float A = B.GetMaterials().Get(FThermalMaterialTable::TerrainDefault).Albedo;
		const double SinEl = FMath::Sin(FMath::DegreesToRadians(FThermalModel::SunElevationDeg(B.GetModel().GetSite(),
			FThermalFrameBuilder::LocalSolarSeconds(In.UtcMicros, In.CamLonDeg) / 3600.0)));
		CamSimThermalRef::FPixelSample S;
		S.U = 0.5f; S.V = 0.6f;
		S.DeviceZ = CamSimThermalTest::DeviceZForPlane(P, Rot, S.U, S.V, -700.0f);
		S.bHasBase = true;
		S.Base = FVector3f(A);
		S.Color = FVector3f(static_cast<float>(A * In.SunIlluminanceLux * SinEl / UE_DOUBLE_PI));
		const CamSimThermalRef::FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
		TestNearlyEqual(*FString::Printf(TEXT("cloud %.1f: reference surface keeps the class temperature"), Cloud),
			R.TempK, P.ClassTempK[FThermalMaterialTable::TerrainDefault], 1e-2f);
	}
	FThermalFrameInputs Night = SanFrancisco(10, 10);
	FThermalFrameParams P;
	B.Build(Night, P);
	TestTrue(TEXT("night K_lum finite and > 0"), FMath::IsFinite(P.KLum) && P.KLum > 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderExtinctionTest, "CamSim.Thermal.Builder.Extinction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderExtinctionTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder Mwir = MakeBuilder(3.0f, 5.0f), Lwir = MakeBuilder(8.0f, 12.0f);
	TestTrue(TEXT("3-5 um is MWIR"), Mwir.IsMwir());
	TestFalse(TEXT("8-12 um is LWIR"), Lwir.IsMwir());
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalFrameParams P;
	Mwir.Build(In, P);
	TestNearlyEqual(TEXT("MWIR beta"), P.BetaPerCm, 0.15f / 1e5f, 1e-9f);
	Lwir.Build(In, P);
	TestNearlyEqual(TEXT("LWIR beta"), P.BetaPerCm, 0.10f / 1e5f, 1e-9f);
	In.bFogActive = true;
	In.VisibilityM = 2000.0;
	Mwir.Build(In, P);
	TestNearlyEqual(TEXT("fog: + 3.912 / V_km * 0.4"), P.BetaPerCm, static_cast<float>((0.15 + 3.912 / 2.0 * 0.4) / 1e5), 1e-9f);
	In.VisibilityM = 0.0;
	Mwir.Build(In, P);
	TestTrue(TEXT("zero visibility stays finite"), FMath::IsFinite(P.BetaPerCm));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderFallbackTest, "CamSim.Thermal.Builder.FallbackFastTerm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderFallbackTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("base colour + sun light: fast term on"), P.KFastScale, 1.0f);
	In.bBaseColorAvailable = false;
	B.Build(In, P);
	TestEqual(TEXT("no base colour (Task 1 FALLBACK): k_fast = 0"), P.KFastScale, 0.0f);
	In.bBaseColorAvailable = true;
	In.SunIlluminanceLux = 0.0;
	B.Build(In, P);
	TestEqual(TEXT("no atmosphere sun light: k_fast = 0"), P.KFastScale, 0.0f);
	In.SunIlluminanceLux = 1e5;
	In.bBaseColorSrgb = true;
	B.Build(In, P);
	TestEqual(TEXT("sRGB flag passed through"), P.bBaseColorSrgb, 1u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderDateLineTest, "CamSim.Thermal.Builder.DateLineLocalTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderDateLineTest::RunTest(const FString& Parameters)
{
	// Review focus 3: local solar time and date across the date line, near UTC midnight.
	const uint64 Late = FSimClock::ToMicros(FDateTime(2026, 12, 31, 23, 59, 59));
	const double East = FThermalFrameBuilder::LocalSolarSeconds(Late, 179.9);
	const double West = FThermalFrameBuilder::LocalSolarSeconds(Late, -179.9);
	TestTrue(TEXT("east of Greenwich: in [0, 86400)"), East >= 0.0 && East < 86400.0);
	TestTrue(TEXT("west of Greenwich: in [0, 86400)"), West >= 0.0 && West < 86400.0);
	TestNearlyEqual(TEXT("+179.9: 11:59:35 next day"), East, FMath::Fmod(86399.0 + 179.9 * 240.0, 86400.0), 1e-3);
	TestEqual(TEXT("+179.9 rolls into 2027"), FThermalFrameBuilder::LocalSolarDate(Late, 179.9).GetYear(), 2027);
	TestEqual(TEXT("-179.9 stays 31 Dec"), FThermalFrameBuilder::LocalSolarDate(Late, -179.9).GetDayOfYear(), 365);
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(23, 59);
	In.UtcMicros = Late;
	for (const double Lon : { 179.9, -179.9 })
	{
		In.CamLonDeg = Lon;
		FThermalFrameParams P;
		B.Build(In, P);
		bool bFinite = FMath::IsFinite(P.KLum) && FMath::IsFinite(P.SkyHemiRadiance);
		for (uint32 C = 0; C < P.NumClasses; ++C) bFinite &= FMath::IsFinite(P.ClassTempK[C]) && P.ClassTempK[C] > 150.0f && P.ClassTempK[C] < 400.0f;
		TestTrue(*FString::Printf(TEXT("lon %.1f: finite, in range"), Lon), bFinite);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderCostTest, "CamSim.Thermal.Builder.PerFrameCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderCostTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	for (int32 S = 1; S <= 32; ++S) { FThermalStencilEntity E; E.Stencil = static_cast<uint8>(S); E.bSurfaceVehicle = true; In.Entities.Add(E); }
	FThermalFrameParams P;
	B.Build(In, P);   // fits the model and builds the LUT once
	constexpr int32 Frames = 1000;
	const double T0 = FPlatformTime::Seconds();
	for (int32 F = 0; F < Frames; ++F)
	{
		In.UtcMicros += 33333;
		B.Build(In, P);
	}
	const double UsPerFrame = (FPlatformTime::Seconds() - T0) * 1e6 / Frames;
	AddInfo(FString::Printf(TEXT("builder: %.1f us per frame (budget 50 us)"), UsPerFrame));
	if (UsPerFrame > 50.0) AddWarning(FString::Printf(TEXT("builder over the 50 us budget: %.1f us"), UsPerFrame));
	TestTrue(TEXT("builder per frame well under a millisecond"), UsPerFrame < 500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderSanitiseTest, "CamSim.Thermal.Builder.SanitisedInputsNoRefit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderSanitiseTest::RunTest(const FString& Parameters)
{
	// Ruling R4: cloud/air jitter below the quantum must not refit; NaN inputs fall back to defaults.
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	In.CloudCover01 = 0.301;
	FThermalFrameParams A, C;
	B.Build(In, A);
	In.CloudCover01 = 0.304;
	In.AirTempC = 15.01;
	B.Build(In, C);
	bool bSame = A.NumClasses == C.NumClasses;
	for (uint32 K = 0; K < A.NumClasses; ++K) bSame &= A.ClassTempK[K] == C.ClassTempK[K] && A.ClassSAbsRef[K] == C.ClassSAbsRef[K];
	TestTrue(TEXT("sub-quantum jitter: identical class temperatures (no refit)"), bSame);

	const double NaN = std::numeric_limits<double>::quiet_NaN();
	FThermalFrameInputs Bad = SanFrancisco(20, 10);
	Bad.CloudCover01 = NaN; Bad.AirTempC = NaN; Bad.WaterTempC = NaN; Bad.VisibilityM = NaN; Bad.bFogActive = true;
	FThermalFrameParams P;
	B.Build(Bad, P);
	bool bFinite = FMath::IsFinite(P.KLum) && FMath::IsFinite(P.SkyHemiRadiance) && FMath::IsFinite(P.SkyEpsZ)
		&& FMath::IsFinite(P.TairK) && FMath::IsFinite(P.BetaPerCm) && FMath::IsFinite(P.Cloud);
	for (uint32 K = 0; K < P.NumClasses; ++K) bFinite &= FMath::IsFinite(P.ClassTempK[K]);
	TestTrue(TEXT("NaN inputs give finite params"), bFinite);
	return true;
}
