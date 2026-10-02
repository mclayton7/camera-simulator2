// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeExit.h"
#include "Common/UdpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Components/SceneCaptureComponent2D.h"

#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "CIGI/CigiPacketTypes.h"
#include "CIGI/CigiReceiver.h"
#include "Config/CamSimConfig.h"
#include "Environment/CamSimEnvironment.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/Geoid.h"
#include "Metadata/KlvBuilder.h"

THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiHostSession.h"
#include "cigicl/CigiOutgoingMsg.h"
#include "cigicl/CigiIGCtrlV3_3.h"
#include "cigicl/CigiEntityCtrlV3_3.h"
#include "cigicl/CigiViewCtrlV3.h"
#include "cigicl/CigiViewDefV3.h"
#include "cigicl/CigiSensorCtrlV3.h"
THIRD_PARTY_INCLUDES_END

// -------------------------------------------------------------------------
// Hooter HITL rig support (HITL.md, "CamSim today and its gaps"): gimbal yaw
// wrap and slew, one read point per datagram, the gimbal sign convention
// pinned to the world, KLV fidelity tags, CIGI packet 201 and the image
// footprint.
// -------------------------------------------------------------------------

namespace
{
	constexpr auto kHitlTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** Walk a ST 0601 packet's top-level TLVs. Returns false on malformed input. */
	bool ParseKlv(const TArray<uint8>& Packet, TMap<uint8, TArray<uint8>>& OutTags)
	{
		if (Packet.Num() < 17) return false;
		int32 Pos = 16;
		int32 ValueLen = Packet[Pos++];
		if (ValueLen & 0x80)
		{
			const int32 NumBytes = ValueLen & 0x7F;
			ValueLen = 0;
			for (int32 i = 0; i < NumBytes; ++i) ValueLen = (ValueLen << 8) | Packet[Pos++];
		}
		const int32 End = Pos + ValueLen;
		if (End != Packet.Num()) return false;
		while (Pos < End)
		{
			if (Pos + 2 > End) return false;
			const uint8 Tag = Packet[Pos++];
			int32 Len = Packet[Pos++];
			if (Len & 0x80)
			{
				const int32 NumBytes = Len & 0x7F;
				Len = 0;
				for (int32 i = 0; i < NumBytes; ++i) Len = (Len << 8) | Packet[Pos++];
			}
			if (Pos + Len > End) return false;
			OutTags.Add(Tag, TArray<uint8>(Packet.GetData() + Pos, Len));
			Pos += Len;
		}
		return true;
	}

	TMap<uint8, TArray<uint8>> BuildTags(const FCamSimTelemetry& T)
	{
		TMap<uint8, TArray<uint8>> Tags;
		ParseKlv(FKlvBuilder::BuildMisbST0601(T), Tags);
		return Tags;
	}

	/** Big-endian unsigned value of a tag (0 when absent). */
	uint64 TagUnsigned(const TMap<uint8, TArray<uint8>>& Tags, uint8 Tag)
	{
		const TArray<uint8>* V = Tags.Find(Tag);
		uint64 X = 0;
		if (V) for (uint8 B : *V) X = (X << 8) | B;
		return X;
	}

	FString TagString(const TMap<uint8, TArray<uint8>>& Tags, uint8 Tag)
	{
		const TArray<uint8>* V = Tags.Find(Tag);
		return V ? FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(V->GetData()), V->Num()) : FString();
	}

	FCamSimTelemetry BaseTelemetry()
	{
		FCamSimTelemetry T;
		T.TimestampUs = 1790000000000000ULL;
		T.Latitude = 38.8977; T.Longitude = -77.0365; T.Altitude = 1500.0;
		return T;
	}

	/** Local (North, East, Up) metres from the sensor to a point at SurfaceAlt. */
	FVector OffsetNeu(const FCamSimTelemetry& T, double Lat, double Lon, double SurfaceAlt = 0.0)
	{
		return CamSimFrames::GeodeticDeltaToNeu(T.Latitude, T.Longitude, SurfaceAlt, Lat, Lon, SurfaceAlt);
	}

	void PutBe32(TArray<uint8>& B, uint32 V) { for (int32 i = 3; i >= 0; --i) B.Add(uint8(V >> (8 * i))); }
	void PutLe32(TArray<uint8>& B, uint32 V) { for (int32 i = 0; i < 4; ++i) B.Add(uint8(V >> (8 * i))); }

	/** CIGI user-defined packet 201 (hitl/PROTOCOL.md section 2) in the given byte order. */
	TArray<uint8> PackKinematics(bool bBigEndian, uint16 EntityId, uint8 Flags, float Tas, float Ias, float MagHdg,
		float VelN, float VelE, float VelD, double SampleUtc)
	{
		TArray<uint8> B;
		B.Add(201);
		B.Add(48);
		if (bBigEndian) { B.Add(uint8(EntityId >> 8)); B.Add(uint8(EntityId)); }
		else            { B.Add(uint8(EntityId)); B.Add(uint8(EntityId >> 8)); }
		B.Add(Flags); B.Add(0); B.Add(0); B.Add(0);
		for (float F : { Tas, Ias, MagHdg, VelN, VelE, VelD })
		{
			uint32 Bits; FMemory::Memcpy(&Bits, &F, 4);
			bBigEndian ? PutBe32(B, Bits) : PutLe32(B, Bits);
		}
		uint64 Bits64; FMemory::Memcpy(&Bits64, &SampleUtc, 8);
		if (bBigEndian) { PutBe32(B, uint32(Bits64 >> 32)); PutBe32(B, uint32(Bits64)); }
		else            { PutLe32(B, uint32(Bits64)); PutLe32(B, uint32(Bits64 >> 32)); }
		for (int32 i = 0; i < 8; ++i) B.Add(0);
		check(B.Num() == 48);
		return B;
	}

	/** Send one datagram to the receiver's port on localhost. */
	bool SendDatagram(int32 Port, const TArray<uint8>& Bytes)
	{
		ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Tx = FUdpSocketBuilder(TEXT("HitlTestHost")).Build();
		if (!Tx) return false;
		TSharedRef<FInternetAddr> Dest = SS->CreateInternetAddr();
		bool bValidIp = false;
		Dest->SetIp(TEXT("127.0.0.1"), bValidIp);
		Dest->SetPort(Port);
		int32 Sent = 0;
		const bool bOk = Tx->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *Dest) && Sent == Bytes.Num();
		SS->DestroySocket(Tx);
		return bOk;
	}

	/** Wait for up to Count camera frames. */
	TArray<FCigiCameraFrame> WaitForCameraFrames(FCigiReceiver& Receiver, int32 Count, double TimeoutSec = 2.0)
	{
		TArray<FCigiCameraFrame> Frames;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSec;
		FCigiCameraFrame Frame;
		while (Frames.Num() < Count && FPlatformTime::Seconds() < Deadline)
		{
			if (Receiver.DequeueCameraFrame(Frame)) Frames.Add(MoveTemp(Frame));
			else FPlatformProcess::Sleep(0.002f);
		}
		return Frames;
	}
}

