// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// Phase 26: Standards Compliance Tests
//
// The reference decoder is misb.js (github.com/vidterra/misb.js). These tests
// pin the wire format it expects; scripts/klv_conformance runs misb.js itself
// against packets exported by CamSim.KlvConformance.ExportPackets.
// -------------------------------------------------------------------------

namespace
{
	/** Walk a ST 0601 packet's top-level TLVs. Returns false on malformed input. */
	bool ParseTopLevelTags(const TArray<uint8>& Packet, TMap<uint8, TArray<uint8>>& OutTags)
	{
		if (Packet.Num() < 17) return false;
		int32 Pos = 16;
		int32 ValueLen = Packet[Pos++];
		if (ValueLen & 0x80)
		{
			const int32 NumBytes = ValueLen & 0x7F;
			ValueLen = 0;
			for (int32 i = 0; i < NumBytes; ++i)
			{
				ValueLen = (ValueLen << 8) | Packet[Pos++];
			}
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
				for (int32 i = 0; i < NumBytes; ++i)
				{
					Len = (Len << 8) | Packet[Pos++];
				}
			}
			if (Pos + Len > End) return false;
			OutTags.Add(Tag, TArray<uint8>(Packet.GetData() + Pos, Len));
			Pos += Len;
		}
		return true;
	}

	FCamSimTelemetry MakeTelemetry()
	{
		FCamSimTelemetry T;
		T.TimestampUs = 1700000000000000ULL;
		T.Latitude    = 38.8977;
		T.Longitude   = -77.0365;
		T.Altitude    = 1500.0;
		T.SlantRangeM = 2000.0;
		return T;
	}
}

// -------------------------------------------------------------------------
// MapGroundSpeed
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26MapGroundSpeedTest,
	"CamSim.Phase26.MapGroundSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26MapGroundSpeedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Speed 0 -> 0"), FKlvBuilder::MapGroundSpeed(0.0f), static_cast<uint8>(0));
	TestEqual(TEXT("Speed 50 -> 50"), FKlvBuilder::MapGroundSpeed(50.0f), static_cast<uint8>(50));
	TestEqual(TEXT("Speed 300 clamped -> 255"), FKlvBuilder::MapGroundSpeed(300.0f), static_cast<uint8>(255));
	TestEqual(TEXT("Speed -10 clamped -> 0"), FKlvBuilder::MapGroundSpeed(-10.0f), static_cast<uint8>(0));
	TestEqual(TEXT("Speed 99.6 rounds to 100"), FKlvBuilder::MapGroundSpeed(99.6f), static_cast<uint8>(100));
	TestEqual(TEXT("Speed NaN -> 0"), FKlvBuilder::MapGroundSpeed(std::numeric_limits<float>::quiet_NaN()),
		static_cast<uint8>(0));
	return true;
}

// -------------------------------------------------------------------------
// ComputeChecksum — running 16-bit sum
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26ComputeChecksumTest,
	"CamSim.Phase26.ComputeChecksum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26ComputeChecksumTest::RunTest(const FString& Parameters)
{
	// Even-indexed bytes go to the high byte, odd-indexed to the low byte:
	// 0x0100 + 0x0002 + 0x0300 + 0x0004 = 0x0406
	const uint8 Data[] = { 0x01, 0x02, 0x03, 0x04 };
	TestEqual(TEXT("Sum {01,02,03,04}"), FKlvBuilder::ComputeChecksum(Data, 4), static_cast<uint16>(0x0406));
	TestEqual(TEXT("Sum empty"), FKlvBuilder::ComputeChecksum(nullptr, 0), static_cast<uint16>(0));

	const uint8 Single[] = { 0xAB };
	TestEqual(TEXT("Sum single byte"), FKlvBuilder::ComputeChecksum(Single, 1), static_cast<uint16>(0xAB00));

	// Wraps modulo 2^16
	const uint8 Wrap[] = { 0xFF, 0x00, 0x02, 0x00 };
	TestEqual(TEXT("Sum wraps"), FKlvBuilder::ComputeChecksum(Wrap, 4), static_cast<uint16>(0x0100));
	return true;
}

