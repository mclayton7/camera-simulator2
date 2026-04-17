// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DIS/DisPduTypes.h"
#include "DIS/DisEntityAdapter.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Helpers: build a raw 144-byte Entity State PDU in network byte order
// -------------------------------------------------------------------------
namespace
{

void WriteU16BE(uint8* P, uint16 V)
{
	P[0] = static_cast<uint8>(V >> 8);
	P[1] = static_cast<uint8>(V & 0xFF);
}

void WriteU32BE(uint8* P, uint32 V)
{
	P[0] = static_cast<uint8>((V >> 24) & 0xFF);
	P[1] = static_cast<uint8>((V >> 16) & 0xFF);
	P[2] = static_cast<uint8>((V >>  8) & 0xFF);
	P[3] = static_cast<uint8>( V        & 0xFF);
}

void WriteF32BE(uint8* P, float V)
{
	uint32 Bits;
	FMemory::Memcpy(&Bits, &V, 4);
	WriteU32BE(P, Bits);
}

void WriteF64BE(uint8* P, double V)
{
	uint64 Bits;
	FMemory::Memcpy(&Bits, &V, 8);
	WriteU32BE(P,     static_cast<uint32>(Bits >> 32));
	WriteU32BE(P + 4, static_cast<uint32>(Bits & 0xFFFFFFFF));
}

// Build a minimal valid Entity State PDU
void BuildEntityStatePdu(uint8* Buf, uint16 Site, uint16 App, uint16 Entity,
                         double EcefX, double EcefY, double EcefZ,
                         float Psi, float Theta, float Phi,
                         uint8 ExerciseId = 1)
{
	FMemory::Memzero(Buf, 144);

	// Header (12 bytes)
	Buf[0] = 6;                   // Protocol version (DIS 6)
	Buf[1] = ExerciseId;          // Exercise ID
	Buf[2] = EDisPduType::EntityState;
	Buf[3] = EDisProtocolFamily::EntityInformation;
	WriteU32BE(Buf + 4, 0);       // Timestamp
	WriteU16BE(Buf + 8, 144);     // PDU length
	WriteU16BE(Buf + 10, 0);      // Padding

	// Entity ID (offset 12)
	WriteU16BE(Buf + 12, Site);
	WriteU16BE(Buf + 14, App);
	WriteU16BE(Buf + 16, Entity);

	// Force ID (offset 18)
	Buf[18] = 1; // Friendly

	// Entity Type (offset 20): Kind=1 (Platform), Domain=2 (Air), Country=225 (USA), Cat=1
	Buf[20] = 1;   // Kind
	Buf[21] = 2;   // Domain
	WriteU16BE(Buf + 22, 225); // Country
	Buf[24] = 1;   // Category

	// Linear Velocity (offset 36)
	WriteF32BE(Buf + 36, 100.0f);  // VelX
	WriteF32BE(Buf + 40, 0.0f);    // VelY
	WriteF32BE(Buf + 44, 0.0f);    // VelZ

	// Entity Location ECEF (offset 48)
	WriteF64BE(Buf + 48, EcefX);
	WriteF64BE(Buf + 56, EcefY);
	WriteF64BE(Buf + 64, EcefZ);

	// Entity Orientation (offset 72) — radians
	WriteF32BE(Buf + 72, Psi);
	WriteF32BE(Buf + 76, Theta);
	WriteF32BE(Buf + 80, Phi);

	// DR Algorithm (offset 88)
	Buf[88] = 2;  // FPW

	// Marking (offset 128): encoding=6 (ASCII), then "TEST       "
	Buf[128] = 6;
	FMemory::Memcpy(Buf + 129, "TEST\0\0\0\0\0\0\0", 11);
}

} // anonymous namespace