// -------------------------------------------------------------------------
// Gap 1: View Control yaw is unwound before the clamp (CIGI sends 0-360)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlGimbalYawUnwoundTest,
	"CamSim.Hitl.Gimbal.ViewControlYawUnwound",
	kHitlTestFlags)

bool FHitlGimbalYawUnwoundTest::RunTest(const FString& Parameters)
{
	UCamSimGimbalComponent* Gimbal = NewObject<UCamSimGimbalComponent>(GetTransientPackage());
	FCamSimConfig Cfg;  // yaw limits -180..180

	FCigiViewControl Vc;
	Vc.bYawEn = true;
	for (const TPair<float, float>& Case : { TPair<float, float>(270.0f, -90.0f), TPair<float, float>(90.0f, 90.0f),
		TPair<float, float>(359.0f, -1.0f), TPair<float, float>(360.0f, 0.0f), TPair<float, float>(-90.0f, -90.0f),
		TPair<float, float>(180.0f, 180.0f) })
	{
		Vc.Yaw = Case.Key;
		Gimbal->ApplyViewControl(Vc, Cfg);
		TestTrue(FString::Printf(TEXT("View Control yaw %.0f -> %.1f (got %.1f)"), Case.Key, Case.Value, Gimbal->GetGimbalYaw()),
			FMath::IsNearlyEqual(FMath::UnwindDegrees(Gimbal->GetGimbalYaw() - Case.Value), 0.0f, 1e-3f));
	}

	// Articulated Part on the camera entity: same unwrap.
	FCigiArtPartControl Art;
	Art.bArtPartEn = true;
	Art.bYawEn = true;
	Art.Yaw = 270.0f;
	Gimbal->ApplyArtPart(Art, 0.0f, Cfg);  // slew rate 0 = snap
	TestEqual(TEXT("Art Part yaw 270 -> -90"), Gimbal->GetGimbalYaw(), -90.0f);
	return true;
}

// -------------------------------------------------------------------------
// Gap 1 (slew): a rate-limited yaw slews across +/-180 instead of sticking at
// the limit, and a gimbal with stops goes the long way round.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlGimbalSlewAcross180Test,
	"CamSim.Hitl.Gimbal.SlewAcross180",
	kHitlTestFlags)

bool FHitlGimbalSlewAcross180Test::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.GimbalMaxSlewRateDegPerSec = 10.0f;  // 10 deg per 1 s tick
	Cfg.GimbalPitchMin = -90.0f; Cfg.GimbalPitchMax = 90.0f;

	FCigiViewControl Start;
	Start.bYawEn = true; Start.Yaw = 170.0f;
	FCigiArtPartControl Target;
	Target.bArtPartEn = true; Target.bYawEn = true; Target.Yaw = 190.0f;  // = -170

	// Continuous rotation (limits span 360): 170 -> 180 -> -170, the short way.
	{
		UCamSimGimbalComponent* Gimbal = NewObject<UCamSimGimbalComponent>(GetTransientPackage());
		Gimbal->ApplyViewControl(Start, Cfg);
		Gimbal->ApplyArtPart(Target, 1.0f, Cfg);
		TestTrue(FString::Printf(TEXT("first tick reaches +/-180 (%.1f)"), Gimbal->GetGimbalYaw()),
			FMath::IsNearlyEqual(FMath::Abs(Gimbal->GetGimbalYaw()), 180.0f, 1e-3f));
		Gimbal->AdvanceSlew(1.0f, Cfg);
		TestTrue(FString::Printf(TEXT("second tick crosses to -170 (%.1f)"), Gimbal->GetGimbalYaw()),
			FMath::IsNearlyEqual(Gimbal->GetGimbalYaw(), -170.0f, 1e-3f));
		Gimbal->AdvanceSlew(1.0f, Cfg);
		TestTrue(TEXT("and stays on the target"), FMath::IsNearlyEqual(Gimbal->GetGimbalYaw(), -170.0f, 1e-3f));
	}

	// Mechanical stops at +/-175: it can't cross 180, so it slews back through 0.
	{
		FCamSimConfig Stops = Cfg;
		Stops.GimbalYawMin = -175.0f; Stops.GimbalYawMax = 175.0f;
		Stops.GimbalMaxSlewRateDegPerSec = 100.0f;
		UCamSimGimbalComponent* Gimbal = NewObject<UCamSimGimbalComponent>(GetTransientPackage());
		Gimbal->ApplyViewControl(Start, Stops);
		Gimbal->ApplyArtPart(Target, 1.0f, Stops);
		TestTrue(FString::Printf(TEXT("with stops, the first tick moves toward 0 (%.1f)"), Gimbal->GetGimbalYaw()),
			FMath::IsNearlyEqual(Gimbal->GetGimbalYaw(), 70.0f, 1e-3f));
		for (int32 i = 0; i < 5; ++i) Gimbal->AdvanceSlew(1.0f, Stops);
		TestTrue(FString::Printf(TEXT("and arrives at -170 the long way (%.1f)"), Gimbal->GetGimbalYaw()),
			FMath::IsNearlyEqual(Gimbal->GetGimbalYaw(), -170.0f, 1e-3f));
	}
	return true;
}

// -------------------------------------------------------------------------
// Gap 2: the receiver publishes each datagram's camera packets as one frame,
// so pose and gimbal are always read together.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlCameraFramePerDatagramTest,
	"CamSim.Hitl.Receiver.CameraFramePerDatagram",
	kHitlTestFlags)

bool FHitlCameraFramePerDatagramTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.CigiBindAddr   = TEXT("127.0.0.1");
	Config.CigiPort       = 48881;
	Config.CameraEntityId = 1;
	FCigiReceiver Receiver(Config);
	if (!TestTrue(TEXT("Receiver started"), Receiver.Start())) return false;
	ON_SCOPE_EXIT { Receiver.Stop(); };

	CigiHostSession Session(1, 4096, 2, 4096);
	Session.SetCigiVersion(3, 3);
	CigiOutgoingMsg& Out = Session.GetOutgoingMsgMgr();

	// Two host frames, each a full camera update: pose, gimbal, FOV, sensor and packet 201.
	TArray<TArray<uint8>> Datagrams;
	for (int32 Frame = 0; Frame < 2; ++Frame)
	{
		CigiIGCtrlV3_3 IgCtrl;
		IgCtrl.SetFrameCntr(100 + Frame);
		CigiEntityCtrlV3_3 Ent;
		Ent.SetEntityID(1);
		Ent.SetEntityState(CigiBaseEntityCtrl::Active);
		Ent.SetLat(45.0 + Frame);
		Ent.SetLon(-100.0);
		Ent.SetAlt(1000.0);
		Ent.SetYaw(10.0f * Frame);
		CigiViewCtrlV3 View;
		View.SetEntityID(0);
		View.SetYawEn(true);
		View.SetPitchEn(true);
		View.SetYaw(270.0f - Frame);  // CCL takes 0-360 only
		View.SetPitch(-45.0f);
		CigiViewDefV3 Def;
		Def.SetFOVLeft(-10.0f - Frame);
		Def.SetFOVRight(10.0f + Frame);
		CigiSensorCtrlV3 Sensor;
		Sensor.SetSensorID(static_cast<Cigi_uint8>(Frame));
		Sensor.SetSensorOn(true);

		Out.BeginMsg();
		Out << IgCtrl << Ent << View << Def << Sensor;
		Cigi_uint8* Buf = nullptr;
		int Len = 0;
		if (!TestTrue(TEXT("host message packaged"), Out.PackageMsg(&Buf, Len) == CIGI_SUCCESS && Buf && Len > 0)) return false;
		TArray<uint8> Bytes(Buf, Len);
		Out.FreeMsg();
		// CCL packs in host (little-endian) order and says so in IG Control's Byte Swap Magic.
		Bytes.Append(PackKinematics(/*bBigEndian=*/Bytes[6] == 0x80, 1, 0x0F, 50.0f + Frame, 45.0f, 123.0f,
			3.0f, 4.0f, -1.0f, 1790000000.5 + Frame));
		Datagrams.Add(MoveTemp(Bytes));
	}
	for (const TArray<uint8>& D : Datagrams) TestTrue(TEXT("datagram sent"), SendDatagram(Config.CigiPort, D));

	const TArray<FCigiCameraFrame> Frames = WaitForCameraFrames(Receiver, 2);
	if (!TestEqual(TEXT("one camera frame per datagram"), Frames.Num(), 2)) return false;
	FCigiCameraFrame Extra;
	TestFalse(TEXT("no partial frames"), Receiver.DequeueCameraFrame(Extra));

	for (int32 i = 0; i < 2; ++i)
	{
		const FCigiCameraFrame& F = Frames[i];
		const FString Ctx = FString::Printf(TEXT("frame %d: "), i);
		TestTrue(Ctx + TEXT("pose"), F.bHasPose && FMath::IsNearlyEqual(F.Pose.Latitude, 45.0 + i, 1e-9));
		TestTrue(Ctx + TEXT("view control with the same pose"), F.ViewControls.Num() == 1 && FMath::IsNearlyEqual(F.ViewControls[0].Yaw, 270.0f - i, 1e-3f));
		TestTrue(Ctx + TEXT("view definition"), F.ViewDefinitions.Num() == 1 && FMath::IsNearlyEqual(F.ViewDefinitions[0].HFovDeg(), 20.0f + 2 * i, 1e-3f));
		TestTrue(Ctx + TEXT("sensor control"), F.SensorControls.Num() == 1 && F.SensorControls[0].SensorId == i);
		TestTrue(Ctx + TEXT("kinematics"), F.bHasKinematics && FMath::IsNearlyEqual(F.Kinematics.TrueAirspeedMps, 50.0f + i));
		TestTrue(Ctx + TEXT("sample time kept with a pose"), F.Kinematics.HasSampleTime()
			&& FMath::IsNearlyEqual(F.Kinematics.SampleUtcSec, 1790000000.5 + i, 1e-6));
	}

	// Folding both (as the camera does when two datagrams arrive in one frame): the newest wins.
	FCigiCameraFrame Merged;
	Merged.Append(Frames[0]);
	Merged.Append(Frames[1]);
	TestTrue(TEXT("merged pose is the newest"), FMath::IsNearlyEqual(Merged.Pose.Latitude, 46.0, 1e-9));
	TestEqual(TEXT("merged packets keep their order"), Merged.ViewControls.Num(), 2);
	TestTrue(TEXT("merged kinematics are the newest"), FMath::IsNearlyEqual(Merged.Kinematics.TrueAirspeedMps, 51.0f));
	FCigiCameraFrame PoseOnly;
	PoseOnly.bHasPose = true;
	Merged.Append(PoseOnly);
	TestFalse(TEXT("a later pose without packet 201 drops the sample time"), Merged.Kinematics.HasSampleTime());
	return true;
}

// -------------------------------------------------------------------------
// Gap 9: packet 201 in a big-endian datagram (a standard non-CCL host), and
// for another entity (ignored).
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlKinematicsBigEndianTest,
	"CamSim.Hitl.Receiver.KinematicsBigEndian",
	kHitlTestFlags)

bool FHitlKinematicsBigEndianTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.CigiBindAddr   = TEXT("127.0.0.1");
	Config.CigiPort       = 48882;
	Config.CameraEntityId = 7;
	FCigiReceiver Receiver(Config);
	if (!TestTrue(TEXT("Receiver started"), Receiver.Start())) return false;
	ON_SCOPE_EXIT { Receiver.Stop(); };

	// CIGI 3.3 IG Control packed big-endian (Byte Swap Magic 0x8000 in network order).
	TArray<uint8> Igc = { 1, 24, 3, 0, 0x30, 0, 0x80, 0x00 };
	PutBe32(Igc, 5); PutBe32(Igc, 0); PutBe32(Igc, 0); PutBe32(Igc, 0);

	TArray<uint8> Mine = Igc;
	Mine.Append(PackKinematics(true, 7, 0x0F, 61.5f, 55.25f, 271.0f, -12.5f, 30.0f, 2.0f, 1790000123.25));
	TArray<uint8> Other = Igc;
	Other.Append(PackKinematics(true, 8, 0x0F, 99.0f, 99.0f, 99.0f, 99.0f, 99.0f, 99.0f, 1.0));
	TestTrue(TEXT("other entity's datagram sent"), SendDatagram(Config.CigiPort, Other));
	TestTrue(TEXT("camera entity's datagram sent"), SendDatagram(Config.CigiPort, Mine));

	const TArray<FCigiCameraFrame> Frames = WaitForCameraFrames(Receiver, 1);
	if (!TestEqual(TEXT("only the camera entity's packet 201 makes a frame"), Frames.Num(), 1)) return false;
	const FCigiPlatformKinematics& K = Frames[0].Kinematics;
	TestTrue(TEXT("kinematics present"), Frames[0].bHasKinematics);
	TestEqual(TEXT("entity id"), K.EntityId, static_cast<uint16>(7));
	TestEqual(TEXT("true airspeed"), K.TrueAirspeedMps, 61.5f);
	TestEqual(TEXT("indicated airspeed"), K.IndicatedAirspeedMps, 55.25f);
	TestEqual(TEXT("magnetic heading"), K.MagneticHeadingDeg, 271.0f);
	TestEqual(TEXT("velocity north"), K.VelNorthMps, -12.5f);
	TestEqual(TEXT("velocity east"), K.VelEastMps, 30.0f);
	TestEqual(TEXT("velocity down"), K.VelDownMps, 2.0f);
	TestEqual(TEXT("sample time"), K.SampleUtcSec, 1790000123.25);
	TestTrue(TEXT("airspeed / heading / velocity flags"), K.HasAirspeeds() && K.HasMagneticHeading() && K.HasNedVelocity());
	TestFalse(TEXT("sample time dropped: no pose in this datagram"), K.HasSampleTime());
	return true;
}

