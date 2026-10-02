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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityTypeThermalPartsTest, "CamSim.Thermal.Entity.TypeTable.ThermalParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityTypeThermalPartsTest::RunTest(const FString& Parameters)
{
	// ROADMAP 4C: hot-spot volumes; invalid parts skipped, at most 4 kept (in order), tiny extents clamped.
	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"2001\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"    thermal_parts:\n"
		"      - { kind: running_gear, shape: box, centre_m: [0.47, 0, -0.62], half_m: [3.7, 1.55, 0.62], falloff_m: 0.15 }\n"
		"      - { kind: engine, shape: ellipsoid, centre_m: [3.45, 0, -1.45], half_m: [0.75, 0.75, 0.45], falloff_m: 0.3, delta_k: 30 }\n"
		"      - { kind: exhaust, shape: box, centre_m: [1.9, 1.25, -0.75], half_m: [0.5, 0.2, 0.2], falloff_m: 0.15, temp_k: 500 }\n"
		"      - { kind: plasma, shape: box, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n"
		"      - { kind: engine, shape: cone, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n"
		"      - { kind: engine, shape: box, centre_m: [0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n"
		"      - { kind: engine, shape: box, centre_m: [0, 0, .nan], half_m: [1, 1, 1], falloff_m: 0.1 }\n"
		"      - { kind: engine, shape: box, centre_m: [0, 0, 0], half_m: [0.001, 1, 1], falloff_m: 0 }\n"
		"      - { kind: exhaust, shape: box, centre_m: [0, 0, 0], half_m: [1, 1, 1], falloff_m: 0.1 }\n"
		"  \"3001\":\n"
		"    mesh: boat/mako_655.glb\n"));
	const FEntityTypeEntry* E = Table.FindEntry(2001);
	const FEntityTypeEntry* Boat = Table.FindEntry(3001);
	if (!TestNotNull(TEXT("entry"), E) || !TestNotNull(TEXT("boat"), Boat)) return false;
	TestEqual(TEXT("no parts by default"), Boat->ThermalParts.Num(), 0);
	if (!TestEqual(TEXT("parts (4 valid kept, the 5th valid dropped)"), E->ThermalParts.Num(), 4)) return false;
	TestEqual(TEXT("gear kind"), E->ThermalParts[0].Kind, EEntityThermalPartKind::RunningGear);
	TestEqual(TEXT("gear centre x"), E->ThermalParts[0].CentreM.X, 0.47f);
	TestEqual(TEXT("gear centre z"), E->ThermalParts[0].CentreM.Z, -0.62f);
	TestEqual(TEXT("gear half y"), E->ThermalParts[0].HalfM.Y, 1.55f);
	TestEqual(TEXT("gear falloff"), E->ThermalParts[0].FalloffM, 0.15f);
	TestEqual(TEXT("engine shape"), E->ThermalParts[1].Shape, EEntityThermalPartShape::Ellipsoid);
	TestTrue(TEXT("engine delta override"), E->ThermalParts[1].DeltaK.IsSet() && *E->ThermalParts[1].DeltaK == 30.0f);
	TestFalse(TEXT("gear no override"), E->ThermalParts[0].DeltaK.IsSet());
	TestTrue(TEXT("exhaust temp override"), E->ThermalParts[2].TempK.IsSet() && *E->ThermalParts[2].TempK == 500.0f);
	TestEqual(TEXT("4th: engine"), E->ThermalParts[3].Kind, EEntityThermalPartKind::Engine);
	TestEqual(TEXT("tiny half clamped"), E->ThermalParts[3].HalfM.X, FEntityThermalPartSpec::MinExtentM);
	TestEqual(TEXT("zero falloff clamped"), E->ThermalParts[3].FalloffM, FEntityThermalPartSpec::MinExtentM);
	return true;
}
