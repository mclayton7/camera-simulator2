// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// Phase 26: Standards Compliance Tests
//
// Validates new MISB ST 0601.9 tags, BCC-16 checksum, and MapGroundSpeed.
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// MapGroundSpeed
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26MapGroundSpeedTest,
	"CamSim.Phase26.MapGroundSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26MapGroundSpeedTest::RunTest(const FString& Parameters)
{
	// Zero speed
	TestEqual(TEXT("Speed 0 -> 0"), FKlvBuilder::MapGroundSpeed(0.0f), static_cast<uint8>(0));

	// Normal speed
	TestEqual(TEXT("Speed 50 -> 50"), FKlvBuilder::MapGroundSpeed(50.0f), static_cast<uint8>(50));

	// Max clamp at 255
	TestEqual(TEXT("Speed 300 clamped -> 255"), FKlvBuilder::MapGroundSpeed(300.0f), static_cast<uint8>(255));

	// Negative clamp
	TestEqual(TEXT("Speed -10 clamped -> 0"), FKlvBuilder::MapGroundSpeed(-10.0f), static_cast<uint8>(0));

	// Rounding: 99.6 -> 100
	TestEqual(TEXT("Speed 99.6 rounds to 100"), FKlvBuilder::MapGroundSpeed(99.6f), static_cast<uint8>(100));

	// NaN guard
	TestEqual(TEXT("Speed NaN -> 0"), FKlvBuilder::MapGroundSpeed(std::numeric_limits<float>::quiet_NaN()),
		static_cast<uint8>(0));

	return true;
}

// -------------------------------------------------------------------------
// ComputeBcc16
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26ComputeBcc16Test,
	"CamSim.Phase26.ComputeBcc16",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26ComputeBcc16Test::RunTest(const FString& Parameters)
{
	// Known BCC-16 test: alternating byte accumulation
	// Data: {0x01, 0x02, 0x03, 0x04}
	// Byte 0 (even): Bcc += 0x01 << 8 = 0x0100
	// Byte 1 (odd):  Bcc += 0x02 << 0 = 0x0102
	// Byte 2 (even): Bcc += 0x03 << 8 = 0x0402
	// Byte 3 (odd):  Bcc += 0x04 << 0 = 0x0406
	const uint8 Data[] = { 0x01, 0x02, 0x03, 0x04 };
	const uint16 Expected = 0x0406;
	TestEqual(TEXT("BCC-16 {01,02,03,04}"), FKlvBuilder::ComputeBcc16(Data, 4), Expected);

	// Empty data
	TestEqual(TEXT("BCC-16 empty"), FKlvBuilder::ComputeBcc16(nullptr, 0), static_cast<uint16>(0));

	// Single byte
	const uint8 Single[] = { 0xAB };
	TestEqual(TEXT("BCC-16 single byte"), FKlvBuilder::ComputeBcc16(Single, 1), static_cast<uint16>(0xAB00));

	return true;
}

// -------------------------------------------------------------------------
// ComputeCrc16 (existing, but now public — verify accessible)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26ComputeCrc16PublicTest,
	"CamSim.Phase26.ComputeCrc16Public",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26ComputeCrc16PublicTest::RunTest(const FString& Parameters)
{
	// Known CRC-16/CCITT value for "123456789"
	const uint8 Data[] = { '1','2','3','4','5','6','7','8','9' };
	const uint16 Crc = FKlvBuilder::ComputeCrc16(Data, 9);
	// CRC-16/CCITT of "123456789" with init=0xFFFF, poly=0x1021 → 0x29B1
	TestEqual(TEXT("CRC-16 of '123456789'"), Crc, static_cast<uint16>(0x29B1));

	return true;
}

// -------------------------------------------------------------------------
// Tag 4 — Platform Tail Number presence
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26TailNumberTest,
	"CamSim.Phase26.TailNumber",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26TailNumberTest::RunTest(const FString& Parameters)
{
	// Configure with a tail number
	FKlvBuilder::Configure(TEXT("crc16"), TEXT("N12345"), 0.0f, 0.0f);

	FCamSimTelemetry T;
	T.TimestampUs = 1000000000000000ULL;
	TArray<uint8> Packet = FKlvBuilder::BuildMisbST0601(T);

	// Tag 4 should be present — scan for tag byte 4
	bool bFoundTag4 = false;
	// Skip UL (16 bytes) + BER length (variable), scan TLV payload
	for (int32 i = 17; i < Packet.Num() - 4; ++i)
	{
		if (Packet[i] == 4 && i > 16)
		{
			// Check if this looks like a tag (next byte = length, reasonable)
			const uint8 Len = Packet[i + 1];
			if (Len > 0 && Len <= 127 && i + 2 + Len <= Packet.Num())
			{
				bFoundTag4 = true;
				break;
			}
		}
	}
	TestTrue(TEXT("Tag 4 present when tail number configured"), bFoundTag4);

	// Now configure with empty tail number — tag should be absent
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);
	TArray<uint8> PacketNoTail = FKlvBuilder::BuildMisbST0601(T);
	TestTrue(TEXT("Packet without tail number is smaller"),
		PacketNoTail.Num() < Packet.Num());

	return true;
}

