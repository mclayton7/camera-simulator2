// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "Camera/CamSimCamera.h"
#include "Camera/CamSimPlatformRig.h"
#include "Control/ControlApiJson.h"
#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "Config/CamSimConfig.h"
#include "Environment/CamSimEnvironment.h"
#include "Sim/Commands.h"

// -------------------------------------------------------------------------
// Canonical consumers for the control API (ROADMAP 2.3/2.4): FViewCommand,
// FSensorCommand, FAtmosphereCommand, FWeatherCommand reach the camera and
// environment through the same internals as CIGI.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersGimbalTest,
	"CamSim.ControlApi.Consumers.GimbalSnapAndSlew",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersGimbalTest::RunTest(const FString& Parameters)
{
	UCamSimGimbalComponent* G = NewObject<UCamSimGimbalComponent>(GetTransientPackage());
	FCamSimConfig Cfg;
	Cfg.GimbalPitchMin = -110.0f;
	Cfg.GimbalMaxSlewRateDegPerSec = 10.0f;

	FViewCommand Snap;
	Snap.Gimbal = FViewCommand::EGimbal::Snap;
	Snap.bYawEn = Snap.bPitchEn = true;
	Snap.Yaw = 270.0f;
	Snap.Pitch = -120.0f;
	G->ApplyView(Snap, 0.0f, Cfg);
	TestEqual(TEXT("snap: yaw unwound"), G->GetGimbalYaw(), -90.0f);
	TestEqual(TEXT("snap: pitch clamped to the envelope"), G->GetGimbalPitch(), -110.0f);

	FViewCommand Slew;
	Slew.Gimbal = FViewCommand::EGimbal::Slew;
	Slew.bYawEn = true;
	Slew.Yaw = 0.0f;
	G->ApplyView(Slew, 1.0f, Cfg);
	TestEqual(TEXT("slew: 10 deg/s for 1 s"), G->GetGimbalYaw(), -80.0f);
	TestEqual(TEXT("slew: pitch untouched"), G->GetGimbalPitch(), -110.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersSensorTest,
	"CamSim.ControlApi.Consumers.SensorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersSensorTest::RunTest(const FString& Parameters)
{
	UCamSimSensorComponent* S = NewObject<UCamSimSensorComponent>(GetTransientPackage());
	FSensorCommand Cmd;
	Cmd.bOn = true;
	Cmd.Waveband = ESensorMode::IR;
	Cmd.Polarity = 1;
	S->ApplySensor(Cmd);
	TestEqual(TEXT("IR"), S->GetMode(), ESensorMode::IR);
	TestEqual(TEXT("black hot"), S->GetPolarity(), static_cast<uint8>(1));
	TestTrue(TEXT("on"), S->IsOn());

	Cmd.bOn = false;
	Cmd.Waveband = ESensorMode::EO;
	S->ApplySensor(Cmd);
	TestEqual(TEXT("back to EO"), S->GetMode(), ESensorMode::EO);
	TestFalse(TEXT("off"), S->IsOn());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersAtmosphereTest,
	"CamSim.ControlApi.Consumers.AtmosphereFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersAtmosphereTest::RunTest(const FString& Parameters)
{
	FAtmosphereCommand A;
	A.bEnabled = false;
	A.HumidityPct = 41.0f;
	A.AirTempC = 23.0f;
	A.VisibilityM = 12000.0f;
	A.HorizWindMps = 4.5f;
	A.VertWindMps = 0.5f;
	A.WindDirDeg = 270.0f;
	A.BaroPressureMb = 1001.0f;
	const FCigiAtmosphereState S = ACamSimEnvironment::ToAtmosphereState(A);
	TestFalse(TEXT("enabled"), S.bAtmosEn);
	TestEqual(TEXT("humidity"), S.Humidity, 41.0f);
	TestEqual(TEXT("air temp"), S.AirTemp, 23.0f);
	TestEqual(TEXT("visibility"), S.Visibility, 12000.0f);
	TestEqual(TEXT("horiz wind"), S.HorizWindSp, 4.5f);
	TestEqual(TEXT("vert wind"), S.VertWindSp, 0.5f);
	TestEqual(TEXT("wind dir"), S.WindDir, 270.0f);
	TestEqual(TEXT("baro"), S.BaroPress, 1001.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersWeatherTest,
	"CamSim.ControlApi.Consumers.WeatherFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersWeatherTest::RunTest(const FString& Parameters)
{
	FWeatherCommand W;
	W.Scope = FWeatherCommand::EScope::Global;
	W.RegionId = 0;
	W.LayerId = 1;
	W.bEnabled = true;
	W.Severity = 2;
	W.CloudType = 3;
	W.CoveragePct = 40.0f;
	W.BaseElevM = 2500.0f;
	W.ThicknessM = 600.0f;
	W.TransitionM = 300.0f;
	W.VisibilityM = 8000.0f;
	W.HorizWindMps = 6.0f;
	W.VertWindMps = 1.0f;
	W.WindDirDeg = 90.0f;
	const FCigiWeatherState S = ACamSimEnvironment::ToWeatherState(W);
	TestEqual(TEXT("scope global"), S.Scope, static_cast<uint8>(0));
	TestEqual(TEXT("region"), S.RegionId, static_cast<uint16>(0));
	TestEqual(TEXT("layer"), S.LayerId, static_cast<uint8>(1));
	TestTrue(TEXT("enabled"), S.bWeatherEn);
	TestEqual(TEXT("severity"), S.Severity, static_cast<uint8>(2));
	TestEqual(TEXT("cloud type"), S.CloudType, static_cast<uint8>(3));
	TestEqual(TEXT("coverage"), S.Coverage, 40.0f);
	TestEqual(TEXT("base"), S.BaseElev, 2500.0f);
	TestEqual(TEXT("thickness"), S.Thickness, 600.0f);
	TestEqual(TEXT("transition"), S.Transition, 300.0f);
	TestEqual(TEXT("visibility"), S.VisibilityRng, 8000.0f);
	TestEqual(TEXT("horiz wind"), S.HorizWindSp, 6.0f);
	TestEqual(TEXT("vert wind"), S.VertWindSp, 1.0f);
	TestEqual(TEXT("wind dir"), S.WindDir, 90.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersHFovTest,
	"CamSim.ControlApi.Consumers.HFovClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersHFovTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("low"), ACamSimCamera::ClampHFov(0.2f), 1.0f);
	TestEqual(TEXT("high"), ACamSimCamera::ClampHFov(200.0f), 179.0f);
	TestEqual(TEXT("in range"), ACamSimCamera::ClampHFov(20.0f), 20.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersFpsTest,
	"CamSim.ControlApi.Consumers.PlatformPoseEndsFirstPersonView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersFpsTest::RunTest(const FString& Parameters)
{
	FCamSimPlatformRig Rig;   // not initialized: state only, no globe anchor
	Rig.UpdateFirstPersonView(7);
	TestEqual(TEXT("CIGI View Control rides entity 7"), Rig.GetFirstPersonEntityId(), static_cast<uint16>(7));
	CamSimFrames::FGeoPose Pose;
	Pose.Lat = 33.0; Pose.Lon = -117.0; Pose.Alt = 500.0;
	Rig.SetPlatformPose(Pose, 0.0);
	TestEqual(TEXT("an API platform pose ends the first-person view"), Rig.GetFirstPersonEntityId(), static_cast<uint16>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlConsumersSensorMergeTest,
	"CamSim.ControlApi.Consumers.PartialSensorKeepsCurrentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlConsumersSensorMergeTest::RunTest(const FString& Parameters)
{
	FControlSensorRequest Polarity;
	Polarity.Polarity = 1;
	const FSensorCommand A = CamSimControl::MergeSensor(Polarity, true, ESensorMode::IR, 0);
	TestEqual(TEXT("polarity only: stays IR"), A.Waveband, ESensorMode::IR);
	TestEqual(TEXT("black hot"), A.Polarity, static_cast<uint8>(1));
	TestTrue(TEXT("stays on"), A.bOn);
	FControlSensorRequest Mode;
	Mode.Mode = ESensorMode::EO;
	const FSensorCommand B = CamSimControl::MergeSensor(Mode, false, ESensorMode::IR, 1);
	TestEqual(TEXT("mode only: EO"), B.Waveband, ESensorMode::EO);
	TestEqual(TEXT("polarity kept"), B.Polarity, static_cast<uint8>(1));
	TestFalse(TEXT("off kept"), B.bOn);
	return true;
}
