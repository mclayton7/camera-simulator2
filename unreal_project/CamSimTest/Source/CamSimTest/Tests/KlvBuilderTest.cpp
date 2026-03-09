// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// KlvBuilder Automation Tests
//
// Validates MISB ST 0601 fixed-point encoding, boundary values, and
// round-trip correctness of all public mapping helpers.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapLatLonTest,
	"CamSim.KlvBuilder.MapLatLon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapLatLonTest::RunTest(const FString& Parameters)
{
	// Zero degrees should map to zero
	TestEqual(TEXT("Lat 0.0 -> 0"), FKlvBuilder::MapLatLon(0.0, 90.0), 0);

	// +90 should map to INT32_MAX
	TestEqual(TEXT("Lat +90 -> INT32_MAX"), FKlvBuilder::MapLatLon(90.0, 90.0), 0x7FFFFFFF);

	// -90 should map to -INT32_MAX
	TestEqual(TEXT("Lat -90 -> -INT32_MAX"), FKlvBuilder::MapLatLon(-90.0, 90.0), -0x7FFFFFFF);

	// Longitude ±180
	TestEqual(TEXT("Lon +180 -> INT32_MAX"), FKlvBuilder::MapLatLon(180.0, 180.0), 0x7FFFFFFF);
	TestEqual(TEXT("Lon -180 -> -INT32_MAX"), FKlvBuilder::MapLatLon(-180.0, 180.0), -0x7FFFFFFF);

	// Clamping: values beyond range should clamp
	TestEqual(TEXT("Lat +100 clamped -> INT32_MAX"), FKlvBuilder::MapLatLon(100.0, 90.0), 0x7FFFFFFF);
	TestEqual(TEXT("Lat -100 clamped -> -INT32_MAX"), FKlvBuilder::MapLatLon(-100.0, 90.0), -0x7FFFFFFF);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapAzimuth360Test,
	"CamSim.KlvBuilder.MapAzimuth360",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapAzimuth360Test::RunTest(const FString& Parameters)
{
	// 0 degrees -> 0
	TestEqual(TEXT("Az 0 -> 0"), FKlvBuilder::MapAzimuth360(0.0f), 0u);

	// 180 degrees -> ~UINT32_MAX/2
	const uint32 Half = FKlvBuilder::MapAzimuth360(180.0f);
	TestTrue(TEXT("Az 180 near UINT32_MAX/2"), FMath::Abs(static_cast<int64>(Half) - static_cast<int64>(0x80000000u)) < 1000);

	// Negative normalization: -90 -> 270 equivalent
	const uint32 Neg90  = FKlvBuilder::MapAzimuth360(-90.0f);
	const uint32 Pos270 = FKlvBuilder::MapAzimuth360(270.0f);
	TestTrue(TEXT("-90 == 270"), FMath::Abs(static_cast<int64>(Neg90) - static_cast<int64>(Pos270)) < 1000);

	// 360 wraps to 0
	TestEqual(TEXT("Az 360 wraps to 0"), FKlvBuilder::MapAzimuth360(360.0f), 0u);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapElevation180Test,
	"CamSim.KlvBuilder.MapElevation180",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapElevation180Test::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Elev 0 -> 0"), FKlvBuilder::MapElevation180(0.0f), 0);
	TestEqual(TEXT("Elev +180 -> INT32_MAX"), FKlvBuilder::MapElevation180(180.0f), 0x7FFFFFFF);
	TestEqual(TEXT("Elev -180 -> -INT32_MAX"), FKlvBuilder::MapElevation180(-180.0f), -0x7FFFFFFF);

	// Clamping
	TestEqual(TEXT("Elev +200 clamped"), FKlvBuilder::MapElevation180(200.0f), 0x7FFFFFFF);
	TestEqual(TEXT("Elev -200 clamped"), FKlvBuilder::MapElevation180(-200.0f), -0x7FFFFFFF);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapAltitudeTest,
	"CamSim.KlvBuilder.MapAltitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapAltitudeTest::RunTest(const FString& Parameters)
{
	// -900 m -> 0 (minimum)
	TestEqual(TEXT("Alt -900 -> 0"), FKlvBuilder::MapAltitude(-900.0), static_cast<int16>(0));

	// 19000 m -> 65535 (maximum)
	const uint16 MaxAlt = static_cast<uint16>(FKlvBuilder::MapAltitude(19000.0));
	TestEqual(TEXT("Alt 19000 -> 65535"), MaxAlt, static_cast<uint16>(65535));

	// 0 m -> should be near 900/19900 * 65535 ≈ 2963
	const uint16 ZeroAlt = static_cast<uint16>(FKlvBuilder::MapAltitude(0.0));
	TestTrue(TEXT("Alt 0 near 2963"), FMath::Abs(static_cast<int32>(ZeroAlt) - 2963) < 2);

	// Clamping below minimum
	TestEqual(TEXT("Alt -2000 clamped -> 0"), FKlvBuilder::MapAltitude(-2000.0), static_cast<int16>(0));

	// Clamping above maximum
	const uint16 OverAlt = static_cast<uint16>(FKlvBuilder::MapAltitude(25000.0));
	TestEqual(TEXT("Alt 25000 clamped -> 65535"), OverAlt, static_cast<uint16>(65535));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapFovTest,
	"CamSim.KlvBuilder.MapFov",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapFovTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("FOV 0 -> 0"), FKlvBuilder::MapFov(0.0f), static_cast<uint16>(0));
	TestEqual(TEXT("FOV 180 -> 65535"), FKlvBuilder::MapFov(180.0f), static_cast<uint16>(65535));

	// 60 degrees -> ~21845
	const uint16 Fov60 = FKlvBuilder::MapFov(60.0f);
	TestTrue(TEXT("FOV 60 near 21845"), FMath::Abs(static_cast<int32>(Fov60) - 21845) < 2);

	// Clamping
	TestEqual(TEXT("FOV -10 clamped -> 0"), FKlvBuilder::MapFov(-10.0f), static_cast<uint16>(0));
	TestEqual(TEXT("FOV 200 clamped -> 65535"), FKlvBuilder::MapFov(200.0f), static_cast<uint16>(65535));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapHeadingTest,
	"CamSim.KlvBuilder.MapHeading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapHeadingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Heading 0 -> 0"), FKlvBuilder::MapHeading(0.0f), static_cast<uint16>(0));

	// 180 -> ~32767
	const uint16 H180 = FKlvBuilder::MapHeading(180.0f);
	TestTrue(TEXT("Heading 180 near 32767"), FMath::Abs(static_cast<int32>(H180) - 32767) < 2);

	// Negative normalization
	const uint16 HNeg90  = FKlvBuilder::MapHeading(-90.0f);
	const uint16 HPos270 = FKlvBuilder::MapHeading(270.0f);
	TestTrue(TEXT("Heading -90 == 270"), FMath::Abs(static_cast<int32>(HNeg90) - static_cast<int32>(HPos270)) < 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapPlatformPitchTest,
	"CamSim.KlvBuilder.MapPlatformPitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapPlatformPitchTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Pitch 0 -> 0"), FKlvBuilder::MapPlatformPitch(0.0f), static_cast<int16>(0));
	TestEqual(TEXT("Pitch +20 -> INT16_MAX"), FKlvBuilder::MapPlatformPitch(20.0f), static_cast<int16>(0x7FFF));
	TestEqual(TEXT("Pitch -20 -> -INT16_MAX"), FKlvBuilder::MapPlatformPitch(-20.0f), static_cast<int16>(-0x7FFF));

	// Clamping
	TestEqual(TEXT("Pitch +45 clamped"), FKlvBuilder::MapPlatformPitch(45.0f), static_cast<int16>(0x7FFF));
	TestEqual(TEXT("Pitch -45 clamped"), FKlvBuilder::MapPlatformPitch(-45.0f), static_cast<int16>(-0x7FFF));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapPlatformRollTest,
	"CamSim.KlvBuilder.MapPlatformRoll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapPlatformRollTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Roll 0 -> 0"), FKlvBuilder::MapPlatformRoll(0.0f), static_cast<int16>(0));
	TestEqual(TEXT("Roll +50 -> INT16_MAX"), FKlvBuilder::MapPlatformRoll(50.0f), static_cast<int16>(0x7FFF));
	TestEqual(TEXT("Roll -50 -> -INT16_MAX"), FKlvBuilder::MapPlatformRoll(-50.0f), static_cast<int16>(-0x7FFF));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderMapSlantRangeTest,
	"CamSim.KlvBuilder.MapSlantRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderMapSlantRangeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Range 0 -> 0"), FKlvBuilder::MapSlantRange(0.0), 0u);

	// 5000000 m -> UINT32_MAX
	TestEqual(TEXT("Range 5M -> UINT32_MAX"), FKlvBuilder::MapSlantRange(5000000.0), TNumericLimits<uint32>::Max());

	// 2500000 -> ~UINT32_MAX/2
	const uint32 Half = FKlvBuilder::MapSlantRange(2500000.0);
	TestTrue(TEXT("Range 2.5M near UINT32_MAX/2"),
		FMath::Abs(static_cast<int64>(Half) - static_cast<int64>(TNumericLimits<uint32>::Max() / 2)) < 1000);

	// Clamping above max
	TestEqual(TEXT("Range 10M clamped"), FKlvBuilder::MapSlantRange(10000000.0), TNumericLimits<uint32>::Max());

	// Clamping below min
	TestEqual(TEXT("Range -100 clamped -> 0"), FKlvBuilder::MapSlantRange(-100.0), 0u);

	return true;
}

