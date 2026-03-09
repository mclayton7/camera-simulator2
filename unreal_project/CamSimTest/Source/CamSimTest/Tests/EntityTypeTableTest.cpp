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