// -------------------------------------------------------------------------
// Gap 3: the gimbal sign convention, pinned to the world. The platform's
// rotation is what FCamSimPlatformRig gives the globe anchor (at the
// georeference origin Cesium's East-South-Up axes are UE's world axes), the
// gimbal's is what ACamSimCamera gives the SceneCapture, composed by UE's own
// component attachment. The telemetry composition (SensorToNeu) and the
// fallback frame centre must agree with it.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlGimbalSignTest,
	"CamSim.Hitl.Gimbal.SignConventionInWorld",
	kHitlTestFlags)

bool FHitlGimbalSignTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		float Heading, Pitch, Roll;          // platform (CIGI Entity Control)
		float GYaw, GPitch, GRoll;           // gimbal (CIGI View Control)
		FVector ExpectedCentreNeu;           // frame centre from the sensor, metres (N, E), on flat ground 1000 m below
	};
	const double Alt = 1000.0;
	const double Tan30 = FMath::Tan(FMath::DegreesToRadians(30.0));
	const FCase Cases[] = {
		{ TEXT("heading 0, gimbal yaw +90 pitch -45: due east, one altitude away"),  0, 0, 0,   90, -45, 0, FVector(0.0, Alt, 0.0) },
		{ TEXT("heading 0, gimbal yaw 270 (= -90) pitch -45: due west"),             0, 0, 0,  270, -45, 0, FVector(0.0, -Alt, 0.0) },
		{ TEXT("heading 90 (east), gimbal yaw +90 pitch -45: due south"),           90, 0, 0,   90, -45, 0, FVector(-Alt, 0.0, 0.0) },
		{ TEXT("heading 225, gimbal yaw -45 pitch -45: due south"),                225, 0, 0,  -45, -45, 0, FVector(-Alt, 0.0, 0.0) },
		{ TEXT("roll 30 right wing down, gimbal nadir: the view swings west"),       0, 0, 30,   0, -90, 0, FVector(0.0, -Alt * Tan30, 0.0) },
		{ TEXT("pitch 10 nose up, gimbal pitch -55: 45 deg down, north"),            0, 10, 0,   0, -55, 0, FVector(Alt, 0.0, 0.0) },
		{ TEXT("heading 0, gimbal pitch -60 roll 90: roll about the boresight only"), 0, 0, 0,   0, -60, 90, FVector(Alt * Tan30, 0.0, 0.0) },
	};

	FCamSimConfig Cfg;
	Cfg.GimbalPitchMin = -90.0f; Cfg.GimbalPitchMax = 90.0f;

	for (const FCase& C : Cases)
	{
		// The scene: platform rotation on the actor root, gimbal as the SceneCapture's relative rotation.
		USceneComponent* Platform = NewObject<USceneComponent>(GetTransientPackage());
		USceneCaptureComponent2D* Sensor = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
		Sensor->SetupAttachment(Platform);
		Platform->SetWorldRotation(CamSimFrames::CigiToEastSouthUp(C.Heading, C.Pitch, C.Roll));

		UCamSimGimbalComponent* Gimbal = NewObject<UCamSimGimbalComponent>(GetTransientPackage());
		FCigiViewControl Vc;
		Vc.bYawEn = Vc.bPitchEn = Vc.bRollEn = true;
		Vc.Yaw = C.GYaw; Vc.Pitch = C.GPitch; Vc.Roll = C.GRoll;
		Gimbal->ApplyViewControl(Vc, Cfg);
		Sensor->SetRelativeRotation(Gimbal->GetGimbalRelativeRotation());

		// UE world = East-South-Up here; to North-East-Up.
		const FVector FwdEsu = Sensor->GetForwardVector();
		const FVector FwdNeu(-FwdEsu.Y, FwdEsu.X, FwdEsu.Z);
		const FVector Expected = FVector(C.ExpectedCentreNeu.X, C.ExpectedCentreNeu.Y, -Alt).GetSafeNormal();
		TestTrue(FString::Printf(TEXT("%s: scene boresight %s, expected %s"), C.Name, *FwdNeu.ToString(), *Expected.ToString()),
			FwdNeu.Equals(Expected, 1e-4));

		// Telemetry: the same composition, and the frame centre it reports without terrain.
		FCamSimTelemetryAssembler Asm;
		Asm.SetPlatformPose(0.0, 0.0, Alt, C.Heading, C.Pitch, C.Roll);
		Asm.SetGimbal(Gimbal->GetGimbalYaw(), Gimbal->GetGimbalPitch(), Gimbal->GetGimbalRoll());
		Asm.SetFieldOfView(20.0f, CamSimTelemetry::VerticalFovDeg(20.0f, 1920, 1080));
		const FVector TelemetryFwd = FCamSimTelemetryAssembler::SensorToNeu(Asm.Get()).GetForwardVector();
		TestTrue(FString::Printf(TEXT("%s: telemetry boresight %s matches the scene"), C.Name, *TelemetryFwd.ToString()),
			TelemetryFwd.Equals(FwdNeu, 1e-4));

		Asm.UpdateFootprint(nullptr, nullptr, nullptr, nullptr);
		const FCamSimTelemetry& T = Asm.Get();
		const FVector Centre = OffsetNeu(T, T.FrameCenterLat, T.FrameCenterLon);
		TestTrue(FString::Printf(TEXT("%s: frame centre N %.1f E %.1f m"), C.Name, Centre.X, Centre.Y),
			T.SlantRangeM > 0.0 && FMath::IsNearlyEqual(Centre.X, C.ExpectedCentreNeu.X, 1.0)
			&& FMath::IsNearlyEqual(Centre.Y, C.ExpectedCentreNeu.Y, 1.0));
	}
	return true;
}