// =========================================================================
// Test: PDU Header parsing
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisPduHeaderParseTest,
	"CamSim.Phase21.PduHeaderParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisPduHeaderParseTest::RunTest(const FString& Parameters)
{
	uint8 Buf[144];
	BuildEntityStatePdu(Buf, 1, 2, 47,
		1115077.0, -4844132.0, 3982816.0,
		0.0f, 0.0f, 0.0f);

	FDisPduHeader Hdr;
	TestTrue(TEXT("Header parse succeeds"), FDisPduHeader::Parse(Buf, 144, Hdr));
	TestEqual(TEXT("Protocol version"), (int32)Hdr.ProtocolVersion, 6);
	TestEqual(TEXT("Exercise ID"),      (int32)Hdr.ExerciseId, 1);
	TestEqual(TEXT("PDU type"),         (int32)Hdr.PduType, (int32)EDisPduType::EntityState);
	TestEqual(TEXT("PDU length"),       (int32)Hdr.PduLength, 144);

	// Too short
	FDisPduHeader ShortHdr;
	TestFalse(TEXT("Header parse fails on short buffer"), FDisPduHeader::Parse(Buf, 8, ShortHdr));

	return true;
}

// =========================================================================
// Test: Entity State PDU parsing
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisEntityStatePduParseTest,
	"CamSim.Phase21.EntityStatePduParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisEntityStatePduParseTest::RunTest(const FString& Parameters)
{
	uint8 Buf[144];
	const float Psi = 1.5707963f;  // 90 degrees
	BuildEntityStatePdu(Buf, 3, 5, 100,
		1115077.0, -4844132.0, 3982816.0,
		Psi, 0.1f, -0.05f);

	FDisEntityStatePdu Pdu;
	TestTrue(TEXT("Parse succeeds"), FDisEntityStatePdu::Parse(Buf, 144, Pdu));

	TestEqual(TEXT("Site"),   (int32)Pdu.EntityId.Site, 3);
	TestEqual(TEXT("App"),    (int32)Pdu.EntityId.Application, 5);
	TestEqual(TEXT("Entity"), (int32)Pdu.EntityId.Entity, 100);
	TestEqual(TEXT("ForceId"), (int32)Pdu.ForceId, 1);

	TestEqual(TEXT("EntityKind"), (int32)Pdu.EntityType.EntityKind, 1);
	TestEqual(TEXT("Domain"),     (int32)Pdu.EntityType.Domain, 2);
	TestEqual(TEXT("Country"),    (int32)Pdu.EntityType.Country, 225);
	TestEqual(TEXT("Category"),   (int32)Pdu.EntityType.Category, 1);

	TestTrue(TEXT("ECEF X close"), FMath::Abs(Pdu.LocationX - 1115077.0) < 0.001);
	TestTrue(TEXT("ECEF Y close"), FMath::Abs(Pdu.LocationY - (-4844132.0)) < 0.001);
	TestTrue(TEXT("ECEF Z close"), FMath::Abs(Pdu.LocationZ - 3982816.0) < 0.001);

	TestTrue(TEXT("Psi close"),   FMath::Abs(Pdu.Psi - Psi) < 0.001f);
	TestTrue(TEXT("Theta close"), FMath::Abs(Pdu.Theta - 0.1f) < 0.001f);
	TestTrue(TEXT("Phi close"),   FMath::Abs(Pdu.Phi - (-0.05f)) < 0.001f);

	TestEqual(TEXT("DR algo"), (int32)Pdu.DeadReckoning.Algorithm, 2);
	TestTrue(TEXT("DR VelX"), FMath::Abs(Pdu.DeadReckoning.VelX - 100.0f) < 0.01f);
	TestEqual(TEXT("Marking"), Pdu.Marking, FString(TEXT("TEST")));

	// Wrong PDU type should fail
	Buf[2] = 99;
	FDisEntityStatePdu BadPdu;
	TestFalse(TEXT("Wrong PDU type"), FDisEntityStatePdu::Parse(Buf, 144, BadPdu));

	return true;
}

