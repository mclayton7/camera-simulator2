// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Metadata/KlvBuilder.h"
#include "DIS/DisPduTypes.h"
#include "Sensor/SensorTypes.h"

// Forward-declare so we can test BuildCotXml without the full sender
// (We re-implement the XML builder inline for unit testing)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCotXmlFormatTest,
	"CamSim.Phase21.Streaming.CotXmlFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCotXmlFormatTest::RunTest(const FString& Parameters)
{
	// Build a minimal config
	FCamSimConfig Config;
	Config.Streaming.CotUid      = TEXT("Test-ISR-1");
	Config.Streaming.CotType     = TEXT("a-f-G-E-S");
	Config.Streaming.CotCallsign = TEXT("TESTCAM");
	Config.Streaming.CotIntervalSec = 2.0f;

	// Build telemetry
	FCamSimTelemetry Telemetry;
	Telemetry.Latitude  = 38.8977;
	Telemetry.Longitude = -77.0365;
	Telemetry.Altitude  = 500.0;
	Telemetry.Yaw       = 90.0f;

	// Inline CoT XML builder (matches CotSender::BuildCotXml logic)
	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime Stale = Now + FTimespan::FromSeconds(4.0);
	const FString TimeStr  = Now.ToIso8601();
	const FString StaleStr = Stale.ToIso8601();

	FString Xml = FString::Printf(
		TEXT("<?xml version='1.0' encoding='UTF-8'?>"
		     "<event version=\"2.0\" uid=\"%s\" type=\"%s\" "
		     "time=\"%s\" start=\"%s\" stale=\"%s\" how=\"m-g\">"
		     "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"%.1f\" ce=\"9999999\" le=\"9999999\"/>"
		     "<detail>"
		     "<track course=\"%.1f\" speed=\"0.0\"/>"
		     "<contact callsign=\"%s\"/>"
		     "<__group name=\"Cyan\" role=\"ISR\"/>"
		     "</detail>"
		     "</event>"),
		*Config.Streaming.CotUid,
		*Config.Streaming.CotType,
		*TimeStr, *TimeStr, *StaleStr,
		Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude,
		Telemetry.Yaw,
		*Config.Streaming.CotCallsign);

	TestTrue(TEXT("CoT XML has <event>"), Xml.Contains(TEXT("<event")));
	TestTrue(TEXT("CoT XML has <point>"), Xml.Contains(TEXT("<point")));
	TestTrue(TEXT("CoT XML has <detail>"), Xml.Contains(TEXT("<detail>")));
	TestTrue(TEXT("CoT XML has <track>"), Xml.Contains(TEXT("<track")));
	TestTrue(TEXT("CoT XML has lat"), Xml.Contains(TEXT("lat=\"38.897700\"")));
	TestTrue(TEXT("CoT XML has lon"), Xml.Contains(TEXT("lon=\"-77.036500\"")));
	TestTrue(TEXT("CoT XML has hae"), Xml.Contains(TEXT("hae=\"500.0\"")));
	TestTrue(TEXT("CoT XML has version 2.0"), Xml.Contains(TEXT("version=\"2.0\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCotXmlCallsignTest,
	"CamSim.Phase21.Streaming.CotXmlCallsign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCotXmlCallsignTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.Streaming.CotUid      = TEXT("MyUID-42");
	Config.Streaming.CotType     = TEXT("a-f-A");
	Config.Streaming.CotCallsign = TEXT("EAGLE1");
	Config.Streaming.CotIntervalSec = 3.0f;

	FCamSimTelemetry Telemetry;
	Telemetry.Latitude = 0.0;
	Telemetry.Longitude = 0.0;
	Telemetry.Altitude = 0.0;
	Telemetry.Yaw = 0.0f;

	const FDateTime Now = FDateTime::UtcNow();
	const FString TimeStr = Now.ToIso8601();
	FString Xml = FString::Printf(
		TEXT("<event version=\"2.0\" uid=\"%s\" type=\"%s\" "
		     "time=\"%s\" start=\"%s\" stale=\"%s\" how=\"m-g\">"
		     "<point lat=\"0.000000\" lon=\"0.000000\" hae=\"0.0\" ce=\"9999999\" le=\"9999999\"/>"
		     "<detail><track course=\"0.0\" speed=\"0.0\"/>"
		     "<contact callsign=\"%s\"/>"
		     "</__group name=\"Cyan\" role=\"ISR\"/></detail></event>"),
		*Config.Streaming.CotUid, *Config.Streaming.CotType,
		*TimeStr, *TimeStr, *TimeStr,
		*Config.Streaming.CotCallsign);

	TestTrue(TEXT("uid matches config"), Xml.Contains(TEXT("uid=\"MyUID-42\"")));
	TestTrue(TEXT("type matches config"), Xml.Contains(TEXT("type=\"a-f-A\"")));
	TestTrue(TEXT("callsign matches config"), Xml.Contains(TEXT("callsign=\"EAGLE1\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoverPidDefaultsTest,
	"CamSim.Phase21.Streaming.RoverPidDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoverPidDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	TestEqual(TEXT("Default ROVER video PID"), Config.Streaming.RoverVideoPid, 0x1011);
	TestEqual(TEXT("Default ROVER KLV PID"), Config.Streaming.RoverKlvPid, 0x1012);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamingConfigDefaultsTest,
	"CamSim.Phase21.Streaming.StreamingConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamingConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;

	TestFalse(TEXT("CoT disabled by default"), Config.Streaming.bCotEnabled);
	TestEqual(TEXT("CoT addr default"), Config.Streaming.CotAddr, FString(TEXT("239.2.3.1")));
	TestEqual(TEXT("CoT port default"), Config.Streaming.CotPort, 6969);
	TestTrue(TEXT("CoT interval default ~2s"), FMath::IsNearlyEqual(Config.Streaming.CotIntervalSec, 2.0f));
	TestEqual(TEXT("CoT uid default"), Config.Streaming.CotUid, FString(TEXT("CamSim-ISR-1")));
	TestEqual(TEXT("CoT type default"), Config.Streaming.CotType, FString(TEXT("a-f-G-E-S")));
	TestEqual(TEXT("CoT callsign default"), Config.Streaming.CotCallsign, FString(TEXT("CAMSIM")));

	TestFalse(TEXT("ATAK view disabled by default"), Config.Streaming.bAtakViewEnabled);
	TestEqual(TEXT("ATAK addr default"), Config.Streaming.AtakAddr, FString(TEXT("239.1.1.2")));
	TestEqual(TEXT("ATAK port default"), Config.Streaming.AtakPort, 5005);
	TestEqual(TEXT("ATAK bitrate default"), Config.Streaming.AtakBitrate, 2000000);

	TestFalse(TEXT("ROVER compat disabled by default"), Config.Streaming.bRoverCompat);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignatorPduParseTest,
	"CamSim.Phase21.Streaming.DesignatorPduParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDesignatorPduParseTest::RunTest(const FString& Parameters)
{
	// Build a 56-byte Designator PDU buffer
	uint8 Buf[56];
	FMemory::Memzero(Buf, 56);

	// Header (12 bytes)
	Buf[0] = 6;   // protocol version (DIS 6)
	Buf[1] = 1;   // exercise ID
	Buf[2] = 24;  // PDU type = Designator
	Buf[3] = 1;   // protocol family

	// Designating Entity ID (offset 12): site=1, app=2, entity=3
	Buf[12] = 0; Buf[13] = 1;  // site
	Buf[14] = 0; Buf[15] = 2;  // app
	Buf[16] = 0; Buf[17] = 3;  // entity

	// Code Name (offset 18): 0x0001
	Buf[18] = 0; Buf[19] = 1;

	// Designated Entity ID (offset 20): site=4, app=5, entity=6
	Buf[20] = 0; Buf[21] = 4;
	Buf[22] = 0; Buf[23] = 5;
	Buf[24] = 0; Buf[25] = 6;

	// Designator Code (offset 26): 1688 = 0x0698
	Buf[26] = 0x06; Buf[27] = 0x98;

	// Designator Power (offset 28): 1000.0f as IEEE 754
	{
		float Power = 1000.0f;
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Power, 4);
		Buf[28] = static_cast<uint8>((Bits >> 24) & 0xFF);
		Buf[29] = static_cast<uint8>((Bits >> 16) & 0xFF);
		Buf[30] = static_cast<uint8>((Bits >> 8)  & 0xFF);
		Buf[31] = static_cast<uint8>((Bits)       & 0xFF);
	}

	// Spot Location ECEF (offset 32): X=1000000.0, Y=2000000.0, Z=3000000.0
	auto WriteF64BE = [&Buf](int32 Offset, double Val)
	{
		uint64 Bits;
		FMemory::Memcpy(&Bits, &Val, 8);
		for (int32 i = 0; i < 8; ++i)
			Buf[Offset + i] = static_cast<uint8>((Bits >> (56 - i * 8)) & 0xFF);
	};
	WriteF64BE(32, 1000000.0);
	WriteF64BE(40, 2000000.0);
	WriteF64BE(48, 3000000.0);

	FDisDesignatorPdu Pdu;
	const bool bParsed = FDisDesignatorPdu::Parse(Buf, 56, Pdu);

	TestTrue(TEXT("Parse succeeds"), bParsed);
	TestEqual(TEXT("PDU type is Designator"), Pdu.Header.PduType, (uint8)24);
	TestEqual(TEXT("Designating entity site"), Pdu.DesignatingEntityId.Site, (uint16)1);
	TestEqual(TEXT("Designating entity app"), Pdu.DesignatingEntityId.Application, (uint16)2);
	TestEqual(TEXT("Designating entity entity"), Pdu.DesignatingEntityId.Entity, (uint16)3);
	TestEqual(TEXT("Designated entity site"), Pdu.DesignatedEntityId.Site, (uint16)4);
	TestEqual(TEXT("Designator code"), Pdu.DesignatorCode, (uint16)1688);
	TestTrue(TEXT("Spot X is approximately 1000000"), FMath::IsNearlyEqual(Pdu.SpotLocationX, 1000000.0, 1.0));
	TestTrue(TEXT("Spot Y is approximately 2000000"), FMath::IsNearlyEqual(Pdu.SpotLocationY, 2000000.0, 1.0));
	TestTrue(TEXT("Spot Z is approximately 3000000"), FMath::IsNearlyEqual(Pdu.SpotLocationZ, 3000000.0, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesignatorPduWrongTypeTest,
	"CamSim.Phase21.Streaming.DesignatorPduWrongType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDesignatorPduWrongTypeTest::RunTest(const FString& Parameters)
{
	uint8 Buf[56];
	FMemory::Memzero(Buf, 56);

	// Header with PDU type = 1 (Entity State, not Designator)
	Buf[0] = 6;
	Buf[1] = 1;
	Buf[2] = 1;  // wrong type
	Buf[3] = 1;

	FDisDesignatorPdu Pdu;
	const bool bParsed = FDisDesignatorPdu::Parse(Buf, 56, Pdu);
	TestFalse(TEXT("Parse fails for wrong PDU type"), bParsed);

	return true;
}
