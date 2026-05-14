// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "CIGI/CigiPacketTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Config/CamSimConfig.h"

// Audit test bed for the CIGI → camera wiring: ViewControl / ArtPart →
// UCamSimGimbalComponent, SensorControl → UCamSimSensorComponent, and the
// FOV helper on FCigiViewDefinition. Exercises the per-packet Apply* hooks
// rather than the queue-drain shell so the tests don't need an FCigiReceiver
// or a UWorld.

namespace
{
constexpr auto kAutomationFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

UCamSimGimbalComponent* NewGimbal()
{
	return NewObject<UCamSimGimbalComponent>(GetTransientPackage());
}

UCamSimSensorComponent* NewSensor()
{
	return NewObject<UCamSimSensorComponent>(GetTransientPackage());
}

USceneCaptureComponent2D* NewSceneCapture()
{
	return NewObject<USceneCaptureComponent2D>(GetTransientPackage());
}
} // namespace

// -----------------------------------------------------------------------------
// FCigiViewDefinition — HFOV / VFOV helpers
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimViewDefHFovTest,
	"CamSim.Camera.ViewDef.HFovAndVFovDerivedFromBounds",
	kAutomationFlags)

bool FCamSimViewDefHFovTest::RunTest(const FString& /*Parameters*/)
{
	FCigiViewDefinition View;
	View.FovLeft   = -30.0f;
	View.FovRight  =  30.0f;
	View.FovTop    =  17.0f;
	View.FovBottom = -17.0f;
	TestEqual(TEXT("symmetric 60° HFOV"), View.HFovDeg(), 60.0f);
	TestEqual(TEXT("symmetric 34° VFOV"), View.VFovDeg(), 34.0f);

	// Asymmetric / offset frustum — HFovDeg is total width
	View.FovLeft  = -10.0f;
	View.FovRight =  50.0f;
	TestEqual(TEXT("asymmetric 60° HFOV"), View.HFovDeg(), 60.0f);
	return true;
}

// -----------------------------------------------------------------------------
// UCamSimGimbalComponent — boresight default
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimGimbalBoresightTest,
	"CamSim.Camera.Gimbal.DefaultBoresightIsNadir",
	kAutomationFlags)

bool FCamSimGimbalBoresightTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimGimbalComponent* Gimbal = NewGimbal();
	TestNotNull(TEXT("gimbal allocated"), Gimbal);
	TestEqual(TEXT("default yaw = 0"),   Gimbal->GetGimbalYaw(),   0.0f);
	TestEqual(TEXT("default pitch = -90 (nadir)"), Gimbal->GetGimbalPitch(), -90.0f);
	TestEqual(TEXT("default roll = 0"),  Gimbal->GetGimbalRoll(),  0.0f);

	// FRotator components are doubles under UE5 LWC — feed double literals so the
	// TestEqual overload picks resolves unambiguously.
	const FRotator Rot = Gimbal->GetGimbalRelativeRotation();
	TestEqual(TEXT("FRotator.Pitch = gimbal pitch"), Rot.Pitch, -90.0);
	TestEqual(TEXT("FRotator.Yaw   = gimbal yaw"),   Rot.Yaw,    0.0);
	TestEqual(TEXT("FRotator.Roll  = gimbal roll"),  Rot.Roll,   0.0);
	return true;
}

// -----------------------------------------------------------------------------
// UCamSimGimbalComponent::ApplyViewControl — enable flags + clamping
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimGimbalViewCtrlEnableFlagsTest,
	"CamSim.Camera.Gimbal.ViewControl_EnableFlagsRespected",
	kAutomationFlags)

bool FCamSimGimbalViewCtrlEnableFlagsTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimGimbalComponent* Gimbal = NewGimbal();
	FCamSimConfig Cfg;

	FCigiViewControl Pkt;
	Pkt.Yaw = 45.0f; Pkt.Pitch = -10.0f; Pkt.Roll = 5.0f;
	Pkt.bYawEn = true; Pkt.bPitchEn = false; Pkt.bRollEn = false;

	Gimbal->ApplyViewControl(Pkt, Cfg);
	TestEqual(TEXT("yaw enable applied"),      Gimbal->GetGimbalYaw(),   45.0f);
	TestEqual(TEXT("pitch disable preserved"), Gimbal->GetGimbalPitch(), -90.0f);  // boresight default
	TestEqual(TEXT("roll disable preserved"),  Gimbal->GetGimbalRoll(),   0.0f);

	// Flip enables — now pitch and roll move, yaw stays at last value
	Pkt.bYawEn = false; Pkt.bPitchEn = true; Pkt.bRollEn = true;
	Gimbal->ApplyViewControl(Pkt, Cfg);
	TestEqual(TEXT("yaw stays after disable"), Gimbal->GetGimbalYaw(),   45.0f);
	TestEqual(TEXT("pitch enable applied"),    Gimbal->GetGimbalPitch(), -10.0f);
	TestEqual(TEXT("roll enable applied"),     Gimbal->GetGimbalRoll(),   5.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimGimbalViewCtrlClampTest,
	"CamSim.Camera.Gimbal.ViewControl_ClampsToAxisLimits",
	kAutomationFlags)

bool FCamSimGimbalViewCtrlClampTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimGimbalComponent* Gimbal = NewGimbal();
	FCamSimConfig Cfg;
	Cfg.GimbalPitchMin = -45.0f;
	Cfg.GimbalPitchMax =  10.0f;
	Cfg.GimbalYawMin   = -90.0f;
	Cfg.GimbalYawMax   =  90.0f;

	// Send values well outside the envelope — both axes should clamp
	FCigiViewControl Pkt;
	Pkt.bYawEn = true;   Pkt.Yaw   = 200.0f;
	Pkt.bPitchEn = true; Pkt.Pitch = -120.0f;
	Pkt.bRollEn = true;  Pkt.Roll  =  0.0f;

	Gimbal->ApplyViewControl(Pkt, Cfg);
	TestEqual(TEXT("yaw clamped to YawMax"),     Gimbal->GetGimbalYaw(),   90.0f);
	TestEqual(TEXT("pitch clamped to PitchMin"), Gimbal->GetGimbalPitch(), -45.0f);

	// Hit the opposite limits
	Pkt.Yaw = -300.0f; Pkt.Pitch = 90.0f;
	Gimbal->ApplyViewControl(Pkt, Cfg);
	TestEqual(TEXT("yaw clamped to YawMin"),     Gimbal->GetGimbalYaw(),   -90.0f);
	TestEqual(TEXT("pitch clamped to PitchMax"), Gimbal->GetGimbalPitch(),  10.0f);
	return true;
}

// -----------------------------------------------------------------------------
// UCamSimGimbalComponent::ApplyArtPart — slew rate + axis limits
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimGimbalArtPartSlewRateTest,
	"CamSim.Camera.Gimbal.ArtPart_SlewRateLimited",
	kAutomationFlags)

bool FCamSimGimbalArtPartSlewRateTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimGimbalComponent* Gimbal = NewGimbal();
	FCamSimConfig Cfg;
	Cfg.GimbalMaxSlewRateDegPerSec = 60.0f;  // 1° per 0.01666 s
	Cfg.GimbalPitchMin = -180.0f; Cfg.GimbalPitchMax = 180.0f;
	Cfg.GimbalYawMin   = -180.0f; Cfg.GimbalYawMax   = 180.0f;

	// Slew toward yaw=60 from yaw=0 over a 0.5 s tick → should advance by exactly 30°
	FCigiArtPartControl Pkt;
	Pkt.bArtPartEn = true;
	Pkt.bYawEn = true; Pkt.Yaw = 60.0f;
	Pkt.bPitchEn = true; Pkt.Pitch = -90.0f;  // already at boresight; should not move

	// Reset roll baseline so test is hermetic
	FCigiViewControl Reset;
	Reset.bYawEn = true; Reset.Yaw = 0.0f;
	Reset.bPitchEn = true; Reset.Pitch = -90.0f;
	Reset.bRollEn = true; Reset.Roll = 0.0f;
	Gimbal->ApplyViewControl(Reset, Cfg);

	Gimbal->ApplyArtPart(Pkt, /*DeltaTime=*/0.5f, Cfg);
	TestEqual(TEXT("yaw slewed by 30° (60°/s × 0.5s)"), Gimbal->GetGimbalYaw(), 30.0f);

	// Second 0.5s tick reaches the target exactly
	Gimbal->ApplyArtPart(Pkt, 0.5f, Cfg);
	TestEqual(TEXT("yaw reached target on 2nd tick"), Gimbal->GetGimbalYaw(), 60.0f);

	// MaxRate <= 0 means snap instantly
	Cfg.GimbalMaxSlewRateDegPerSec = 0.0f;
	Pkt.Yaw = 120.0f;
	Gimbal->ApplyArtPart(Pkt, 0.0001f, Cfg);
	TestEqual(TEXT("rate=0 snaps to target"), Gimbal->GetGimbalYaw(), 120.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimGimbalArtPartClampTest,
	"CamSim.Camera.Gimbal.ArtPart_ClampsToAxisLimits",
	kAutomationFlags)

bool FCamSimGimbalArtPartClampTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimGimbalComponent* Gimbal = NewGimbal();
	FCamSimConfig Cfg;
	Cfg.GimbalMaxSlewRateDegPerSec = 0.0f;     // snap so we can read the clamp directly
	Cfg.GimbalPitchMin = -45.0f;
	Cfg.GimbalPitchMax =  10.0f;
	Cfg.GimbalYawMin   = -90.0f;
	Cfg.GimbalYawMax   =  90.0f;

	FCigiArtPartControl Pkt;
	Pkt.bArtPartEn = true;
	Pkt.bYawEn = true;   Pkt.Yaw   = 250.0f;
	Pkt.bPitchEn = true; Pkt.Pitch = -120.0f;

	Gimbal->ApplyArtPart(Pkt, 0.001f, Cfg);
	TestEqual(TEXT("ArtPart yaw clamped to YawMax"),     Gimbal->GetGimbalYaw(),    90.0f);
	TestEqual(TEXT("ArtPart pitch clamped to PitchMin"), Gimbal->GetGimbalPitch(), -45.0f);

	// bArtPartEn = false → no-op
	Pkt.bArtPartEn = false;
	Pkt.Yaw = 0.0f; Pkt.Pitch = 0.0f;
	Gimbal->ApplyArtPart(Pkt, 0.001f, Cfg);
	TestEqual(TEXT("disabled ArtPart leaves yaw"),   Gimbal->GetGimbalYaw(),    90.0f);
	TestEqual(TEXT("disabled ArtPart leaves pitch"), Gimbal->GetGimbalPitch(), -45.0f);
	return true;
}

// -----------------------------------------------------------------------------
// UCamSimSensorComponent::ApplySensorControl — FOV preset selection by Gain
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimSensorGainToFovTest,
	"CamSim.Camera.Sensor.GainSelectsFovPreset",
	kAutomationFlags)

bool FCamSimSensorGainToFovTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimSensorComponent*    Sensor    = NewSensor();
	USceneCaptureComponent2D*  Capture   = NewSceneCapture();
	FCamSimConfig Cfg;
	Cfg.SensorFovPresets = { 60.0f, 30.0f, 10.0f, 2.0f };  // wide → narrow

	FCigiSensorControl Pkt;
	Pkt.bSensorOn = true;
	Pkt.SensorId  = 0; // EO
	Pkt.Gain      = 0.0f;
	TestEqual(TEXT("gain=0 picks widest preset"),
		Sensor->ApplySensorControl(Pkt, Cfg, Capture), 60.0f);
	TestEqual(TEXT("capture FOV updated"), Capture->FOVAngle, 60.0f);

	Pkt.Gain = 0.5f;
	TestEqual(TEXT("gain=0.5 picks preset[2]=10°"),
		Sensor->ApplySensorControl(Pkt, Cfg, Capture), 10.0f);

	Pkt.Gain = 1.0f;
	TestEqual(TEXT("gain=1 picks narrowest preset (safe at boundary)"),
		Sensor->ApplySensorControl(Pkt, Cfg, Capture), 2.0f);

	// Out-of-range gain clamps inside [0, N-1]
	Pkt.Gain = 2.0f;
	TestEqual(TEXT("gain>1 clamps to narrowest"),
		Sensor->ApplySensorControl(Pkt, Cfg, Capture), 2.0f);

	// Sanity: polarity / on-off / mode bookkeeping moves too
	Pkt.bSensorOn = false; Pkt.Polarity = 1; Pkt.SensorId = 1; Pkt.Gain = 0.0f;
	Sensor->ApplySensorControl(Pkt, Cfg, Capture);
	TestFalse(TEXT("sensor off"), Sensor->IsOn());
	TestEqual(TEXT("polarity=BlackHot"), (int)Sensor->GetPolarity(), 1);
	TestEqual(TEXT("mode=IR (sensor id 1)"), (int)Sensor->GetMode(), 1);

	// Unknown sensor IDs clamp to EO (0)
	Pkt.SensorId = 99;
	Sensor->ApplySensorControl(Pkt, Cfg, Capture);
	TestEqual(TEXT("unknown sensor ID clamps to mode<=2"), (int)Sensor->GetMode() <= 2, 1);
	return true;
}

// -----------------------------------------------------------------------------
// Empty preset table — sensor should leave FOV untouched
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamSimSensorEmptyPresetsTest,
	"CamSim.Camera.Sensor.EmptyPresetsLeavesFovUntouched",
	kAutomationFlags)

bool FCamSimSensorEmptyPresetsTest::RunTest(const FString& /*Parameters*/)
{
	UCamSimSensorComponent*    Sensor    = NewSensor();
	USceneCaptureComponent2D*  Capture   = NewSceneCapture();
	Capture->FOVAngle = 42.0f;

	FCamSimConfig Cfg;
	Cfg.SensorFovPresets.Reset();

	FCigiSensorControl Pkt;
	Pkt.bSensorOn = true;
	Pkt.Gain      = 0.7f;
	Sensor->ApplySensorControl(Pkt, Cfg, Capture);
	TestEqual(TEXT("empty presets → FOV unchanged"), Capture->FOVAngle, 42.0f);
	return true;
}