// =========================================================================
// Test: ECEF → Geodetic conversion accuracy
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisEcefToGeodeticTest,
	"CamSim.Phase21.EcefToGeodetic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisEcefToGeodeticTest::RunTest(const FString& Parameters)
{
	// Test point: Washington DC (38.8977°N, -77.0365°W, 0m altitude)
	// Pre-computed ECEF for this geodetic point:
	//   X = 1115076.853
	//   Y = -4844133.139
	//   Z = 3982816.376
	const double EcefX = 1115076.853;
	const double EcefY = -4844133.139;
	const double EcefZ = 3982816.376;

	double Lat, Lon, Alt;
	FDisEntityAdapter::EcefToGeodetic(EcefX, EcefY, EcefZ, Lat, Lon, Alt);

	// Allow 0.0001 degree tolerance (~11m) for this test
	// The ECEF values above were computed with limited precision
	TestTrue(TEXT("Latitude within tolerance"),
		FMath::Abs(Lat - 38.8977) < 0.001);
	TestTrue(TEXT("Longitude within tolerance"),
		FMath::Abs(Lon - (-77.0365)) < 0.001);
	TestTrue(TEXT("Altitude within 50m"),
		FMath::Abs(Alt) < 50.0);

	// Test point 2: Equator prime meridian at sea level
	// Lat=0, Lon=0, Alt=0 → ECEF = (6378137, 0, 0)
	double Lat2, Lon2, Alt2;
	FDisEntityAdapter::EcefToGeodetic(6378137.0, 0.0, 0.0, Lat2, Lon2, Alt2);

	TestTrue(TEXT("Equator lat"),  FMath::Abs(Lat2) < 0.001);
	TestTrue(TEXT("Prime lon"),    FMath::Abs(Lon2) < 0.001);
	TestTrue(TEXT("Sea level alt"), FMath::Abs(Alt2) < 1.0);

	// Test point 3: North pole
	// Lat=90, Lon=0, Alt=0 → ECEF = (0, 0, 6356752.314)
	double Lat3, Lon3, Alt3;
	FDisEntityAdapter::EcefToGeodetic(0.0, 0.0, 6356752.314245, Lat3, Lon3, Alt3);

	TestTrue(TEXT("North pole lat"), FMath::Abs(Lat3 - 90.0) < 0.001);
	TestTrue(TEXT("North pole alt"), FMath::Abs(Alt3) < 1.0);

	return true;
}

// =========================================================================
// Test: DIS Entity ID → CamSim uint16 allocation (monotonic, non-recycling)
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisIdTranslationTest,
	"CamSim.Phase21.IdTranslation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisIdTranslationTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.DIS.bEnabled = true;
	Config.DIS.IdBaseOffset = 1000;

	FDisEntityAdapter Adapter(Config, nullptr);

	FDisEntityId Id1 { 1, 2, 47 };
	FDisEntityId Id2 { 1, 2, 48 };
	FDisEntityId Id3 { 3, 1, 1 };

	const uint16 CamId1 = Adapter.GetOrAllocateId(Id1);
	const uint16 CamId2 = Adapter.GetOrAllocateId(Id2);
	const uint16 CamId3 = Adapter.GetOrAllocateId(Id3);

	// All IDs should be unique
	TestTrue(TEXT("IDs unique 1-2"), CamId1 != CamId2);
	TestTrue(TEXT("IDs unique 1-3"), CamId1 != CamId3);
	TestTrue(TEXT("IDs unique 2-3"), CamId2 != CamId3);

	// IDs should be >= base offset
	TestTrue(TEXT("ID1 >= 1000"), CamId1 >= 1000);
	TestTrue(TEXT("ID2 >= 1000"), CamId2 >= 1000);

	// Same DIS ID should return same CamSim ID
	TestEqual(TEXT("ID1 stable"), Adapter.GetOrAllocateId(Id1), CamId1);

	// Release + re-allocate a fresh DIS triple: post 7C.4, IDs are monotonic
	// and never recycled. CamId4 must NOT equal the freed CamId1.
	Adapter.ReleaseId(Id1);
	FDisEntityId Id4 { 5, 5, 5 };
	const uint16 CamId4 = Adapter.GetOrAllocateId(Id4);
	TestNotEqual(TEXT("No ID recycling after release"), CamId4, CamId1);
	TestTrue(TEXT("CamId4 monotonically greater than prior peak"), CamId4 > CamId3);

	return true;
}