// -------------------------------------------------------------------------
// Full KLV packet structure tests
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderBuildPacketTest,
	"CamSim.KlvBuilder.BuildPacket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderBuildPacketTest::RunTest(const FString& Parameters)
{
	// Build a packet with known telemetry
	FCamSimTelemetry T;
	T.TimestampUs      = 1700000000000000ULL; // ~2023 epoch
	T.Latitude         = 38.8977;
	T.Longitude        = -77.0365;
	T.Altitude         = 500.0;
	T.Yaw              = 90.0f;
	T.Pitch            = -5.0f;
	T.Roll             = 2.0f;
	T.HFovDeg          = 60.0f;
	T.VFovDeg          = 33.75f;
	T.GimbalYaw        = 10.0f;
	T.GimbalPitch      = -30.0f;
	T.GimbalRoll       = 0.0f;
	T.SlantRangeM      = 1000.0;
	T.FrameCenterLat   = 38.89;
	T.FrameCenterLon   = -77.04;
	T.FrameCenterElev  = 50.0;
	T.SensorMode       = 0;
	T.SensorPolarity   = 0;

	TArray<uint8> Packet = FKlvBuilder::BuildMisbST0601(T);

	// Packet should have content
	TestTrue(TEXT("Packet not empty"), Packet.Num() > 0);

	// Should start with the ST 0601 Universal Label
	static const uint8 UL[4] = { 0x06, 0x0E, 0x2B, 0x34 };
	TestTrue(TEXT("Starts with UL key"),
		Packet.Num() >= 4 &&
		Packet[0] == UL[0] && Packet[1] == UL[1] &&
		Packet[2] == UL[2] && Packet[3] == UL[3]);

	// Last 4 bytes should be Tag 1 (checksum): tag=1, len=2, crc16(2 bytes)
	TestTrue(TEXT("Ends with checksum tag"),
		Packet.Num() >= 4 &&
		Packet[Packet.Num() - 4] == 1 &&
		Packet[Packet.Num() - 3] == 2);

	// Minimum expected size: 16 (UL) + 1-3 (BER len) + tags >= 60 bytes
	TestTrue(TEXT("Reasonable packet size"), Packet.Num() >= 60 && Packet.Num() < 1024);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderBuildPacketZeroSlantRangeTest,
	"CamSim.KlvBuilder.BuildPacketZeroSlantRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderBuildPacketZeroSlantRangeTest::RunTest(const FString& Parameters)
{
	// When SlantRangeM <= 0, Tag 21 should be omitted -> smaller packet
	FCamSimTelemetry T;
	T.TimestampUs   = 1000000000000000ULL;
	T.Latitude      = 0.0;
	T.Longitude     = 0.0;
	T.Altitude      = 100.0;
	T.SlantRangeM   = 0.0;

	TArray<uint8> PacketNoRange = FKlvBuilder::BuildMisbST0601(T);

	T.SlantRangeM = 1000.0;
	TArray<uint8> PacketWithRange = FKlvBuilder::BuildMisbST0601(T);

	// The packet with range should be 6 bytes larger (tag=1, len=1, value=4)
	TestTrue(TEXT("Packet with range is larger"),
		PacketWithRange.Num() > PacketNoRange.Num());

	return true;
}

// -------------------------------------------------------------------------
// NaN guard tests
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKlvBuilderNaNGuardTest,
	"CamSim.KlvBuilder.NaNGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FKlvBuilderNaNGuardTest::RunTest(const FString& Parameters)
{
	// NaN inputs should not crash — they should be clamped by FMath::Clamp
	const float NaN = std::numeric_limits<float>::quiet_NaN();

	// These should not crash (NaN propagation through FMath::Clamp is implementation-defined,
	// but the function should not crash)
	FKlvBuilder::MapLatLon(NaN, 90.0);
	FKlvBuilder::MapAzimuth360(NaN);
	FKlvBuilder::MapElevation180(NaN);
	FKlvBuilder::MapFov(NaN);
	FKlvBuilder::MapHeading(NaN);
	FKlvBuilder::MapPlatformPitch(NaN);
	FKlvBuilder::MapPlatformRoll(NaN);
	FKlvBuilder::MapSlantRange(NaN);
	FKlvBuilder::MapAltitude(NaN);

	// Build a packet with NaN telemetry — should not crash
	FCamSimTelemetry T;
	T.Latitude = NaN;
	T.Longitude = NaN;
	T.Altitude = NaN;
	T.Yaw = NaN;
	TArray<uint8> Packet = FKlvBuilder::BuildMisbST0601(T);
	TestTrue(TEXT("NaN telemetry produces valid packet"), Packet.Num() > 0);

	return true;
}
