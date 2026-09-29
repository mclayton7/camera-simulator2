// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/EntityTypeTable.h"

// -------------------------------------------------------------------------
// EntityTypeTable Automation Tests
//
// Validates type-map population, lookup, and cache behavior.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityTypeTableLookupTest,
	"CamSim.EntityTypeTable.Lookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityTypeTableLookupTest::RunTest(const FString& Parameters)
{
	FEntityTypeTable Table;

	// Before loading, all lookups should return nullptr
	TestNull(TEXT("Empty table returns nullptr"), Table.FindEntry(1001));
	TestNull(TEXT("Type 0 returns nullptr"), Table.FindEntry(0));
	TestNull(TEXT("Type 65535 returns nullptr"), Table.FindEntry(65535));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityTypeTableCacheTest,
	"CamSim.EntityTypeTable.Cache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityTypeTableCacheTest::RunTest(const FString& Parameters)
{
	FEntityTypeTable Table;

	// Cache should return nullptr for unknown types
	TestNull(TEXT("Static mesh cache empty"), Table.GetCachedStaticMesh(1001));
	TestNull(TEXT("Skeletal mesh cache empty"), Table.GetCachedSkeletalMesh(1001));

	// Setting nullptr and retrieving should return nullptr
	Table.SetCachedStaticMesh(1001, nullptr);
	// Weak pointers to nullptr are valid but return nullptr
	TestNull(TEXT("Cached nullptr static mesh"), Table.GetCachedStaticMesh(1001));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityTypeTableZOffsetTest,
	"CamSim.EntityTypeTable.ZOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityTypeTableZOffsetTest::RunTest(const FString& Parameters)
{
	// glTF paths only need the file to exist (no RHI), so this runs under NullRHI.
	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"2001\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"  \"3001\":\n"
		"    mesh: boat/mako_655.glb\n"
		"    z_offset_m: -0.49\n"));

	const FEntityTypeEntry* Truck = Table.FindEntry(2001);
	const FEntityTypeEntry* Boat = Table.FindEntry(3001);
	if (!TestNotNull(TEXT("truck entry"), Truck) || !TestNotNull(TEXT("boat entry"), Boat)) return false;
	TestEqual(TEXT("absent z_offset_m defaults to 0"), Truck->ModelZOffsetCm, 0.0f);
	TestEqual(TEXT("z_offset_m stored in cm"), Boat->ModelZOffsetCm, -49.0f, 1e-3f);
	return true;
}
