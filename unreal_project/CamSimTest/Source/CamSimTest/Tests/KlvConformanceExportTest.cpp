// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// Exports KLV packets built from known telemetry so the misb.js conformance
// check (scripts/klv_conformance/check.js) can decode them with the reference
// decoder and compare every tag against the input values.
//
// Output: <Project>/Saved/KlvConformance/packets.jsonl, one JSON object per
// line: { "name", "hex", "gate": [w, h], "tail", "telemetry": { ... } }.
// -------------------------------------------------------------------------

namespace
{
	struct FKlvCase
	{
		FString          Name;
		FCamSimTelemetry Telemetry;
		FString          Tail;
		FString          Mission;
		FString          Designation;
		FString          CallSign;
		bool             bFullRangeAttitude = false;
		float            GateW = 0.0f;
		float            GateH = 0.0f;
	};

	FString ToJson(const FKlvCase& Case, const TArray<uint8>& Packet)
	{
		const FCamSimTelemetry& T = Case.Telemetry;
		// Corners: [lat, lon] for each corner on the ground, null otherwise.
		FString Corners;
		for (int32 i = 0; i < 4; ++i)
		{
			Corners += (i ? TEXT(",") : TEXT(""));
			Corners += (T.CornerValidMask & (1u << i))
				? FString::Printf(TEXT("[%.9f,%.9f]"), T.CornerLat[i], T.CornerLon[i]) : FString(TEXT("null"));
		}
		const FString Hitl = FString::Printf(
			TEXT("\"hasAirspeed\":%s,\"tas\":%.3f,\"ias\":%.3f,\"hasMagHeading\":%s,\"magHeading\":%.6f,"
			     "\"hasVelocity\":%s,\"velN\":%.3f,\"velE\":%.3f,"
			     "\"hasAtmosphere\":%s,\"windDir\":%.3f,\"windSpeed\":%.3f,\"baroMb\":%.3f,\"airTemp\":%.3f,"
			     "\"humidity\":%.4f,\"corners\":[%s]"),
			T.bHasAirspeed ? TEXT("true") : TEXT("false"), T.TrueAirspeedMps, T.IndicatedAirspeedMps,
			T.bHasMagneticHeading ? TEXT("true") : TEXT("false"), T.MagneticHeadingDeg,
			T.bHasVelocity ? TEXT("true") : TEXT("false"), T.VelNorthMps, T.VelEastMps,
			T.bHasAtmosphere ? TEXT("true") : TEXT("false"), T.WindDirectionDeg, T.WindSpeedMps, T.BaroPressureMb,
			T.AirTempCelsius, T.RelativeHumidity, *Corners);
		return FString::Printf(
			TEXT("{\"name\":\"%s\",\"hex\":\"%s\",\"tail\":\"%s\",\"mission\":\"%s\",\"designation\":\"%s\","
			     "\"callSign\":\"%s\",\"fullRangeAttitude\":%s,\"gate\":[%.1f,%.1f],\"telemetry\":{"
			     "\"timestampUs\":%llu,\"yaw\":%.6f,\"pitch\":%.6f,\"roll\":%.6f,"
			     "\"lat\":%.9f,\"lon\":%.9f,\"alt\":%.3f,\"hfov\":%.6f,\"vfov\":%.6f,"
			     "\"gimbalYaw\":%.6f,\"gimbalPitch\":%.6f,\"gimbalRoll\":%.6f,\"slantRange\":%.3f,"
			     "\"fcLat\":%.9f,\"fcLon\":%.9f,\"fcElev\":%.3f,\"groundSpeed\":%.3f,"
			     "\"sensorMode\":%d,\"polarity\":%d,%s}}"),
			*Case.Name, *BytesToHex(Packet.GetData(), Packet.Num()).ToLower(), *Case.Tail,
			*Case.Mission, *Case.Designation, *Case.CallSign, Case.bFullRangeAttitude ? TEXT("true") : TEXT("false"),
			Case.GateW, Case.GateH,
			T.TimestampUs, T.Yaw, T.Pitch, T.Roll,
			T.Latitude, T.Longitude, T.Altitude, T.HFovDeg, T.VFovDeg,
			T.GimbalYaw, T.GimbalPitch, T.GimbalRoll, T.SlantRangeM,
			T.FrameCenterLat, T.FrameCenterLon, T.FrameCenterElev, T.GroundSpeedMps,
			T.SensorMode, T.SensorPolarity, *Hitl);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvConformanceExportTest,
	"CamSim.KlvConformance.ExportPackets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvConformanceExportTest::RunTest(const FString& Parameters)
{
	TArray<FKlvCase> Cases;

	{
		FKlvCase C;
		C.Name = TEXT("eo_nominal");
		FCamSimTelemetry& T = C.Telemetry;
		T.TimestampUs = 1790000000000000ULL; // 2026-09-21
		T.Yaw = 200.0f; T.Pitch = -5.0f; T.Roll = 3.0f;
		T.Latitude = 38.8977; T.Longitude = -77.0365; T.Altitude = 1500.0;
		T.HFovDeg = 20.0f; T.VFovDeg = 11.25f;
		T.GimbalYaw = 30.0f; T.GimbalPitch = -45.0f; T.GimbalRoll = 0.0f;
		T.SlantRangeM = 2100.0;
		T.FrameCenterLat = 38.88; T.FrameCenterLon = -77.02; T.FrameCenterElev = 20.0;
		T.GroundSpeedMps = 40.0f;
		Cases.Add(C);
	}
	{
		FKlvCase C;
		C.Name = TEXT("ir_black_hot_with_gate_and_tail");
		C.Tail = TEXT("CAMSIM-01");
		C.GateW = 64.0f; C.GateH = 48.0f;
		FCamSimTelemetry& T = C.Telemetry;
		T.TimestampUs = 1790000123456789ULL;
		T.Yaw = 359.5f; T.Pitch = 12.0f; T.Roll = -30.0f;
		T.Latitude = -33.8688; T.Longitude = 151.2093; T.Altitude = 6000.0;
		T.HFovDeg = 2.5f; T.VFovDeg = 1.4f;
		T.GimbalYaw = 270.0f; T.GimbalPitch = -80.0f; T.GimbalRoll = 5.0f;
		T.SlantRangeM = 6100.0;
		T.FrameCenterLat = -33.87; T.FrameCenterLon = 151.21; T.FrameCenterElev = 58.0;
		T.GroundSpeedMps = 120.0f;
		T.SensorMode = 1; T.SensorPolarity = 1;
		Cases.Add(C);
	}
	{
		FKlvCase C;
		C.Name = TEXT("above_horizon_no_range_no_speed");
		FCamSimTelemetry& T = C.Telemetry;
		T.TimestampUs = 1790000500000000ULL;
		T.Yaw = 0.0f; T.Latitude = 64.1466; T.Longitude = -21.9426; T.Altitude = 300.0;
		T.HFovDeg = 60.0f; T.VFovDeg = 33.75f; T.GimbalPitch = 10.0f;
		T.SensorMode = 2;
		Cases.Add(C);
	}

	{
		// HITL (Hooter): identity strings, host kinematics (packet 201), host
		// atmosphere, a steep bank beyond Tags 6/7 (Tags 90/91), and the corners.
		FKlvCase C;
		C.Name = TEXT("hitl_kinematics_weather_corners");
		C.Tail = TEXT("HOOTER-01");
		C.Mission = TEXT("HITL-2026-10-01");
		C.Designation = TEXT("Hooter");
		C.CallSign = TEXT("HOOT21");
		C.bFullRangeAttitude = true;  // phase26.klv_full_range_attitude on (off in the other cases)
		FCamSimTelemetry& T = C.Telemetry;
		T.TimestampUs = 1790000222333444ULL;
		T.Yaw = 95.0f; T.Pitch = 27.5f; T.Roll = -62.0f;
		T.Latitude = 35.0527; T.Longitude = -118.1512; T.Altitude = 1200.0;
		T.HFovDeg = 30.0f; T.VFovDeg = 17.06f;
		T.GimbalYaw = 270.0f; T.GimbalPitch = -30.0f;
		T.SlantRangeM = 2300.0;
		T.FrameCenterLat = 35.06; T.FrameCenterLon = -118.17; T.FrameCenterElev = 800.0;
		T.GroundSpeedMps = 31.0f;
		T.bHasAirspeed = true; T.TrueAirspeedMps = 33.4f; T.IndicatedAirspeedMps = 31.6f;
		T.bHasMagneticHeading = true; T.MagneticHeadingDeg = 83.2f;
		T.bHasVelocity = true; T.VelNorthMps = -2.5f; T.VelEastMps = 30.9f;
		T.bHasAtmosphere = true; T.WindDirectionDeg = 240.0f; T.WindSpeedMps = 7.5f; T.BaroPressureMb = 1016.0f;
		T.AirTempCelsius = 24.0f; T.RelativeHumidity = 0.35f;
		T.CornerValidMask = 0x0D;  // corner 2 (upper right) above the horizon
		const double Corners[4][2] = { { 35.071, -118.192 }, { 0.0, 0.0 }, { 35.052, -118.171 }, { 35.048, -118.190 } };
		for (int32 i = 0; i < 4; ++i) { T.CornerLat[i] = Corners[i][0]; T.CornerLon[i] = Corners[i][1]; }
		Cases.Add(C);
	}

	// Tag 48 content is process-global; set it so the export is deterministic.
	FKlvBuilder::SetSecurityMetadata(TEXT("UNCLASSIFIED"), TEXT("//US"), TEXT("US"));

	TArray<FString> Lines;
	for (const FKlvCase& Case : Cases)
	{
		FKlvBuilder::Configure(Case.Tail, Case.GateW, Case.GateH, Case.Mission, Case.Designation, Case.CallSign, Case.bFullRangeAttitude);
		Lines.Add(ToJson(Case, FKlvBuilder::BuildMisbST0601(Case.Telemetry)));
	}
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);

	const FString OutPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("KlvConformance/packets.jsonl"));
	const bool bSaved = FFileHelper::SaveStringArrayToFile(Lines, *OutPath);
	TestTrue(FString::Printf(TEXT("Wrote %s"), *OutPath), bSaved);
	AddInfo(FString::Printf(TEXT("KLV conformance packets: %s"), *OutPath));
	return bSaved;
}