// -------------------------------------------------------------------------
// Tag 8 — Ground Speed presence/absence
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26GroundSpeedTagTest,
	"CamSim.Phase26.GroundSpeedTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26GroundSpeedTagTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);

	FCamSimTelemetry T;
	T.TimestampUs = 1000000000000000ULL;

	// No speed → no Tag 8
	T.GroundSpeedMps = 0.0f;
	TArray<uint8> PacketNoSpeed = FKlvBuilder::BuildMisbST0601(T);

	// With speed → Tag 8 present → larger packet
	T.GroundSpeedMps = 50.0f;
	TArray<uint8> PacketWithSpeed = FKlvBuilder::BuildMisbST0601(T);

	// Tag 8 is 3 bytes (tag + len + value)
	TestTrue(TEXT("Packet with ground speed is larger"),
		PacketWithSpeed.Num() > PacketNoSpeed.Num());

	return true;
}

// -------------------------------------------------------------------------
// Version bump: Tag 65 = 9
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26VersionTest,
	"CamSim.Phase26.VersionNumber",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26VersionTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);

	FCamSimTelemetry T;
	T.TimestampUs = 1000000000000000ULL;
	TArray<uint8> Packet = FKlvBuilder::BuildMisbST0601(T);

	// Find Tag 65 in the packet (tag=65, len=1, value)
	bool bFoundVersion9 = false;
	for (int32 i = 17; i < Packet.Num() - 4; ++i)
	{
		if (Packet[i] == 65 && Packet[i + 1] == 1 && Packet[i + 2] == 9)
		{
			bFoundVersion9 = true;
			break;
		}
	}
	TestTrue(TEXT("Tag 65 version = 9"), bFoundVersion9);

	return true;
}

// -------------------------------------------------------------------------
// BCC-16 checksum mode
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26Bcc16ModeTest,
	"CamSim.Phase26.Bcc16Mode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26Bcc16ModeTest::RunTest(const FString& Parameters)
{
	FCamSimTelemetry T;
	T.TimestampUs = 1000000000000000ULL;

	// Build with CRC-16
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);
	TArray<uint8> PacketCrc = FKlvBuilder::BuildMisbST0601(T);

	// Build with BCC-16
	FKlvBuilder::Configure(TEXT("bcc16"), TEXT(""), 0.0f, 0.0f);
	TArray<uint8> PacketBcc = FKlvBuilder::BuildMisbST0601(T);

	// Both should be the same size (same tags, just different checksum)
	TestEqual(TEXT("Same packet size"), PacketCrc.Num(), PacketBcc.Num());

	// But the last 2 bytes (checksum value) should differ
	const int32 N = PacketCrc.Num();
	const bool bChecksumDiffers =
		(PacketCrc[N - 2] != PacketBcc[N - 2]) ||
		(PacketCrc[N - 1] != PacketBcc[N - 1]);
	TestTrue(TEXT("CRC-16 and BCC-16 produce different checksums"), bChecksumDiffers);

	// Reset to CRC-16 for other tests
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);

	return true;
}

// -------------------------------------------------------------------------
// Tags 40/41 — Target Track Gate Width/Height
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26TargetGateTest,
	"CamSim.Phase26.TargetTrackGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26TargetGateTest::RunTest(const FString& Parameters)
{
	FCamSimTelemetry T;
	T.TimestampUs = 1000000000000000ULL;

	// No gate configured
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);
	TArray<uint8> PacketNoGate = FKlvBuilder::BuildMisbST0601(T);

	// Gate configured
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 32.0f, 24.0f);
	TArray<uint8> PacketWithGate = FKlvBuilder::BuildMisbST0601(T);

	// Tags 40+41 = 6 bytes total (tag+len+value each)
	TestTrue(TEXT("Packet with gate is 6 bytes larger"),
		PacketWithGate.Num() == PacketNoGate.Num() + 6);

	// Reset
	FKlvBuilder::Configure(TEXT("crc16"), TEXT(""), 0.0f, 0.0f);

	return true;
}