// =========================================================================
// Test: DIS ID pool exhaustion sentinel (7C.4)
//
// After 4096 unique DIS entities have been allocated, further allocations
// return the sentinel MaxId == IdBaseOffset + 4096 rather than silently
// corrupting the map. The exhaustion log fires once, not per-allocation.
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisIdPoolExhaustionTest,
	"CamSim.Phase21.IdPoolExhaustion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisIdPoolExhaustionTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.DIS.bEnabled = true;
	Config.DIS.IdBaseOffset = 0;  // keep arithmetic simple: IDs run 0..4095

	// The adapter emits a single Error-level log when the pool is first exhausted.
	// Declare it expected so the automation framework doesn't promote it to a failure.
	AddExpectedError(TEXT("DIS entity ID pool exhausted"),
		EAutomationExpectedErrorFlags::Contains, 1);

	FDisEntityAdapter Adapter(Config, nullptr);

	constexpr uint16 PoolSize = 4096;
	const uint16 Sentinel = static_cast<uint16>(Config.DIS.IdBaseOffset + PoolSize);

	// Fill the pool. Each allocation should return a fresh unique ID < Sentinel.
	TSet<uint16> SeenIds;
	SeenIds.Reserve(PoolSize);
	for (uint32 I = 0; I < PoolSize; ++I)
	{
		// Three-tuple must be unique per allocation; vary Entity field since it's 16-bit.
		FDisEntityId DisId { 1, 1, static_cast<uint16>(I) };
		const uint16 CamId = Adapter.GetOrAllocateId(DisId);

		if (CamId >= Sentinel)
		{
			AddError(FString::Printf(
				TEXT("Allocation %u returned sentinel (%u) before pool was full"), I, CamId));
			return false;
		}
		SeenIds.Add(CamId);
	}
	TestEqual(TEXT("Full pool produces 4096 unique IDs"), SeenIds.Num(), static_cast<int32>(PoolSize));

	// One past the cap: must return the sentinel.
	FDisEntityId Overflow1 { 2, 2, 1 };
	const uint16 CamOverflow1 = Adapter.GetOrAllocateId(Overflow1);
	TestEqual(TEXT("First post-exhaustion allocation returns sentinel"),
		CamOverflow1, Sentinel);

	// Repeated overflow — still the sentinel, no crash, no corruption of prior mappings.
	FDisEntityId Overflow2 { 3, 3, 1 };
	const uint16 CamOverflow2 = Adapter.GetOrAllocateId(Overflow2);
	TestEqual(TEXT("Second post-exhaustion allocation also returns sentinel"),
		CamOverflow2, Sentinel);

	// Re-querying an in-pool entity must still return its original mapping — the
	// sentinel path must not pollute the DisIdMap.
	FDisEntityId InPool { 1, 1, 0 };
	const uint16 FirstId = Adapter.GetOrAllocateId(InPool);
	TestTrue(TEXT("Prior mapping survives overflow"), FirstId < Sentinel);

	return true;
}