// -------------------------------------------------------------------------
// Gap 4: Sensor Control logs a Log line only when the sensor changes.
// -------------------------------------------------------------------------

namespace
{
	class FSensorLogCapture : public FOutputDevice
	{
	public:
		int32 LogLines = 0;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity <= ELogVerbosity::Log && FCString::Strstr(V, TEXT("UCamSimSensorComponent: sensor=")))
			{
				++LogLines;
			}
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlSensorLogOnChangeTest,
	"CamSim.Hitl.Sensor.LogsOnlyOnChange",
	kHitlTestFlags)

bool FHitlSensorLogOnChangeTest::RunTest(const FString& Parameters)
{
	UCamSimSensorComponent* Sensor = NewObject<UCamSimSensorComponent>(GetTransientPackage());
	FCamSimConfig Cfg;
	Cfg.SensorFovPresets.Empty();

	FSensorLogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Capture); };

	FCigiSensorControl Pkt;
	Pkt.SensorId = 0; Pkt.bSensorOn = true; Pkt.Polarity = 0;
	for (int32 i = 0; i < 30; ++i) Sensor->ApplySensorControl(Pkt, Cfg, nullptr);  // a second of resends
	Pkt.SensorId = 1;
	for (int32 i = 0; i < 30; ++i) Sensor->ApplySensorControl(Pkt, Cfg, nullptr);  // switch to IR
	Pkt.Polarity = 1;
	Sensor->ApplySensorControl(Pkt, Cfg, nullptr);                              // black-hot
	GLog->Flush();

	TestEqual(TEXT("three state changes, three Log lines (61 packets)"), Capture.LogLines, 3);
	TestEqual(TEXT("the state still follows every packet"), Sensor->GetPolarity(), static_cast<uint8>(1));
	return true;
}

// -------------------------------------------------------------------------
// Gap 5: Tag 17 is the angular vertical FOV, 2·atan(tan(HFOV/2)·H/W).
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlVerticalFovTest,
	"CamSim.Hitl.Klv.VerticalFovIsAngular",
	kHitlTestFlags)

bool FHitlVerticalFovTest::RunTest(const FString& Parameters)
{
	const float VFov = CamSimTelemetry::VerticalFovDeg(90.0f, 1920, 1080);
	const float Expected = static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Atan(9.0 / 16.0)));  // 58.72, not 50.6
	TestTrue(FString::Printf(TEXT("HFOV 90 at 16:9 -> VFOV %.3f (expected %.3f)"), VFov, Expected),
		FMath::IsNearlyEqual(VFov, Expected, 1e-3f));
	TestTrue(TEXT("narrow FOV ~ linear"), FMath::IsNearlyEqual(CamSimTelemetry::VerticalFovDeg(2.0f, 1920, 1080), 1.125f, 1e-3f));
	TestEqual(TEXT("square image: VFOV = HFOV"), CamSimTelemetry::VerticalFovDeg(40.0f, 512, 512), 40.0f);

	// Tag 17's fallback when the telemetry has no VFOV assumes 16:9, angular too.
	FCamSimTelemetry T = BaseTelemetry();
	T.HFovDeg = 90.0f; T.VFovDeg = 0.0f;
	const uint64 Tag17 = TagUnsigned(BuildTags(T), 17);
	TestEqual(TEXT("Tag 17 fallback"), static_cast<uint16>(Tag17), FKlvBuilder::MapFov(Expected));
	return true;
}

// -------------------------------------------------------------------------
// Gap 6: Tags 90/91, full-range platform pitch and roll, alongside 6/7.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlFullPitchRollTest,
	"CamSim.Hitl.Klv.FullRangePitchRoll",
	kHitlTestFlags)

bool FHitlFullPitchRollTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("+90 -> INT32_MAX"), FKlvBuilder::MapFullAngle90(90.0f), 0x7FFFFFFF);
	TestEqual(TEXT("-90 -> -INT32_MAX"), FKlvBuilder::MapFullAngle90(-90.0f), -0x7FFFFFFF);
	TestEqual(TEXT("clamped beyond 90"), FKlvBuilder::MapFullAngle90(120.0f), 0x7FFFFFFF);
	TestEqual(TEXT("NaN -> 0"), FKlvBuilder::MapFullAngle90(NAN), 0);

	ON_SCOPE_EXIT { FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f); };
	FCamSimTelemetry T = BaseTelemetry();
	T.Pitch = 35.0f;   // beyond Tag 6's +/-20
	T.Roll  = -75.0f;  // beyond Tag 7's +/-50

	// Off by default (misb.js 0.1.30 misdecodes them), from config and in the builder.
	TestFalse(TEXT("klv_full_range_attitude defaults to false"), FCamSimConfig::LoadFromYamlString(TEXT("")).Phase26.bKlvFullRangeAttitude);
	TestTrue(TEXT("phase26.klv_full_range_attitude: true"),
		FCamSimConfig::LoadFromYamlString(TEXT("phase26:\n  klv_full_range_attitude: true\n")).Phase26.bKlvFullRangeAttitude);
	{
		const TCHAR* Var = TEXT("CAMSIM_KLV_FULL_RANGE_ATTITUDE");
		ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(Var, TEXT("")); };
		FPlatformMisc::SetEnvironmentVar(Var, TEXT("1"));
		TestTrue(TEXT("CAMSIM_KLV_FULL_RANGE_ATTITUDE=1"), FCamSimConfig::LoadFromYamlString(TEXT("")).Phase26.bKlvFullRangeAttitude);
	}
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	{
		const TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
		TestTrue(TEXT("off: Tags 6 and 7 sent"), Tags.Contains(6) && Tags.Contains(7));
		TestFalse(TEXT("off: Tags 90 and 91 omitted"), Tags.Contains(90) || Tags.Contains(91));
	}

	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f, TEXT(""), TEXT(""), TEXT(""), /*bFullRangeAttitude=*/true);
	const TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
	TestTrue(TEXT("on: Tags 6 and 7 still sent"), Tags.Contains(6) && Tags.Contains(7));
	if (TestTrue(TEXT("on: Tags 90 and 91 are 4 bytes"), Tags.Contains(90) && Tags.Contains(91) && Tags[90].Num() == 4 && Tags[91].Num() == 4))
	{
		const double Pitch = static_cast<int32>(TagUnsigned(Tags, 90)) * 90.0 / 2147483647.0;
		const double Roll  = static_cast<int32>(TagUnsigned(Tags, 91)) * 90.0 / 2147483647.0;
		TestTrue(FString::Printf(TEXT("Tag 90 decodes %.6f"), Pitch), FMath::IsNearlyEqual(Pitch, 35.0, 1e-6));
		TestTrue(FString::Printf(TEXT("Tag 91 decodes %.6f"), Roll), FMath::IsNearlyEqual(Roll, -75.0, 1e-6));
	}
	return true;
}