// -------------------------------------------------------------------------
// Tag 1 covers the whole packet, including its own "01 02" bytes
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26PacketChecksumTest,
	"CamSim.Phase26.PacketChecksum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26PacketChecksumTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT("N12345"), 32.0f, 24.0f);
	const TArray<uint8> Packet = FKlvBuilder::BuildMisbST0601(MakeTelemetry());
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);

	const int32 N = Packet.Num();
	if (!TestTrue(TEXT("Packet ends with checksum item"), N > 20 && Packet[N - 4] == 1 && Packet[N - 3] == 2))
	{
		return false;
	}

	// Independent re-implementation of misb.js klv.calculateChecksum: sum every
	// byte except the final two (the checksum value itself).
	uint32 Sum = 0;
	for (int32 i = 0; i < N - 2; ++i)
	{
		Sum += static_cast<uint32>(Packet[i]) << (8 * ((i + 1) % 2));
	}
	const uint16 Expected = static_cast<uint16>(Sum % 65536);
	const uint16 Actual   = static_cast<uint16>((Packet[N - 2] << 8) | Packet[N - 1]);
	TestEqual(TEXT("Checksum matches misb.js algorithm and range"), Actual, Expected);
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
	FKlvBuilder::Configure(TEXT("N12345"), 0.0f, 0.0f);
	TMap<uint8, TArray<uint8>> Tags;
	TestTrue(TEXT("Packet parses"), ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags));
	const TArray<uint8>* Tail = Tags.Find(4);
	TestTrue(TEXT("Tag 4 present when tail number configured"), Tail != nullptr);
	if (Tail)
	{
		const FString Decoded = FString::ConstructFromPtrSize(
			reinterpret_cast<const ANSICHAR*>(Tail->GetData()), Tail->Num());
		TestEqual(TEXT("Tag 4 value"), Decoded, FString(TEXT("N12345")));
	}

	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	Tags.Reset();
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags);
	TestFalse(TEXT("Tag 4 absent without tail number"), Tags.Contains(4));
	return true;
}

// -------------------------------------------------------------------------
// Tag 56 — Platform Ground Speed (Tag 8 is True Airspeed and must not be used)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26GroundSpeedTagTest,
	"CamSim.Phase26.GroundSpeedTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26GroundSpeedTagTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FCamSimTelemetry T = MakeTelemetry();

	T.GroundSpeedMps = 0.0f;
	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(T), Tags);
	TestFalse(TEXT("No Tag 56 at zero speed"), Tags.Contains(56));

	T.GroundSpeedMps = 50.0f;
	Tags.Reset();
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(T), Tags);
	TestFalse(TEXT("Tag 8 (True Airspeed) not emitted"), Tags.Contains(8));
	const TArray<uint8>* Speed = Tags.Find(56);
	TestTrue(TEXT("Tag 56 present"), Speed != nullptr && Speed->Num() == 1);
	if (Speed && Speed->Num() == 1)
	{
		TestEqual(TEXT("Tag 56 value"), (*Speed)[0], static_cast<uint8>(50));
	}
	return true;
}

// -------------------------------------------------------------------------
// Tag 65 = 9
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26VersionTest,
	"CamSim.Phase26.VersionNumber",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26VersionTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags);
	const TArray<uint8>* Version = Tags.Find(65);
	TestTrue(TEXT("Tag 65 = 9"), Version && Version->Num() == 1 && (*Version)[0] == 9);
	return true;
}

// -------------------------------------------------------------------------
// Tags 43/44 — Target Track Gate Width/Height (value = pixels / 2)
// Tags 40/41 are Target Location Lat/Lon and must not carry gate values.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26TargetGateTest,
	"CamSim.Phase26.TargetTrackGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26TargetGateTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags);
	TestFalse(TEXT("No Tag 43 without gate"), Tags.Contains(43));
	TestFalse(TEXT("No Tag 44 without gate"), Tags.Contains(44));

	FKlvBuilder::Configure(TEXT(""), 32.0f, 24.0f);
	Tags.Reset();
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags);
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);

	TestFalse(TEXT("Tag 40 not used for gate"), Tags.Contains(40));
	TestFalse(TEXT("Tag 41 not used for gate"), Tags.Contains(41));
	const TArray<uint8>* W = Tags.Find(43);
	const TArray<uint8>* H = Tags.Find(44);
	TestTrue(TEXT("Tag 43 = 32 px / 2"), W && W->Num() == 1 && (*W)[0] == 16);
	TestTrue(TEXT("Tag 44 = 24 px / 2"), H && H->Num() == 1 && (*H)[0] == 12);

	TestEqual(TEXT("Gate 600 px clamps to 255"), FKlvBuilder::MapTrackGate(600.0f), static_cast<uint8>(255));
	return true;
}