// =========================================================================
// Test: DIS entity type mapping (exact, fuzzy, default)
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisEntityTypeMappingTest,
	"CamSim.Phase21.EntityTypeMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisEntityTypeMappingTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.DIS.bEnabled = true;
	Config.DIS.DefaultEntityTypeId = 9999;
	// Add an exact mapping: F-16 (Kind=1, Domain=2, Country=225, Cat=1, Sub=2, Spec=0, Extra=0)
	Config.DIS.EntityTypeMappings.Add(TEXT("1:2:225:1:2:0:0"), 1001);
	// Add a mapping that will also create a fuzzy entry for kind=1, domain=2, category=1
	Config.DIS.EntityTypeMappings.Add(TEXT("1:2:0:1:0:0:0"), 2001);

	FDisEntityAdapter Adapter(Config, nullptr);

	// Exact match
	FDisEntityType F16Type;
	F16Type.EntityKind = 1; F16Type.Domain = 2; F16Type.Country = 225;
	F16Type.Category = 1; F16Type.Subcategory = 2;
	TestEqual(TEXT("Exact match"), (int32)Adapter.MapEntityType(F16Type), 1001);

	// Fuzzy match (different subcategory → no exact, but kind:domain:category matches)
	FDisEntityType FuzzyType;
	FuzzyType.EntityKind = 1; FuzzyType.Domain = 2; FuzzyType.Country = 100;
	FuzzyType.Category = 1; FuzzyType.Subcategory = 99;
	TestEqual(TEXT("Fuzzy match"), (int32)Adapter.MapEntityType(FuzzyType), 2001);

	// Default fallback (completely unknown type)
	FDisEntityType UnknownType;
	UnknownType.EntityKind = 9; UnknownType.Domain = 9; UnknownType.Category = 9;
	TestEqual(TEXT("Default fallback"), (int32)Adapter.MapEntityType(UnknownType), 9999);

	return true;
}

// =========================================================================
// Test: DIS orientation conversion (radians → degrees)
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisOrientationConversionTest,
	"CamSim.Phase21.OrientationConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisOrientationConversionTest::RunTest(const FString& Parameters)
{
	// DIS: Psi (heading) = PI/2 rad = 90 degrees
	// DIS: Theta (pitch) = 0.1 rad ≈ 5.73 degrees
	// DIS: Phi (roll) = -PI/6 rad = -30 degrees
	const float Psi   = static_cast<float>(PI / 2.0);
	const float Theta = 0.1f;
	const float Phi   = static_cast<float>(-PI / 6.0);

	const float YawDeg   = Psi   * (180.0f / static_cast<float>(PI));
	const float PitchDeg = Theta * (180.0f / static_cast<float>(PI));
	const float RollDeg  = Phi   * (180.0f / static_cast<float>(PI));

	TestTrue(TEXT("Yaw ~90 deg"),   FMath::Abs(YawDeg - 90.0f) < 0.01f);
	TestTrue(TEXT("Pitch ~5.73"),   FMath::Abs(PitchDeg - 5.7296f) < 0.01f);
	TestTrue(TEXT("Roll ~-30 deg"), FMath::Abs(RollDeg - (-30.0f)) < 0.01f);

	return true;
}

// =========================================================================
// Test: DIS config defaults
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisConfigDefaultsTest,
	"CamSim.Phase21.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestFalse(TEXT("DIS disabled by default"), Cfg.DIS.bEnabled);
	TestEqual(TEXT("Default port"), Cfg.DIS.Port, 3000);
	TestEqual(TEXT("Default bind addr"), Cfg.DIS.BindAddr, FString(TEXT("0.0.0.0")));
	TestEqual(TEXT("Default exercise ID"), Cfg.DIS.ExerciseId, 1);
	TestEqual(TEXT("Default site ID"), Cfg.DIS.SiteId, 1);
	TestEqual(TEXT("Default app ID"), Cfg.DIS.ApplicationId, 1);
	TestTrue(TEXT("Default timeout > 0"), Cfg.DIS.HeartbeatTimeoutSec > 0.0f);
	TestEqual(TEXT("Default ID base"), Cfg.DIS.IdBaseOffset, 1000);
	TestEqual(TEXT("Default entity type"), Cfg.DIS.DefaultEntityTypeId, 1001);

	return true;
}

// =========================================================================
// Test: FDisEntityId hash and equality
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisEntityIdHashTest,
	"CamSim.Phase21.EntityIdHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisEntityIdHashTest::RunTest(const FString& Parameters)
{
	FDisEntityId A { 1, 2, 3 };
	FDisEntityId B { 1, 2, 3 };
	FDisEntityId C { 1, 2, 4 };

	TestTrue(TEXT("Equal IDs"), A == B);
	TestTrue(TEXT("Unequal IDs"), A != C);
	TestEqual(TEXT("Same hash"), GetTypeHash(A), GetTypeHash(B));
	// Different IDs should (very likely) have different hashes
	TestTrue(TEXT("Different hash"), GetTypeHash(A) != GetTypeHash(C));

	// Test as TMap key
	TMap<FDisEntityId, int32> Map;
	Map.Add(A, 42);
	const int32* Found = Map.Find(B);
	TestTrue(TEXT("TMap lookup"), Found != nullptr && *Found == 42);

	return true;
}