// -------------------------------------------------------------------------
// Gap 7: wind, static pressure, air temperature and humidity once the host
// has sent Atmosphere Control, never before.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlAtmosphereTagsTest,
	"CamSim.Hitl.Klv.AtmosphereTags",
	kHitlTestFlags)

bool FHitlAtmosphereTagsTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FCamSimTelemetry T = BaseTelemetry();
	{
		const TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
		for (uint8 Tag : { 35, 36, 37, 39, 55 })
		{
			TestFalse(FString::Printf(TEXT("Tag %u omitted before Atmosphere Control"), Tag), Tags.Contains(Tag));
		}
	}

	// The environment folds the host's packet into the snapshot the telemetry reads.
	ACamSimEnvironment::FAtmosphericSnapshot Snap;
	TestFalse(TEXT("snapshot starts without host atmosphere"), Snap.bHostAtmosphere);
	FCigiAtmosphereState A;
	A.Humidity = 64.0f; A.AirTemp = -12.4f; A.HorizWindSp = 12.0f; A.WindDir = 270.0f; A.BaroPress = 1000.0f;
	ACamSimEnvironment::FoldAtmosphere(Snap, A);
	TestTrue(TEXT("host atmosphere received"), Snap.bHostAtmosphere);

	T.bHasAtmosphere   = Snap.bHostAtmosphere;
	T.WindDirectionDeg = Snap.WindDirectionDeg;
	T.WindSpeedMps     = Snap.WindSpeedMps;
	T.BaroPressureMb   = Snap.BaroPressureMb;
	T.AirTempCelsius   = Snap.AirTempCelsius;
	T.RelativeHumidity = Snap.RelativeHumidity;
	T.Altitude = 0.0;  // ellipsoid height: MSL is ~+33 m here (EGM96), or 0 without the geoid grid
	const TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
	const double MslM = T.Altitude - CamSim::Geospatial::GetGeoidUndulation(T.Latitude, T.Longitude).Get(0.0);

	TestTrue(TEXT("Tag 35 wind from 270"), FMath::IsNearlyEqual(TagUnsigned(Tags, 35) * 360.0 / 65535.0, 270.0, 0.01));
	TestTrue(TEXT("Tag 36 wind 12 m/s"), FMath::IsNearlyEqual(TagUnsigned(Tags, 36) * 100.0 / 255.0, 12.0, 0.2));
	const double Pressure = TagUnsigned(Tags, 37) * 5000.0 / 65535.0;
	const double ExpectedPressure = FKlvBuilder::StaticPressureMb(1000.0f, MslM);
	TestTrue(FString::Printf(TEXT("Tag 37 static pressure %.2f mbar at %.0f m MSL (expected %.2f)"), Pressure, MslM, ExpectedPressure),
		FMath::IsNearlyEqual(Pressure, ExpectedPressure, 0.1) && Pressure < 1000.0 + 0.1);
	TestEqual(TEXT("Tag 39 -12 C (1 byte, signed)"), static_cast<int8>(TagUnsigned(Tags, 39)), static_cast<int8>(-12));
	TestTrue(TEXT("Tag 55 humidity 64 %"), FMath::IsNearlyEqual(TagUnsigned(Tags, 55) * 100.0 / 255.0, 64.0, 0.25));

	// Static pressure follows the ISA troposphere from the sea-level value.
	TestTrue(TEXT("sea level"), FMath::IsNearlyEqual(FKlvBuilder::StaticPressureMb(1013.25f, 0.0), 1013.25f, 0.01f));
	TestTrue(TEXT("5500 m ~ half"), FMath::IsNearlyEqual(FKlvBuilder::StaticPressureMb(1013.25f, 5500.0), 505.0f, 3.0f));
	TestTrue(TEXT("11 km (tropopause) 226.3 mbar"), FMath::IsNearlyEqual(FKlvBuilder::StaticPressureMb(1013.25f, 11000.0), 226.3f, 0.5f));
	return true;
}

// -------------------------------------------------------------------------
// Gap 8: Mission ID (3), Platform Designation (10), Call Sign (59) from config.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlIdentityTagsTest,
	"CamSim.Hitl.Klv.IdentityTags",
	kHitlTestFlags)

bool FHitlIdentityTagsTest::RunTest(const FString& Parameters)
{
	const TCHAR* MissionVar = TEXT("CAMSIM_MISSION_ID");
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(MissionVar, TEXT(""));
		FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	};

	const FString Yaml = TEXT("phase26:\n  mission_id: \"HITL-001\"\n  platform_designation: \"Hooter\"\n  platform_call_sign: \"HOOT21\"\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestEqual(TEXT("mission_id"), Cfg.Phase26.MissionId, FString(TEXT("HITL-001")));
	TestEqual(TEXT("platform_designation"), Cfg.Phase26.PlatformDesignation, FString(TEXT("Hooter")));
	TestEqual(TEXT("platform_call_sign"), Cfg.Phase26.PlatformCallSign, FString(TEXT("HOOT21")));
	FPlatformMisc::SetEnvironmentVar(MissionVar, TEXT("HITL-ENV"));
	TestEqual(TEXT("CAMSIM_MISSION_ID beats YAML"), FCamSimConfig::LoadFromYamlString(Yaml).Phase26.MissionId, FString(TEXT("HITL-ENV")));

	FKlvBuilder::Configure(TEXT("N1"), 0.0f, 0.0f, Cfg.Phase26.MissionId, Cfg.Phase26.PlatformDesignation, Cfg.Phase26.PlatformCallSign);
	TMap<uint8, TArray<uint8>> Tags = BuildTags(BaseTelemetry());
	TestEqual(TEXT("Tag 3"), TagString(Tags, 3), FString(TEXT("HITL-001")));
	TestEqual(TEXT("Tag 10"), TagString(Tags, 10), FString(TEXT("Hooter")));
	TestEqual(TEXT("Tag 59"), TagString(Tags, 59), FString(TEXT("HOOT21")));

	FKlvBuilder::Configure(TEXT("N1"), 0.0f, 0.0f);
	Tags = BuildTags(BaseTelemetry());
	TestFalse(TEXT("Tags 3, 10, 59 omitted when empty"), Tags.Contains(3) || Tags.Contains(10) || Tags.Contains(59));
	return true;
}

// -------------------------------------------------------------------------
// Gaps 9 and 10: packet 201 feeds Tags 8, 9, 64, 79, 80, the ground speed
// (56) and Tag 2; held values expire, and Tag 2 falls back to the sim clock.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlKinematicsTelemetryTest,
	"CamSim.Hitl.Telemetry.HostKinematicsAndTime",
	kHitlTestFlags)

bool FHitlKinematicsTelemetryTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FCamSimTelemetryAssembler Asm;
	Asm.SetPlatformPose(38.8977, -77.0365, 1500.0, 10.0f, 0.0f, 0.0f);

	{
		const FCamSimTelemetry T = Asm.Snapshot(10.0);
		TestFalse(TEXT("no kinematics before packet 201"), T.bHasAirspeed || T.bHasMagneticHeading || T.bHasVelocity);
		TestFalse(TEXT("Tag 2 from the sim clock"), T.bTimestampFromHost);
		const TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
		for (uint8 Tag : { 8, 9, 64, 79, 80 }) TestFalse(FString::Printf(TEXT("Tag %u omitted"), Tag), Tags.Contains(Tag));
	}

	FCigiPlatformKinematics K;
	K.Flags = 0x0F;
	K.TrueAirspeedMps = 42.4f; K.IndicatedAirspeedMps = 38.6f; K.MagneticHeadingDeg = 350.5f;
	K.VelNorthMps = -30.0f; K.VelEastMps = 40.0f; K.VelDownMps = 1.0f;
	K.SampleUtcSec = 1790000000.25;
	Asm.ApplyHostFrame(/*bHasPose=*/true, &K, 100.0);

	FCamSimTelemetry T = Asm.Snapshot(100.0);
	TestTrue(TEXT("Tag 2 from the host"), T.bTimestampFromHost);
	TestEqual(TEXT("Tag 2 = sample_utc"), T.TimestampUs, 1790000000250000ULL);
	TestEqual(TEXT("ground speed from the host velocity"), T.GroundSpeedMps, 50.0f);
	TMap<uint8, TArray<uint8>> Tags = BuildTags(T);
	TestEqual(TEXT("Tag 2 encoded"), TagUnsigned(Tags, 2), 1790000000250000ULL);
	TestEqual(TEXT("Tag 8 true airspeed"), TagUnsigned(Tags, 8), 42ULL);
	TestEqual(TEXT("Tag 9 indicated airspeed"), TagUnsigned(Tags, 9), 39ULL);
	TestTrue(TEXT("Tag 64 magnetic heading"), FMath::IsNearlyEqual(TagUnsigned(Tags, 64) * 360.0 / 65535.0, 350.5, 0.01));
	TestTrue(TEXT("Tag 79 north velocity"), FMath::IsNearlyEqual(static_cast<int16>(TagUnsigned(Tags, 79)) * 327.0 / 32767.0, -30.0, 0.01));
	TestTrue(TEXT("Tag 80 east velocity"), FMath::IsNearlyEqual(static_cast<int16>(TagUnsigned(Tags, 80)) * 327.0 / 32767.0, 40.0, 0.01));
	TestEqual(TEXT("Tag 56 ground speed"), TagUnsigned(Tags, 56), 50ULL);

	// A frame without a new pose: the host time carried forward by the engine frame time.
	T = Asm.Snapshot(100.0 + 1.0 / 30.0);
	TestTrue(FString::Printf(TEXT("Tag 2 advances by one frame (%llu)"), T.TimestampUs),
		FMath::Abs(static_cast<int64>(T.TimestampUs - (1790000000250000ULL + 33333ULL))) <= 1);

	// Host jitter: the next sample is 2 ms earlier than the time carried forward. Tag 2 never steps back.
	K.SampleUtcSec = 1790000000.25 + 1.0 / 30.0 - 0.002;
	Asm.ApplyHostFrame(true, &K, 100.0 + 2.0 / 30.0);
	const uint64 Prev = T.TimestampUs;
	T = Asm.Snapshot(100.0 + 2.0 / 30.0);
	TestTrue(TEXT("Tag 2 monotonic across host jitter"), T.TimestampUs > Prev);

	// Kinematics expire when the host stops sending them.
	T = Asm.Snapshot(100.0 + FCamSimTelemetryAssembler::KinematicsTimeoutSec + 0.5);
	TestFalse(TEXT("stale kinematics dropped"), T.bHasAirspeed || T.bHasMagneticHeading || T.bHasVelocity);

	// A pose without packet 201 (or without its sample-time flag): back to the sim clock.
	Asm.ApplyHostFrame(true, nullptr, 200.0);
	T = Asm.Snapshot(200.0);
	TestFalse(TEXT("Tag 2 falls back to the sim clock"), T.bTimestampFromHost);

	// Valid flags gate each group.
	K.Flags = FCigiPlatformKinematics::FlagMagneticHeading;
	Asm.ApplyHostFrame(true, &K, 300.0);
	T = Asm.Snapshot(300.0);
	TestTrue(TEXT("only the flagged group"), T.bHasMagneticHeading && !T.bHasAirspeed && !T.bHasVelocity && !T.bTimestampFromHost);
	return true;
}

// -------------------------------------------------------------------------
// Gap 11: image corners (Tags 82-89) from the four corner rays; corners
// above the horizon are left out.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlFrameCornersTest,
	"CamSim.Hitl.Telemetry.FrameCorners",
	kHitlTestFlags)

