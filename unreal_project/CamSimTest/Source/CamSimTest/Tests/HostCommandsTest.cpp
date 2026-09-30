// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Hosts/CigiCommands.h"
#include "Hosts/DisCommands.h"
#include "CIGI/CigiPacketTypes.h"
#include "DIS/DisPduTypes.h"
#include "Geospatial/EcefFrames.h"

// -------------------------------------------------------------------------
// Canonical command model (ROADMAP 2.3, phase 1): protocol → command
// converters keep every field the simulation consumes today.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostEntityKeysTest,
	"CamSim.Hosts.EntityKeysArePerSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHostEntityKeysTest::RunTest(const FString& Parameters)
{
	FDisEntityId DisId;
	DisId.Entity = 1000;
	const FEntityKey CigiKey = CamSim::Cigi::Key(1000);
	const FEntityKey DisKey  = CamSim::Dis::Key(DisId);
	TestNotEqual(TEXT("CIGI 1000 and DIS entity 1000 are different entities"), CigiKey, DisKey);

	TMap<FEntityKey, int32> Map;
	Map.Add(CigiKey, 1);
	Map.Add(DisKey, 2);
	TestEqual(TEXT("both keys coexist in a map"), Map.Num(), 2);

	FDisEntityId Other = DisId;
	Other.Site = 1;
	TestNotEqual(TEXT("DIS site is part of the key"), CamSim::Dis::Key(Other), DisKey);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostCigiEntityCommandsTest,
	"CamSim.Hosts.CigiEntityCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHostCigiEntityCommandsTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Cigi;

	FCigiEntityState S;
	S.EntityId = 7; S.EntityType = 1001; S.EntityState = 1;
	S.Latitude = 37.5; S.Longitude = -122.25; S.Altitude = 1500.0f;
	S.Yaw = 90.0f; S.Pitch = 10.0f; S.Roll = -5.0f;
	S.EntityKind = 1; S.EntityDomain = 2; S.EntityCategory = 3;
	S.HostTimeSec = 12.5;
	FEntityCommand C = ToEntityCommand(S);
	TestEqual(TEXT("key"), C.Key, Key(7));
	TestTrue(TEXT("active"), C.Lifecycle == EEntityLifecycle::Active);
	TestEqual(TEXT("type"), C.TypeId, static_cast<uint16>(1001));
	TestTrue(TEXT("classification"), C.Classification.Kind == 1 && C.Classification.Domain == 2 && C.Classification.Category == 3);
	TestTrue(TEXT("position"), C.Pose.Lat == 37.5 && C.Pose.Lon == -122.25 && C.Pose.Alt == 1500.0);
	const FRotator Hpr = C.Pose.Neu.Rotator();
	TestTrue(FString::Printf(TEXT("orientation %s"), *Hpr.ToString()),
		FMath::IsNearlyEqual(Hpr.Yaw, 90.0, 1e-4) && FMath::IsNearlyEqual(Hpr.Pitch, 10.0, 1e-4) && FMath::IsNearlyEqual(Hpr.Roll, -5.0, 1e-4));
	TestFalse(TEXT("not attached"), C.Attachment.IsSet());
	TestEqual(TEXT("source time"), C.SourceTimeSec, 12.5);

	S.EntityState = 0;
	TestTrue(TEXT("standby hides"), ToEntityCommand(S).Lifecycle == EEntityLifecycle::Hidden);
	S.EntityState = 2;
	TestTrue(TEXT("destroyed removes"), ToEntityCommand(S).Lifecycle == EEntityLifecycle::Remove);

	S.EntityState = 1; S.bAttached = true; S.ParentId = 3;
	S.Latitude = 12.5; S.Longitude = -3.0; S.Altitude = 2.0f;
	C = ToEntityCommand(S);
	if (TestTrue(TEXT("attached"), C.Attachment.IsSet()))
	{
		TestEqual(TEXT("parent key"), C.Attachment->Parent, Key(3));
		TestTrue(TEXT("body offsets, not lat/lon"), C.Attachment->OffsetFrd.Equals(FVector(12.5, -3.0, 2.0)));
		TestTrue(TEXT("relative rotation"), C.Attachment->Rotation.Equals(FRotator(10.0, 90.0, -5.0)));
	}

	FCigiConfClampEntityState Clamp;
	Clamp.EntityId = 9; Clamp.Latitude = 1.0; Clamp.Longitude = 2.0; Clamp.Yaw = 45.0f;
	const FEntityCommand Clamped = ToEntityCommand(Clamp);
	TestEqual(TEXT("conformal clamp key"), Clamped.Key, Key(9));
	TestTrue(TEXT("conformal clamp follows the ground"), Clamped.SurfaceMode == ESurfaceMode::Ground);
	TestEqual(TEXT("conformal clamp carries no type"), Clamped.TypeId, static_cast<uint16>(0));
	// An existing entity keeps its type when a command carries none (TypeId 0).
	TestEqual(TEXT("type 0 keeps the current type"), ResolveEntityTypeId(2001, Clamped), static_cast<uint16>(2001));
	TestEqual(TEXT("a real type replaces it"), ResolveEntityTypeId(2001, C), static_cast<uint16>(1001));

	FCigiRateControl Rate;
	Rate.EntityId = 7; Rate.bLocalFrame = false; Rate.bAngularLocalFrame = false;
	Rate.XRate = 5.0f; Rate.YawRate = 2.0f;
	const TOptional<FEntityMotionCommand> Motion = ToMotionCommand(Rate);
	if (TestTrue(TEXT("entity rate control"), Motion.IsSet()))
	{
		TestTrue(TEXT("world frame"), Motion->Motion.LinearFrame == FMotionModel::EFrame::World
			&& Motion->Motion.AngularFrame == FMotionModel::EFrame::World);
		TestTrue(TEXT("rates"), Motion->Motion.Velocity.Equals(FVector(5, 0, 0)) && Motion->Motion.AngularRate.Equals(FVector(0, 0, 2)));
	}
	Rate.bApplyToArtPart = true;
	TestFalse(TEXT("art-part rates are not entity motion"), ToMotionCommand(Rate).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostCigiViewCommandsTest,
	"CamSim.Hosts.CigiViewSensorTimeCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHostCigiViewCommandsTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Cigi;

	FCigiViewDefinition Def;
	Def.FovLeft = -20.0f; Def.FovRight = 20.0f; Def.FovTop = 11.0f; Def.FovBottom = -11.0f;
	const FViewCommand Fov = ToViewCommand(Def);
	TestTrue(TEXT("FOV"), Fov.HFovDeg.IsSet() && *Fov.HFovDeg == 40.0f && *Fov.VFovDeg == 22.0f);

	FCigiViewControl Vc;
	Vc.EntityId = 0; Vc.bYawEn = true; Vc.Yaw = 30.0f;
	FViewCommand V = ToViewCommand(Vc);
	TestTrue(TEXT("View Control snaps the gimbal"), V.Gimbal == FViewCommand::EGimbal::Snap && V.bYawEn && V.Yaw == 30.0f);
	TestTrue(TEXT("entity 0 ends the first-person view"), V.bClearEyeEntity && !V.EyeEntity.IsSet());
	Vc.EntityId = 44;
	V = ToViewCommand(Vc);
	TestTrue(TEXT("an entity starts the first-person view"), V.EyeEntity.IsSet() && *V.EyeEntity == Key(44) && !V.bClearEyeEntity);

	FCigiArtPartControl Art;
	Art.bArtPartEn = true; Art.bPitchEn = true; Art.Pitch = -45.0f;
	const FViewCommand Slew = ToGimbalSlewCommand(Art);
	TestTrue(TEXT("camera art part slews"), Slew.Gimbal == FViewCommand::EGimbal::Slew && Slew.bPitchEn && Slew.Pitch == -45.0f);
	Art.bArtPartEn = false;
	TestTrue(TEXT("disabled art part does nothing"), ToGimbalSlewCommand(Art).Gimbal == FViewCommand::EGimbal::None);

	FCigiSensorControl Sc;
	Sc.SensorId = 1; Sc.Polarity = 1; Sc.Gain = 1.5f; Sc.bSensorOn = false;
	const FSensorCommand Sensor = ToSensorCommand(Sc);
	TestTrue(TEXT("sensor 1 is IR, black hot, off"), Sensor.Waveband == ESensorMode::IR && Sensor.Polarity == 1 && !Sensor.bOn);
	TestEqual(TEXT("gain clamps to zoom 1"), Sensor.Zoom, 1.0f);
	Sc.SensorId = 7;
	TestTrue(TEXT("unknown sensor IDs are EO"), ToSensorCommand(Sc).Waveband == ESensorMode::EO);

	FCigiCelestialState Cel;
	Cel.Year = 2025; Cel.Month = 3; Cel.Day = 1; Cel.Hour = 6; Cel.Minute = 30;
	Cel.bDateVld = true; Cel.bEphemerisEn = false;
	FTimeCommand T = ToTimeCommand(Cel);
	TestTrue(TEXT("date and time"), T.Utc.IsSet() && *T.Utc == FDateTime(2025, 3, 1, 6, 30));
	TestFalse(TEXT("ephemeris off = static time of day"), T.bRunning);
	Cel.Month = 2; Cel.Day = 30;
	TestFalse(TEXT("an impossible date sets no time"), ToTimeCommand(Cel).Utc.IsSet());
	Cel.Day = 1; Cel.bDateVld = false;
	TestFalse(TEXT("Date/Time Valid unset sets no time"), ToTimeCommand(Cel).Utc.IsSet());

	FCigiWeatherState Wx;
	Wx.Scope = 1; Wx.RegionId = 4; Wx.Coverage = 75.0f;
	const FWeatherCommand W = ToWeatherCommand(Wx);
	TestTrue(TEXT("regional weather"), W.Scope == FWeatherCommand::EScope::Regional && W.RegionId == 4 && W.CoveragePct == 75.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostCommandsOceanWaveTest, "CamSim.Hosts.Cigi.OceanWaveCarriesEveryField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FHostCommandsOceanWaveTest::RunTest(const FString& Parameters)
{
	FCigiWaveState In;
	In.WaveID = 2; In.bEnabled = true; In.WaveHtM = 1.5f; In.WaveLenM = 40.0f; In.PeriodS = 6.0f;
	In.DirectionDeg = 45.0f; In.PhaseOffsetDeg = 90.0f; In.Scope = 0; In.EntityRgnId = 7; In.Breaker = 1;
	const FOceanWaveCommand C = CamSim::Cigi::ToOceanWaveCommand(In);
	TestEqual(TEXT("id"), C.WaveId, (uint8)2);
	TestTrue (TEXT("enabled"), C.bEnabled);
	TestEqual(TEXT("height"), C.HeightM, 1.5f);
	TestEqual(TEXT("length"), C.LengthM, 40.0f);
	TestEqual(TEXT("period"), C.PeriodS, 6.0f);
	TestEqual(TEXT("direction"), C.DirectionDeg, 45.0f);
	TestEqual(TEXT("phase"), C.PhaseOffsetDeg, 90.0f);
	TestTrue (TEXT("global scope"), C.Scope == FWeatherCommand::EScope::Global);
	TestEqual(TEXT("region"), C.RegionId, (uint16)7);
	TestEqual(TEXT("breaker"), C.Breaker, (uint8)1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostCommandsMaritimeClarityTest, "CamSim.Hosts.Cigi.MaritimeClarityIsPercent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FHostCommandsMaritimeClarityTest::RunTest(const FString& Parameters)
{
	// CIGI 3.3 Maritime Surface Conditions Control: Clarity is a percent, 0-100 (CCL bounds-checks it).
	FCigiMaritimeSurfaceState In;
	In.bSurfaceCondEn = true; In.SurfaceHeight = -1.5f; In.WaterTemp = 22.0f; In.Clarity = 20.0f;
	FMaritimeSurfaceCommand C = CamSim::Cigi::ToMaritimeSurfaceCommand(In);
	TestEqual(TEXT("clarity 20 % -> 0.2"), static_cast<double>(C.Clarity), 0.2, 1e-6);
	TestEqual(TEXT("surface height"), static_cast<double>(C.SurfaceHeightM), -1.5, 1e-9);
	TestEqual(TEXT("water temperature"), static_cast<double>(C.WaterTempC), 22.0, 1e-9);
	In.Clarity = 100.0f;
	C = CamSim::Cigi::ToMaritimeSurfaceCommand(In);
	TestEqual(TEXT("clarity 100 % -> 1"), static_cast<double>(C.Clarity), 1.0, 1e-6);
	TestEqual(TEXT("default packet state is fully clear"),
		static_cast<double>(CamSim::Cigi::ToMaritimeSurfaceCommand(FCigiMaritimeSurfaceState()).Clarity), 1.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHostDisCommandsTest,
	"CamSim.Hosts.DisEntityCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHostDisCommandsTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Dis;
	const double Lat = 37.77, Lon = -122.42, Alt = 1500.0;
	const FVector Ecef = CamSimFrames::GeodeticToEcef(Lat, Lon, Alt);
	double Psi, Theta, Phi;
	CamSimFrames::CigiToDisEuler(90.0, 10.0, -5.0, Lat, Lon, Psi, Theta, Phi);
	const FVector EastEcef  = CamSimFrames::NedToEcef(Lat, Lon) * FVector(0, 100, 0);
	const FVector NorthEcef = CamSimFrames::NedToEcef(Lat, Lon) * FVector(2, 0, 0);

	FDisEntityStatePdu Pdu;
	Pdu.EntityId.Site = 1; Pdu.EntityId.Application = 2; Pdu.EntityId.Entity = 3;
	Pdu.EntityType.EntityKind = 1; Pdu.EntityType.Domain = 2;
	Pdu.LocationX = Ecef.X; Pdu.LocationY = Ecef.Y; Pdu.LocationZ = Ecef.Z;
	Pdu.Psi = Psi; Pdu.Theta = Theta; Pdu.Phi = Phi;
	Pdu.DeadReckoning.VelX = EastEcef.X; Pdu.DeadReckoning.VelY = EastEcef.Y; Pdu.DeadReckoning.VelZ = EastEcef.Z;
	Pdu.DeadReckoning.AccelX = NorthEcef.X; Pdu.DeadReckoning.AccelY = NorthEcef.Y; Pdu.DeadReckoning.AccelZ = NorthEcef.Z;
	Pdu.DeadReckoning.AngVelZ = 0.1f;

	Pdu.DeadReckoning.Algorithm = 4;  // RVW
	const FEntityCommand C = ToEntityCommand(Pdu, 1001);
	TestEqual(TEXT("key"), C.Key, Key(Pdu.EntityId));
	TestTrue(TEXT("position"), FMath::IsNearlyEqual(C.Pose.Lat, Lat, 1e-9) && FMath::IsNearlyEqual(C.Pose.Alt, Alt, 1e-3));
	const FRotator Hpr = C.Pose.Neu.Rotator();
	TestTrue(FString::Printf(TEXT("local orientation %s"), *Hpr.ToString()),
		FMath::IsNearlyEqual(Hpr.Yaw, 90.0, 1e-3) && FMath::IsNearlyEqual(Hpr.Pitch, 10.0, 1e-3) && FMath::IsNearlyEqual(Hpr.Roll, -5.0, 1e-3));
	if (TestTrue(TEXT("RVW has motion"), C.Motion.IsSet()))
	{
		const FMotionModel& M = *C.Motion;
		TestTrue(TEXT("RVW: world velocity, NED east"), M.LinearFrame == FMotionModel::EFrame::World && M.Velocity.Equals(FVector(0, 100, 0), 0.01));
		TestTrue(TEXT("RVW: world acceleration, NED north"), M.Acceleration.Equals(FVector(2, 0, 0), 0.01));
		TestTrue(TEXT("RVW: body yaw rate"), M.AngularFrame == FMotionModel::EFrame::Body && FMath::IsNearlyEqual(M.AngularRate.Z, 5.7296, 1e-3));
	}

	Pdu.DeadReckoning.Algorithm = 2;  // FPW
	TOptional<FMotionModel> M = ToMotionModel(Pdu, Lat, Lon);
	TestTrue(TEXT("FPW: no acceleration, no rotation"), M.IsSet() && M->Acceleration.IsZero() && M->AngularRate.IsZero());

	Pdu.DeadReckoning.Algorithm = 9;  // FVB
	Pdu.DeadReckoning.VelX = 50.0f; Pdu.DeadReckoning.VelY = 0.0f; Pdu.DeadReckoning.VelZ = 0.0f;
	Pdu.DeadReckoning.AccelX = 1.0f; Pdu.DeadReckoning.AccelY = 0.0f; Pdu.DeadReckoning.AccelZ = 0.0f;
	M = ToMotionModel(Pdu, Lat, Lon);
	TestTrue(TEXT("FVB: body velocity and acceleration, no rotation"),
		M.IsSet() && M->LinearFrame == FMotionModel::EFrame::Body && M->Velocity.Equals(FVector(50, 0, 0))
		&& M->Acceleration.Equals(FVector(1, 0, 0)) && M->AngularRate.IsZero());

	Pdu.DeadReckoning.Algorithm = 1;  // static
	TestFalse(TEXT("static: no motion"), ToMotionModel(Pdu, Lat, Lon).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisSurfaceModeTest,
	"CamSim.Dis.SurfaceMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisSurfaceModeTest::RunTest(const FString& Parameters)
{
	using CamSim::Dis::SurfaceModeFor;
	TestTrue(TEXT("land platform → ground"),    SurfaceModeFor(1, 1, true) == ESurfaceMode::Ground);
	TestTrue(TEXT("air platform → none"),       SurfaceModeFor(1, 2, true) == ESurfaceMode::None);
	TestTrue(TEXT("surface platform → water"),  SurfaceModeFor(1, 3, true) == ESurfaceMode::Water);
	TestTrue(TEXT("subsurface platform → none"), SurfaceModeFor(1, 4, true) == ESurfaceMode::None);
	TestTrue(TEXT("clamp off → none"),          SurfaceModeFor(1, 1, false) == ESurfaceMode::None);
	// Only platforms are placed: a munition's domain is its target's domain.
	TestTrue(TEXT("anti-land munition → none"),    SurfaceModeFor(2, 1, true) == ESurfaceMode::None);
	TestTrue(TEXT("anti-surface munition → none"), SurfaceModeFor(2, 3, true) == ESurfaceMode::None);
	TestTrue(TEXT("life form on land → none"),     SurfaceModeFor(3, 1, true) == ESurfaceMode::None);

	FDisEntityStatePdu Pdu;
	Pdu.EntityType.EntityKind = 1; Pdu.EntityType.Domain = 1;
	Pdu.LocationX = 6378137.0;
	TestTrue(TEXT("command carries ground"), CamSim::Dis::ToEntityCommand(Pdu, 2001).SurfaceMode == ESurfaceMode::Ground);
	TestTrue(TEXT("command honours clamp off"), CamSim::Dis::ToEntityCommand(Pdu, 2001, false).SurfaceMode == ESurfaceMode::None);
	Pdu.EntityType.EntityKind = 2;
	TestTrue(TEXT("munition command: none"), CamSim::Dis::ToEntityCommand(Pdu, 2001).SurfaceMode == ESurfaceMode::None);
	return true;
}
