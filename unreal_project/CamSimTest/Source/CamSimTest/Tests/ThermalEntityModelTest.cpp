// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/EntityThermal.h"

// CamSim.Thermal.Entity.Model.* / SpeedTracker.*: the pure 4C model (spec §1-2).

namespace
{
	const FEntityThermalSettings Defaults;

	TArray<FEntityThermalPartSpec> TruckParts()
	{
		FEntityThermalPartSpec Gear; Gear.Kind = EEntityThermalPartKind::RunningGear;
		FEntityThermalPartSpec Eng;  Eng.Kind  = EEntityThermalPartKind::Engine;
		FEntityThermalPartSpec Exh;  Exh.Kind  = EEntityThermalPartKind::Exhaust;
		return { Gear, Eng, Exh };
	}

	/** T_air 280 K, B 285 K: D = -5 K. */
	FEntityThermalInputs Env(double SimSec, float Tair = 280.0f, float B = 285.0f)
	{
		FEntityThermalInputs In;
		In.SimSec = SimSec; In.bHasEnv = true; In.TairK = Tair; In.BaselineK = B;
		return In;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalTargetsTest, "CamSim.Thermal.Entity.Model.Targets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalTargetsTest::RunTest(const FString& Parameters)
{
	const FEntityThermalSettings& S = Defaults;
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	const FEntityThermalState St;
	float Skin = 1.0f, Parts[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	// Parked, engine off, no environment: everything 0 excess (4A)
	FEntityThermalInputs Off; Off.SimSec = 10.0;
	CamSimEntityThermal::Targets(St, Off, S, P, Skin, Parts);
	TestEqual(TEXT("off skin"), Skin, 0.0f);
	for (int32 K = 0; K < 4; ++K) TestEqual(*FString::Printf(TEXT("off part %d"), K), Parts[K], 0.0f);
	// Engine on, parked, D = -5: skin = run = 4; gear = D = -5; engine = D + 45 = 40; exhaust = 450 - 285 = 165
	FEntityThermalInputs On = Env(10.0); On.Cmd.bEngineOn = true;
	CamSimEntityThermal::Targets(St, On, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("running skin"), Skin, 4.0f, 1e-4f);
	TestNearlyEqual(TEXT("gear parked"), Parts[0], -5.0f, 1e-4f);
	TestNearlyEqual(TEXT("engine"), Parts[1], 40.0f, 1e-4f);
	TestNearlyEqual(TEXT("exhaust"), Parts[2], 165.0f, 1e-4f);
	TestEqual(TEXT("unused slot"), Parts[3], 0.0f);
	// Moving at 10 m/s: c = 0.5; skin = -5 * 0.5 + 4 * 0.5 = -0.5; gear = -5 + min(15, 30) = 10
	FEntityThermalInputs Mv = Env(10.0); Mv.SpeedMps = 10.0f;
	CamSimEntityThermal::Targets(St, Mv, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("convection skin"), Skin, -0.5f, 1e-4f);
	TestNearlyEqual(TEXT("gear moving"), Parts[0], 10.0f, 1e-4f);
	// Cap: 40 m/s -> min(60, 30) = 30 -> 25
	Mv.SpeedMps = 40.0f;
	CamSimEntityThermal::Targets(St, Mv, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("gear capped"), Parts[0], 25.0f, 1e-4f);
	// Part overrides
	TArray<FEntityThermalPartSpec> O = P; O[1].DeltaK = 25.0f; O[2].TempK = 400.0f;
	CamSimEntityThermal::Targets(St, On, S, O, Skin, Parts);
	TestNearlyEqual(TEXT("engine override"), Parts[1], 20.0f, 1e-4f);
	TestNearlyEqual(TEXT("exhaust override"), Parts[2], 115.0f, 1e-4f);
	// No environment, running: exhaust against B = 288.15
	FEntityThermalInputs NoEnv; NoEnv.SimSec = 10.0; NoEnv.Cmd.bEngineOn = true;
	CamSimEntityThermal::Targets(St, NoEnv, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("no-env exhaust"), Parts[2], 450.0f - 288.15f, 1e-3f);
	TestNearlyEqual(TEXT("no-env engine"), Parts[1], 45.0f, 1e-4f);
	TestNearlyEqual(TEXT("c(0) = 1"), CamSimEntityThermal::ConvectionFactor(0.0f, 10.0f), 1.0f, 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalRunningTest, "CamSim.Thermal.Entity.Model.RunningLogic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalRunningTest::RunTest(const FString& Parameters)
{
	const FEntityThermalSettings& S = Defaults;
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	FEntityThermalInputs In = Env(0.0);
	TestFalse(TEXT("parked off"), CamSimEntityThermal::IsRunning(St, In, S));
	In.Cmd.bEngineOn = true;
	TestTrue(TEXT("commanded on"), CamSimEntityThermal::IsRunning(St, In, S));
	In.Cmd.bEngineOn = false; In.SpeedMps = 3.0f;
	TestTrue(TEXT("explicit off while moving still runs"), CamSimEntityThermal::IsRunning(St, In, S));
	CamSimEntityThermal::Step(St, In, S, P);   // moving at t = 0
	FEntityThermalInputs Stop = Env(100.0);
	TestTrue(TEXT("idle hold at 100 s"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.SimSec = 121.0;
	TestFalse(TEXT("hold expired at 121 s"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.Cmd.bEngineOn = true; Stop.Cmd.Damage = 2;
	TestFalse(TEXT("destroyed never runs"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.Cmd.Damage = 1;
	TestTrue(TEXT("damaged still runs"), CamSimEntityThermal::IsRunning(St, Stop, S));
	Stop.Cmd.Damage = 0; Stop.Cmd.bFlaming = true;
	TestFalse(TEXT("flaming never runs"), CamSimEntityThermal::IsRunning(St, Stop, S));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalLagTest, "CamSim.Thermal.Entity.Model.Lag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalLagTest::RunTest(const FString& Parameters)
{
	const FEntityThermalSettings& S = Defaults;
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	// Spawn parked, off: at the target (0)
	FEntityThermalState St;
	CamSimEntityThermal::Step(St, Env(0.0), S, P);
	TestTrue(TEXT("initialised"), St.bInitialized);
	TestEqual(TEXT("spawn exhaust"), St.PartExcessK[2], 0.0f);
	// Engine on: the exhaust rises with tau_up 20 s; after 20 s at 30 Hz: 165 (1 - 1/e)
	FEntityThermalInputs On = Env(0.0); On.Cmd.bEngineOn = true;
	for (int32 I = 1; I <= 600; ++I) { On.SimSec = I / 30.0; CamSimEntityThermal::Step(St, On, S, P); }
	TestNearlyEqual(TEXT("exhaust after 1 tau"), St.PartExcessK[2], 165.0f * (1.0f - FMath::Exp(-1.0f)), 0.05f);
	// One 20 s step gives the same (exact exponential)
	FEntityThermalState St2;
	CamSimEntityThermal::Step(St2, Env(0.0), S, P);
	On.SimSec = 20.0;
	CamSimEntityThermal::Step(St2, On, S, P);
	TestNearlyEqual(TEXT("step-size independent"), St2.PartExcessK[2], St.PartExcessK[2], 0.05f);
	// Spawn running: at the target
	FEntityThermalState W;
	CamSimEntityThermal::Step(W, On, S, P);
	TestNearlyEqual(TEXT("spawn running at target"), W.PartExcessK[2], 165.0f, 1e-3f);
	// Engine off (never moved): falls with tau_down 60 s
	FEntityThermalInputs Off = Env(20.0 + 60.0);
	CamSimEntityThermal::Step(W, Off, S, P);
	TestNearlyEqual(TEXT("exhaust after 1 tau down"), W.PartExcessK[2], 165.0f * FMath::Exp(-1.0f), 0.05f);
	TestNearlyEqual(TEXT("engine after 60 s of tau_down 900"), W.PartExcessK[1], 40.0f * FMath::Exp(-60.0f / 900.0f), 0.05f);
	// dt == 0: no change
	const float Before = W.PartExcessK[2];
	CamSimEntityThermal::Step(W, Off, S, P);
	TestEqual(TEXT("frozen clock"), W.PartExcessK[2], Before);
	// Negative dt snaps to the target (0)
	Off.SimSec = 10.0;
	CamSimEntityThermal::Step(W, Off, S, P);
	TestEqual(TEXT("rewind snaps"), W.PartExcessK[2], 0.0f);
	// A jump > 1 h snaps
	FEntityThermalInputs Far = On; Far.SimSec = 10.0 + 3601.0;
	CamSimEntityThermal::Step(W, Far, S, P);
	TestNearlyEqual(TEXT("jump snaps to the running target"), W.PartExcessK[2], 165.0f, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalBurnTest, "CamSim.Thermal.Entity.Model.Burn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalBurnTest::RunTest(const FString& Parameters)
{
	const FEntityThermalSettings& S = Defaults;
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	CamSimEntityThermal::Step(St, Env(0.0), S, P);
	FEntityThermalInputs D = Env(1.0); D.Cmd.Damage = 2;
	CamSimEntityThermal::Step(St, D, S, P);
	float Skin = 0.0f, Parts[4] = {};
	CamSimEntityThermal::Targets(St, D, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("burn skin target = 700 - B"), Skin, 415.0f, 1e-3f);
	TestNearlyEqual(TEXT("burn part target"), Parts[1], 415.0f, 1e-3f);
	// Burning from t = 0..300 (steps at 1..300), then step 301 is the first after burn_s: target 0, hull tau 1800 s
	for (int32 I = 2; I <= 301; ++I) { D.SimSec = I; CamSimEntityThermal::Step(St, D, S, P); }
	const float Peak = St.SkinExcessK;
	TestNearlyEqual(TEXT("skin after the burn"), Peak, 415.0f * (1.0f - FMath::Exp(-0.5f)) * FMath::Exp(-1.0f / 1800.0f), 0.05f);
	TestTrue(TEXT("burn done, hull cooling"), St.bBurnDone && St.bHullCooling);
	TestFalse(TEXT("destroyed: not burning after burn_s"), CamSimEntityThermal::IsBurning(St, D, S));
	D.SimSec = 301.0 + 1800.0;
	CamSimEntityThermal::Step(St, D, S, P);
	TestNearlyEqual(TEXT("hull cools with 1800 s"), St.SkinExcessK, Peak * FMath::Exp(-1.0f), 0.05f);
	// Repair: leaving destroyed resets the burn state
	FEntityThermalInputs Fix = Env(D.SimSec + 1.0);
	CamSimEntityThermal::Step(St, Fix, S, P);
	TestTrue(TEXT("burn reset"), St.BurnStartSec < 0.0 && !St.bBurnDone && !St.bHullCooling);
	// Flaming alone burns while set (no burn_s limit)
	FEntityThermalInputs Fl = Env(Fix.SimSec + 1.0); Fl.Cmd.bFlaming = true;
	CamSimEntityThermal::Step(St, Fl, S, P);
	Fl.SimSec += 1000.0;
	TestTrue(TEXT("flaming burns past burn_s"), CamSimEntityThermal::IsBurning(St, Fl, S));
	CamSimEntityThermal::Targets(St, Fl, S, P, Skin, Parts);
	TestNearlyEqual(TEXT("flaming target"), Skin, 415.0f, 1e-3f);
	CamSimEntityThermal::Step(St, Fl, S, P);
	// Flaming cleared: cooling follows the hull constant
	Fl.Cmd.bFlaming = false; Fl.SimSec += 1.0;
	CamSimEntityThermal::Step(St, Fl, S, P);
	TestTrue(TEXT("flaming cleared: hull cooling"), St.bHullCooling);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalNoEnvTest, "CamSim.Thermal.Entity.Model.NoEnvironmentStillLags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalNoEnvTest::RunTest(const FString& Parameters)
{
	// Review focus 4: driving with no environment (EO only) still warms the state, from a cold spawn.
	const FEntityThermalSettings& S = Defaults;
	const TArray<FEntityThermalPartSpec> P = TruckParts();
	FEntityThermalState St;
	FEntityThermalInputs In; In.SimSec = 0.0;   // parked at spawn
	CamSimEntityThermal::Step(St, In, S, P);
	TestEqual(TEXT("cold at spawn"), St.PartExcessK[2], 0.0f);
	In.SpeedMps = 8.0f;
	for (int32 I = 1; I <= 600; ++I) { In.SimSec = I; CamSimEntityThermal::Step(St, In, S, P); }
	TestTrue(*FString::Printf(TEXT("engine warm (%.1f K)"), St.PartExcessK[1]), St.PartExcessK[1] > 30.0f);
	TestTrue(*FString::Printf(TEXT("exhaust hot (%.1f K)"), St.PartExcessK[2]), St.PartExcessK[2] > 150.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalComponentTest, "CamSim.Thermal.Entity.Model.ComponentCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalComponentTest::RunTest(const FString& Parameters)
{
	FEntityThermalCommanded C;
	TestTrue(TEXT("11 handled"), CamSimEntityThermal::ApplyComponent(C, 11, 1));
	TestTrue(TEXT("engine on"), C.bEngineOn);
	CamSimEntityThermal::ApplyComponent(C, 11, 0);
	TestFalse(TEXT("engine off"), C.bEngineOn);
	TestTrue(TEXT("12 handled"), CamSimEntityThermal::ApplyComponent(C, 12, 1));
	TestTrue(TEXT("flaming"), C.bFlaming);
	TestTrue(TEXT("10 handled"), CamSimEntityThermal::ApplyComponent(C, 10, 2));
	TestEqual(TEXT("destroyed"), C.Damage, static_cast<uint8>(2));
	CamSimEntityThermal::ApplyComponent(C, 10, 7);
	TestEqual(TEXT("damage clamped"), C.Damage, static_cast<uint8>(2));
	TestFalse(TEXT("lights are not thermal"), CamSimEntityThermal::ApplyComponent(C, 0, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntitySpeedTrackerTest, "CamSim.Thermal.Entity.SpeedTracker.EmaAndTeleport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntitySpeedTrackerTest::RunTest(const FString& Parameters)
{
	FEntitySpeedTracker T;
	const FVector P0(6378137.0, 0.0, 0.0);
	TestEqual(TEXT("first sample 0"), T.Update(P0, 0.0), 0.0f);
	// 10 m/s along Y for 5 s at 30 Hz
	for (int32 I = 1; I <= 150; ++I) T.Update(P0 + FVector(0.0, 10.0 * I / 30.0, 0.0), I / 30.0);
	TestNearlyEqual(TEXT("converged"), T.GetSpeedMps(), 10.0f, 0.1f);
	// dt <= 0 keeps v
	T.Update(P0 + FVector(0.0, 1000.0, 0.0), 5.0);
	TestNearlyEqual(TEXT("dt 0 keeps"), T.GetSpeedMps(), 10.0f, 0.1f);
	// Teleport 5 km in one tick: reset to 0
	T.Update(P0 + FVector(0.0, 5000.0, 0.0), 5.0 + 1.0 / 30.0);
	TestEqual(TEXT("teleport resets"), T.GetSpeedMps(), 0.0f);
	// A fast aircraft (250 m/s) is not a teleport
	FEntitySpeedTracker A;
	for (int32 I = 0; I <= 150; ++I) A.Update(P0 + FVector(0.0, 250.0 * I / 30.0, 0.0), I / 30.0);
	TestNearlyEqual(TEXT("250 m/s tracked"), A.GetSpeedMps(), 250.0f, 3.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntitySpeedJitterTest, "CamSim.Thermal.Entity.SpeedTracker.JitterStaysParked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntitySpeedJitterTest::RunTest(const FString& Parameters)
{
	// Review focus 1: +-2 cm pose noise per axis at 30 Hz (surface clamp / DR corrections) stays below moving_mps (0.5).
	FEntitySpeedTracker T;
	const FVector P0(6378137.0, 0.0, 0.0);
	float Max = 0.0f;
	for (int32 I = 0; I <= 300; ++I)
	{
		const double J = static_cast<double>((I * 7919) % 5 - 2) * 0.01;   // deterministic -0.02..+0.02 m
		Max = FMath::Max(Max, T.Update(P0 + FVector(J, -J, J), I / 30.0));
	}
	TestTrue(*FString::Printf(TEXT("jitter speed %.3f < 0.5"), Max), Max < 0.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntitySpeedFirstTest, "CamSim.Thermal.Entity.SpeedTracker.FirstMeasurementSeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntitySpeedFirstTest::RunTest(const FString& Parameters)
{
	// A moving spawn: no measurement until the first window, which then reads the true speed (no ramp from 0).
	FEntitySpeedTracker T;
	const FVector P0(6378137.0, 0.0, 0.0);
	T.Update(P0, 0.0);
	TestFalse(TEXT("no measurement at the first sample"), T.HasMeasurement());
	for (int32 I = 1; I <= 14; ++I) T.Update(P0 + FVector(0.0, 15.0 * I / 30.0, 0.0), I / 30.0);
	TestFalse(TEXT("none before the window"), T.HasMeasurement());
	T.Update(P0 + FVector(0.0, 15.0 * 15 / 30.0, 0.0), 15 / 30.0);
	TestTrue(TEXT("measured after 0.5 s"), T.HasMeasurement());
	TestNearlyEqual(TEXT("first measurement = raw speed"), T.GetSpeedMps(), 15.0f, 0.01f);
	return true;
}
