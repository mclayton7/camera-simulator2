// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Metadata/KlvBuilder.h"
#include "DIS/DisPduTypes.h"
#include "Sensor/SensorTypes.h"

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