// -------------------------------------------------------------------------
// Tag 47 — Generic Flag Data bits (bit 1 = LSB)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26GenericFlagTest,
	"CamSim.Phase26.GenericFlagData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26GenericFlagTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FCamSimTelemetry T = MakeTelemetry();

	T.SensorPolarity = 1;
	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(T), Tags);
	const TArray<uint8>* Flags = Tags.Find(47);
	TestTrue(TEXT("Black-hot sets only bit 3 (IR polarity)"), Flags && Flags->Num() == 1 && (*Flags)[0] == 0x04);

	T.SensorPolarity = 0;
	Tags.Reset();
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(T), Tags);
	Flags = Tags.Find(47);
	// Slant range is calculated (ray-cast), so bit 5 stays 0; nothing may set
	// bit 4 (icing) or bit 6 (image invalid).
	TestTrue(TEXT("White-hot with calculated range sets no bits"), Flags && Flags->Num() == 1 && (*Flags)[0] == 0x00);
	return true;
}

// -------------------------------------------------------------------------
// Tag 48 — ST 0102 security set: tag 12 is the coding method, tag 13 the
// UTF-16BE country codes
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26SecurityCountryCodesTest,
	"CamSim.Phase26.SecurityCountryCodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26SecurityCountryCodesTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FKlvBuilder::SetSecurityMetadata(TEXT("UNCLASSIFIED"), TEXT("//US"), TEXT("US"));

	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(MakeTelemetry()), Tags);
	const TArray<uint8>* Security = Tags.Find(48);
	if (!TestTrue(TEXT("Tag 48 present"), Security != nullptr))
	{
		return false;
	}

	TMap<uint8, TArray<uint8>> Inner;
	for (int32 Pos = 0; Pos + 2 <= Security->Num();)
	{
		const uint8 Tag = (*Security)[Pos];
		const uint8 Len = (*Security)[Pos + 1];
		Inner.Add(Tag, TArray<uint8>(Security->GetData() + Pos + 2, Len));
		Pos += 2 + Len;
	}

	const TArray<uint8>* Method = Inner.Find(12);
	TestTrue(TEXT("ST 0102 tag 12 = 1 (ISO-3166 two letter)"), Method && Method->Num() == 1 && (*Method)[0] == 1);

	const TArray<uint8>* Codes = Inner.Find(13);
	const TArray<uint8> ExpectedCodes = { 0x00, 'U', 0x00, 'S' };
	TestTrue(TEXT("ST 0102 tag 13 = UTF-16BE \"US\""), Codes && *Codes == ExpectedCodes);
	return true;
}

// -------------------------------------------------------------------------
// Tags 21, 23-25 — omitted together when the boresight misses the ground
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase26FrameCenterOmittedTest,
	"CamSim.Phase26.FrameCenterOmittedAboveHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase26FrameCenterOmittedTest::RunTest(const FString& Parameters)
{
	FKlvBuilder::Configure(TEXT(""), 0.0f, 0.0f);
	FCamSimTelemetry T = MakeTelemetry();
	T.SlantRangeM    = 0.0;
	T.FrameCenterLat = 12.0;  // stale value from an earlier frame
	T.FrameCenterLon = 34.0;

	TMap<uint8, TArray<uint8>> Tags;
	ParseTopLevelTags(FKlvBuilder::BuildMisbST0601(T), Tags);
	for (const uint8 Tag : { 21, 23, 24, 25 })
	{
		TestFalse(FString::Printf(TEXT("Tag %d omitted without ground intersection"), Tag), Tags.Contains(Tag));
	}
	return true;
}