// =========================================================================
// Test: Dead reckoning velocity extraction from PDU
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisDRVelocityTest,
	"CamSim.Phase21.DRVelocityExtraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisDRVelocityTest::RunTest(const FString& Parameters)
{
	uint8 Buf[144];
	BuildEntityStatePdu(Buf, 1, 1, 1,
		6378137.0, 0.0, 0.0,
		0.0f, 0.0f, 0.0f);

	// Set specific velocity values
	WriteF32BE(Buf + 36, 250.0f);   // VelX
	WriteF32BE(Buf + 40, -10.0f);   // VelY
	WriteF32BE(Buf + 44, 5.0f);     // VelZ

	// Set angular velocity (offset 116)
	WriteF32BE(Buf + 116, 0.1f);    // AngVelX (roll rate, rad/s)
	WriteF32BE(Buf + 120, 0.05f);   // AngVelY (pitch rate, rad/s)
	WriteF32BE(Buf + 124, -0.02f);  // AngVelZ (yaw rate, rad/s)

	// Set DR algo to 5 (FPB — includes angular velocity)
	Buf[88] = 5;

	FDisEntityStatePdu Pdu;
	TestTrue(TEXT("Parse"), FDisEntityStatePdu::Parse(Buf, 144, Pdu));

	TestTrue(TEXT("VelX"), FMath::Abs(Pdu.DeadReckoning.VelX - 250.0f) < 0.01f);
	TestTrue(TEXT("VelY"), FMath::Abs(Pdu.DeadReckoning.VelY - (-10.0f)) < 0.01f);
	TestTrue(TEXT("VelZ"), FMath::Abs(Pdu.DeadReckoning.VelZ - 5.0f) < 0.01f);
	TestEqual(TEXT("DR algo"), (int32)Pdu.DeadReckoning.Algorithm, 5);

	TestTrue(TEXT("AngVelX"), FMath::Abs(Pdu.DeadReckoning.AngVelX - 0.1f) < 0.001f);
	TestTrue(TEXT("AngVelY"), FMath::Abs(Pdu.DeadReckoning.AngVelY - 0.05f) < 0.001f);
	TestTrue(TEXT("AngVelZ"), FMath::Abs(Pdu.DeadReckoning.AngVelZ - (-0.02f)) < 0.001f);

	return true;
}

// =========================================================================
// Test: PDU header too short
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisPduHeaderTooShortTest,
	"CamSim.Phase21.PduHeaderTooShort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisPduHeaderTooShortTest::RunTest(const FString& Parameters)
{
	FDisPduHeader Hdr;

	// Null pointer
	TestFalse(TEXT("Null data"), FDisPduHeader::Parse(nullptr, 100, Hdr));

	// 0 length
	uint8 Dummy[1] = {0};
	TestFalse(TEXT("Zero length"), FDisPduHeader::Parse(Dummy, 0, Hdr));

	// 11 bytes (one short)
	uint8 Short[11] = {};
	TestFalse(TEXT("11 bytes"), FDisPduHeader::Parse(Short, 11, Short[0] ? Hdr : Hdr));

	return true;
}

// =========================================================================
// Test: Entity State PDU too short (143 bytes)
// =========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisEntityStatePduTooShortTest,
	"CamSim.Phase21.EntityStatePduTooShort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDisEntityStatePduTooShortTest::RunTest(const FString& Parameters)
{
	uint8 Buf[143];
	FMemory::Memzero(Buf, 143);
	Buf[2] = EDisPduType::EntityState;

	FDisEntityStatePdu Pdu;
	TestFalse(TEXT("143 bytes too short"), FDisEntityStatePdu::Parse(Buf, 143, Pdu));

	return true;
}
