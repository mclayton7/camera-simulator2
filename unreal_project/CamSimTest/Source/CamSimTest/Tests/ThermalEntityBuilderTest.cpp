// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/EntityThermal.h"
#include "Thermal/ThermalMaterials.h"
#include "Time/SimClock.h"

// CamSim.Thermal.Entity.Builder.*: entity records, environment and the render-thread finalise (ROADMAP 4C).

namespace
{
	FThermalFrameInputs Night()
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, 10, 10));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		return In;
	}

	FThermalFrameBuilder Make(bool bEntity)
	{
		FCamSimConfig::FThermalConfig Cfg;
		Cfg.Entity.bEnabled = bEntity;
		FThermalFrameBuilder B;
		B.Configure(Cfg, 3.0f, 5.0f);
		return B;
	}

	/** Bow toward UE +Y (yaw 90), one ellipsoid engine part, skin +3 K, engine +40 K. */
	FThermalStencilEntity Truck(uint8 Stencil)
	{
		FThermalStencilEntity E;
		E.Stencil = Stencil; E.bSurfaceVehicle = true; E.bHasThermalState = true;
		E.OriginWorld = FVector(1000.0, 2000.0, 300.0);
		E.RotationWorld = FQuat(FVector::UpVector, FMath::DegreesToRadians(90.0));
		E.SkinExcessK = 3.0f;
		FEntityThermalPartSpec Eng;
		Eng.Kind = EEntityThermalPartKind::Engine; Eng.Shape = EEntityThermalPartShape::Ellipsoid;
		Eng.CentreM = FVector3f(3.0f, 0.5f, -1.5f); Eng.HalfM = FVector3f(0.7f, 0.6f, 0.4f); Eng.FalloffM = 0.25f;
		E.Parts.Add(Eng);
		E.PartExcessK[0] = 40.0f;
		return E;
	}

	const FVector4f& Row(const FThermalFrameParams& P, int32 S, int32 R) { return P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s + R]; }

	FVector3f Body(const FThermalFrameParams& P, int32 S, const FVector3f& Pw)
	{
		FVector3f Pb;
		for (int32 I = 0; I < 3; ++I) { const FVector4f M = Row(P, S, I); Pb[I] = M.X * Pw.X + M.Y * Pw.Y + M.Z * Pw.Z + M.W; }
		return Pb;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityRecordTest, "CamSim.Thermal.Entity.Builder.EntityRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityRecordTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(7) };
	FThermalFrameParams P;
	B.Build(In, P);
	const float Base = P.ClassTempK[P.StencilClass[7]] + P.StencilOffsetK[7];
	TestEqual(TEXT("4C on: no implicit +8 K"), P.StencilOffsetK[7], 0.0f);
	TestEqual(TEXT("valid"), Row(P, 7, 3).W, 1.0f);
	TestNearlyEqual(TEXT("skin = B + 3"), Row(P, 7, 3).X, Base + 3.0f, 1e-3f);
	TestEqual(TEXT("class"), Row(P, 7, 3).Y, static_cast<float>(P.StencilClass[7]));
	TestEqual(TEXT("parts"), Row(P, 7, 3).Z, 1.0f);
	TestEqual(TEXT("shape ellipsoid"), Row(P, 7, 4).W, 1.0f);
	TestEqual(TEXT("centre x"), Row(P, 7, 4).X, 3.0f);
	TestEqual(TEXT("half z"), Row(P, 7, 5).Z, 0.4f);
	TestEqual(TEXT("falloff"), Row(P, 7, 5).W, 0.25f);
	TestNearlyEqual(TEXT("part T = B + 40"), Row(P, 7, 6).X, Base + 40.0f, 1e-3f);
	TestEqual(TEXT("untagged stencil invalid"), Row(P, 8, 3).W, 0.0f);
	// The world point 3 m ahead of the bow (UE +Y) and 1.5 m up -> body (3, 0, -1.5); camera at (500, 500, 100)
	const FVector3d PreView(-500.0, -500.0, -100.0);
	FinalizeEntityRecords(P, PreView);
	const FVector3f Pb = Body(P, 7, FVector3f(FVector3d(1000.0, 2300.0, 450.0) + PreView));
	TestNearlyEqual(TEXT("body x"), Pb.X, 3.0f, 1e-4f);
	TestNearlyEqual(TEXT("body y"), Pb.Y, 0.0f, 1e-4f);
	TestNearlyEqual(TEXT("body z (down)"), Pb.Z, -1.5f, 1e-4f);
	// Right of the vehicle (UE: actor +Y = world -X for this yaw) -> body +Y
	const FVector3f Pr = Body(P, 7, FVector3f(FVector3d(1000.0 - 100.0, 2000.0, 300.0) + PreView));
	TestNearlyEqual(TEXT("body y right"), Pr.Y, 1.0f, 1e-4f);
	const FEntityThermalEnv& Env = B.GetEntityEnv();
	TestTrue(TEXT("env valid"), Env.bValid);
	TestNearlyEqual(TEXT("env Tair"), Env.TairK, P.TairK, 1e-6f);
	TestNearlyEqual(TEXT("env baseline"), Env.BaselineK[7], Base, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityFarTest, "CamSim.Thermal.Entity.Builder.EntityRecordFarFromOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityFarTest::RunTest(const FString& Parameters)
{
	// Review focus 5: 100 km from the origin, camera 200 m away: body coordinates to 1 mm.
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	FThermalStencilEntity E = Truck(9);
	E.OriginWorld = FVector(1.0e7, -2.0e6, 5.0e4);
	In.Entities = { E };
	FThermalFrameParams P;
	B.Build(In, P);
	const FVector3d Cam(1.0e7 + 20000.0, -2.0e6, 5.0e4 + 3000.0);
	FinalizeEntityRecords(P, -Cam);
	const FVector3f Pb = Body(P, 9, FVector3f(E.OriginWorld + FVector3d(0.0, 300.0, 150.0) - Cam));
	TestNearlyEqual(TEXT("far x"), Pb.X, 3.0f, 1e-3f);
	TestNearlyEqual(TEXT("far y"), Pb.Y, 0.0f, 1e-3f);
	TestNearlyEqual(TEXT("far z"), Pb.Z, -1.5f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityReuseTest, "CamSim.Thermal.Entity.Builder.EntityRecordReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityReuseTest::RunTest(const FString& Parameters)
{
	// Review focus 3: a stencil reused by an entity without parts carries none of the previous owner's parts.
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(5) };
	FThermalFrameParams P;
	B.Build(In, P);
	FThermalStencilEntity Plain;
	Plain.Stencil = 5; Plain.bHasThermalState = true;
	In.Entities = { Plain };
	FThermalFrameParams Q = P;   // reuse the same params object contents
	B.Build(In, Q);
	TestEqual(TEXT("valid"), Row(Q, 5, 3).W, 1.0f);
	TestEqual(TEXT("no parts"), Row(Q, 5, 3).Z, 0.0f);
	TestEqual(TEXT("part rows cleared"), Row(Q, 5, 6).X, 0.0f);
	In.Entities.Reset();
	B.Build(In, Q);
	TestEqual(TEXT("released stencil invalid"), Row(Q, 5, 3).W, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityDisabledTest, "CamSim.Thermal.Entity.Builder.DisabledIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityDisabledTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(false);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(3) };
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("4A +8 K"), P.StencilOffsetK[3], 8.0f);
	int32 Valid = 0;
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) Valid += Row(P, S, 3).W != 0.0f ? 1 : 0;
	TestEqual(TEXT("no valid record with 4C off"), Valid, 0);
	TestFalse(TEXT("no env with 4C off"), B.GetEntityEnv().bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityRejectTest, "CamSim.Thermal.Entity.Builder.RejectsNonFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityRejectTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	FThermalStencilEntity BadOrigin = Truck(10); BadOrigin.OriginWorld.X = NAN;
	FThermalStencilEntity BadPart = Truck(11); BadPart.PartExcessK[0] = INFINITY;
	In.Entities = { BadOrigin, BadPart };
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("bad origin: invalid record"), Row(P, 10, 3).W, 0.0f);
	TestEqual(TEXT("bad part: record valid"), Row(P, 11, 3).W, 1.0f);
	TestEqual(TEXT("bad part dropped"), Row(P, 11, 3).Z, 0.0f);
	TestTrue(TEXT("warned"), Warnings.Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityLatchTest, "CamSim.Thermal.Entity.Builder.EnvLatchedPerEntity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityLatchTest::RunTest(const FString& Parameters)
{
	// Final review #1: the environment of the last IR frame is latched per entity, at build time, from that entity's own
	// stencil. A stencil that was not live then (an entity spawned later, e.g. in EO) has no baseline (0): latching it must
	// fail, so the entity steps without an environment (D = 0) instead of with B = 0 K (a vehicle that looks on fire).
	FThermalFrameBuilder B = Make(true);
	FThermalFrameInputs In = Night();
	In.Entities = { Truck(7) };
	FThermalFrameParams P;
	B.Build(In, P);
	const FEntityThermalEnv& Env = B.GetEntityEnv();
	FEntityThermalLatch Live, Later;
	TestTrue(TEXT("live stencil latches"), CamSimEntityThermal::LatchEnv(Live, Env.bValid, Env.TairK, Env.BaselineK[7]));
	TestNearlyEqual(TEXT("its baseline"), Live.BaselineK, Env.BaselineK[7], 1e-4f);
	TestFalse(TEXT("a stencil not live at the build does not latch"), CamSimEntityThermal::LatchEnv(Later, Env.bValid, Env.TairK, Env.BaselineK[9]));
	TestFalse(TEXT("still no env"), Later.bValid);
	TestFalse(TEXT("4C off: no latch"), CamSimEntityThermal::LatchEnv(Later, false, Env.TairK, Env.BaselineK[7]));
	// The unlatched entity spawned running steps against D = 0: excess bounded by the running targets (engine 45 K)
	FEntityThermalPartSpec Eng; Eng.Kind = EEntityThermalPartKind::Engine;
	const TArray<FEntityThermalPartSpec> Parts = { Eng };
	FEntityThermalState St;
	FEntityThermalInputs Step = CamSimEntityThermal::InputsFromLatch(Later, 10.0);
	Step.Cmd.bEngineOn = true;
	CamSimEntityThermal::Step(St, Step, FEntityThermalSettings(), Parts);
	TestNearlyEqual(TEXT("engine excess = delta_k, not T_air + 45 against B = 0"), St.PartExcessK[0], 45.0f, 1e-3f);
	return true;
}
