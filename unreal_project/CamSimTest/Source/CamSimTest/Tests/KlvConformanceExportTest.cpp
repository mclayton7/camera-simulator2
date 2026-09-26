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
		float            GateW = 0.0f;
		float            GateH = 0.0f;
	};

	FString ToJson(const FKlvCase& Case, const TArray<uint8>& Packet)
	{
		const FCamSimTelemetry& T = Case.Telemetry;
		return FString::Printf(
			TEXT("{\"name\":\"%s\",\"hex\":\"%s\",\"tail\":\"%s\",\"gate\":[%.1f,%.1f],\"telemetry\":{"
			     "\"timestampUs\":%llu,\"yaw\":%.6f,\"pitch\":%.6f,\"roll\":%.6f,"
			     "\"lat\":%.9f,\"lon\":%.9f,\"alt\":%.3f,\"hfov\":%.6f,\"vfov\":%.6f,"
			     "\"gimbalYaw\":%.6f,\"gimbalPitch\":%.6f,\"gimbalRoll\":%.6f,\"slantRange\":%.3f,"
			     "\"fcLat\":%.9f,\"fcLon\":%.9f,\"fcElev\":%.3f,\"groundSpeed\":%.3f,"
			     "\"sensorMode\":%d,\"polarity\":%d}}"),
			*Case.Name, *BytesToHex(Packet.GetData(), Packet.Num()).ToLower(), *Case.Tail,
			Case.GateW, Case.GateH,
			T.TimestampUs, T.Yaw, T.Pitch, T.Roll,
			T.Latitude, T.Longitude, T.Altitude, T.HFovDeg, T.VFovDeg,
			T.GimbalYaw, T.GimbalPitch, T.GimbalRoll, T.SlantRangeM,
			T.FrameCenterLat, T.FrameCenterLon, T.FrameCenterElev, T.GroundSpeedMps,
			T.SensorMode, T.SensorPolarity);
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

	// Tag 48 content is process-global; set it so the export is deterministic.
	FKlvBuilder::SetSecurityMetadata(TEXT("UNCLASSIFIED"), TEXT("//US"), TEXT("US"));

	TArray<FString> Lines;
	for (const FKlvCase& Case : Cases)
	{
		FKlvBuilder::Configure(Case.Tail, Case.GateW, Case.GateH);
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