bool FHitlFrameCornersTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	const double Alt = 1000.0;
	const float HFov = 60.0f;
	const float VFov = CamSimTelemetry::VerticalFovDeg(HFov, 1920, 1080);
	const double HalfW = Alt * FMath::Tan(FMath::DegreesToRadians(HFov * 0.5));
	const double HalfH = Alt * FMath::Tan(FMath::DegreesToRadians(VFov * 0.5));

	// Nadir, heading north: image top = north, image right = east.
	FCamSimTelemetryAssembler Asm;
	Asm.SetPlatformPose(10.0, 20.0, Alt, 0.0f, 0.0f, 0.0f);
	Asm.SetGimbal(0.0f, -90.0f, 0.0f);
	Asm.SetFieldOfView(HFov, VFov);
	Asm.UpdateFootprint(nullptr, nullptr, nullptr, nullptr);
	const FCamSimTelemetry& T = Asm.Get();
	TestEqual(TEXT("all four corners on the ground"), T.CornerValidMask, static_cast<uint8>(0x0F));

	const FVector Expected[4] = {
		FVector( HalfH, -HalfW, 0.0),  // 1 upper left  = north-west
		FVector( HalfH,  HalfW, 0.0),  // 2 upper right = north-east
		FVector(-HalfH,  HalfW, 0.0),  // 3 lower right = south-east
		FVector(-HalfH, -HalfW, 0.0),  // 4 lower left  = south-west
	};
	for (int32 i = 0; i < 4; ++i)
	{
		const FVector Got = OffsetNeu(T, T.CornerLat[i], T.CornerLon[i]);
		TestTrue(FString::Printf(TEXT("corner %d at N %.1f E %.1f (expected N %.1f E %.1f)"), i + 1, Got.X, Got.Y, Expected[i].X, Expected[i].Y),
			FMath::IsNearlyEqual(Got.X, Expected[i].X, 0.5) && FMath::IsNearlyEqual(Got.Y, Expected[i].Y, 0.5));
	}

	TMap<uint8, TArray<uint8>> Tags = BuildTags(Asm.Snapshot(0.0));
	for (uint8 Tag = 82; Tag <= 89; ++Tag)
	{
		TestTrue(FString::Printf(TEXT("Tag %u present, 4 bytes"), Tag), Tags.Contains(Tag) && Tags[Tag].Num() == 4);
	}
	const double Lat1 = static_cast<int32>(TagUnsigned(Tags, 82)) * 90.0 / 2147483647.0;
	const double Lon3 = static_cast<int32>(TagUnsigned(Tags, 87)) * 180.0 / 2147483647.0;
	TestTrue(TEXT("Tag 82 = corner 1 latitude"), FMath::IsNearlyEqual(Lat1, T.CornerLat[0], 1e-7));
	TestTrue(TEXT("Tag 87 = corner 3 longitude"), FMath::IsNearlyEqual(Lon3, T.CornerLon[2], 1e-7));

	// Level boresight: the upper corners look above the horizon and are left out.
	Asm.SetGimbal(0.0f, 0.0f, 0.0f);
	Asm.UpdateFootprint(nullptr, nullptr, nullptr, nullptr);
	TestEqual(TEXT("level: only the lower corners (3, 4) see the ground"), Asm.Get().CornerValidMask, static_cast<uint8>(0x0C));
	TestEqual(TEXT("level: no frame centre"), Asm.Get().SlantRangeM, 0.0);
	Tags = BuildTags(Asm.Snapshot(0.0));
	TestFalse(TEXT("Tags 82-85 omitted"), Tags.Contains(82) || Tags.Contains(83) || Tags.Contains(84) || Tags.Contains(85));
	TestTrue(TEXT("Tags 86-89 sent"), Tags.Contains(86) && Tags.Contains(87) && Tags.Contains(88) && Tags.Contains(89));

	// Looking up: nothing.
	Asm.SetGimbal(0.0f, 45.0f, 0.0f);
	Asm.UpdateFootprint(nullptr, nullptr, nullptr, nullptr);
	TestEqual(TEXT("looking up: no corners"), Asm.Get().CornerValidMask, static_cast<uint8>(0));
	return true;
}

// -------------------------------------------------------------------------
// Gap 12: CAMSIM_* overrides for camera_entity_id, gimbal_* and sensor_fov_presets.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHitlCameraEnvOverridesTest,
	"CamSim.Hitl.Config.CameraEnvOverrides",
	kHitlTestFlags)

bool FHitlCameraEnvOverridesTest::RunTest(const FString& Parameters)
{
	const TCHAR* Vars[] = { TEXT("CAMSIM_CAMERA_ENTITY_ID"), TEXT("CAMSIM_GIMBAL_MAX_SLEW_RATE"),
		TEXT("CAMSIM_GIMBAL_PITCH_MIN"), TEXT("CAMSIM_GIMBAL_PITCH_MAX"), TEXT("CAMSIM_GIMBAL_YAW_MIN"),
		TEXT("CAMSIM_GIMBAL_YAW_MAX"), TEXT("CAMSIM_SENSOR_FOV_PRESETS") };
	ON_SCOPE_EXIT { for (const TCHAR* V : Vars) FPlatformMisc::SetEnvironmentVar(V, TEXT("")); };

	const FString Yaml = TEXT("camera_entity_id: 3\ngimbal_max_slew_rate: 40\ngimbal_pitch_max: 30\nsensor_fov_presets: [60, 20, 5]\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestEqual(TEXT("YAML camera_entity_id"), Cfg.CameraEntityId, 3);
	TestEqual(TEXT("YAML presets"), Cfg.SensorFovPresets.Num(), 3);

	FPlatformMisc::SetEnvironmentVar(Vars[0], TEXT("1"));
	FPlatformMisc::SetEnvironmentVar(Vars[1], TEXT("0"));
	FPlatformMisc::SetEnvironmentVar(Vars[2], TEXT("-110"));
	FPlatformMisc::SetEnvironmentVar(Vars[3], TEXT("90"));
	FPlatformMisc::SetEnvironmentVar(Vars[4], TEXT("-170"));
	FPlatformMisc::SetEnvironmentVar(Vars[5], TEXT("170"));
	FPlatformMisc::SetEnvironmentVar(Vars[6], TEXT("none"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestEqual(TEXT("CAMSIM_CAMERA_ENTITY_ID"), Cfg.CameraEntityId, 1);
	TestEqual(TEXT("CAMSIM_GIMBAL_MAX_SLEW_RATE"), Cfg.GimbalMaxSlewRateDegPerSec, 0.0f);
	TestEqual(TEXT("CAMSIM_GIMBAL_PITCH_MIN"), Cfg.GimbalPitchMin, -110.0f);
	TestEqual(TEXT("CAMSIM_GIMBAL_PITCH_MAX"), Cfg.GimbalPitchMax, 90.0f);
	TestEqual(TEXT("CAMSIM_GIMBAL_YAW_MIN"), Cfg.GimbalYawMin, -170.0f);
	TestEqual(TEXT("CAMSIM_GIMBAL_YAW_MAX"), Cfg.GimbalYawMax, 170.0f);
	TestEqual(TEXT("CAMSIM_SENSOR_FOV_PRESETS=none disables the presets"), Cfg.SensorFovPresets.Num(), 0);

	for (const TCHAR* Off : { TEXT("[]"), TEXT("off"), TEXT("OFF") })
	{
		FPlatformMisc::SetEnvironmentVar(Vars[6], Off);
		TestEqual(FString::Printf(TEXT("CAMSIM_SENSOR_FOV_PRESETS=%s disables"), Off),
			FCamSimConfig::LoadFromYamlString(Yaml).SensorFovPresets.Num(), 0);
	}
	FPlatformMisc::SetEnvironmentVar(Vars[6], TEXT("[45, 10.5 ,2]"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	if (TestEqual(TEXT("comma-separated presets"), Cfg.SensorFovPresets.Num(), 3))
	{
		TestEqual(TEXT("preset 0"), Cfg.SensorFovPresets[0], 45.0f);
		TestEqual(TEXT("preset 1"), Cfg.SensorFovPresets[1], 10.5f);
		TestEqual(TEXT("preset 2"), Cfg.SensorFovPresets[2], 2.0f);
	}
	return true;
}
