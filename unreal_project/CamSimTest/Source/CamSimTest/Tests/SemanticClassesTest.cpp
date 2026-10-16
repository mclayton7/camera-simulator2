// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/SemanticClasses.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Entity/EntityTypeTable.h"

// -------------------------------------------------------------------------
// Semantic classes (ROADMAP 4D): stable ids, land-cover code -> class, entity
// semantic_class, stencil -> class table.
// -------------------------------------------------------------------------

using namespace CamSimSemantic;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticCodeLutTest,
	"CamSim.GroundTruth.Semantic.Classes.CodeLut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSemanticCodeLutTest::RunTest(const FString& Parameters)
{
	const TStaticArray<uint8, 256> L = BuildCodeLut();
	struct FCase { int32 Code; uint8 Class; };
	const FCase Cases[] = {
		{ 0, Unlabeled }, { 10, Tree }, { 20, LowVegetation }, { 30, LowVegetation }, { 40, LowVegetation },
		{ 50, BuiltUp }, { 60, Bare }, { 70, SnowIce }, { 80, Water }, { 90, LowVegetation }, { 95, LowVegetation },
		{ 100, LowVegetation }, { 200, Road }, { 201, Road }, { 210, Rail }, { 220, Water }, { 230, Building },
		{ 11, Unlabeled }, { 99, Unlabeled }, { 202, Unlabeled }, { 255, Unlabeled },
	};
	for (const FCase& C : Cases)
	{
		TestEqual(FString::Printf(TEXT("code %d"), C.Code), L[C.Code], C.Class);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticNamesTest,
	"CamSim.GroundTruth.Semantic.Classes.Names",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSemanticNamesTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> All = AllClasses();
	TestEqual(TEXT("17 classes"), All.Num(), 17);
	TestEqual(TEXT("road"), FString(Name(Road)), FString(TEXT("road")));
	TestEqual(TEXT("low_vegetation"), FString(Name(LowVegetation)), FString(TEXT("low_vegetation")));
	TestEqual(TEXT("other_entity"), FString(Name(OtherEntity)), FString(TEXT("other_entity")));
	TestEqual(TEXT("unused id"), FString(Name(15)), FString());
	for (const uint8 C : All) TestFalse(FString::Printf(TEXT("class %u named"), C), FString(Name(C)).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticEntityClassTest,
	"CamSim.GroundTruth.Semantic.Classes.EntityClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSemanticEntityClassTest::RunTest(const FString& Parameters)
{
	uint8 C = 0;
	const TPair<const TCHAR*, uint8> Names[] = { {TEXT("vehicle"), Vehicle}, {TEXT("vessel"), Vessel},
		{TEXT("aircraft"), Aircraft}, {TEXT("person"), Person}, {TEXT("marker"), Marker}, {TEXT("other"), OtherEntity} };
	for (const auto& N : Names)
	{
		TestTrue(N.Key, ParseEntityClass(N.Key, C));
		TestEqual(N.Key, C, N.Value);
	}
	TestFalse(TEXT("unknown"), ParseEntityClass(TEXT("tank"), C));
	TestEqual(TEXT("character -> person"), DefaultEntityClass(TEXT("character")), static_cast<uint8>(Person));
	TestEqual(TEXT("vehicle category -> other"), DefaultEntityClass(TEXT("vehicle")), static_cast<uint8>(OtherEntity));
	TestEqual(TEXT("none -> other"), DefaultEntityClass(FString()), static_cast<uint8>(OtherEntity));

	AddExpectedMessage(TEXT("semantic_class \"tank\""), EAutomationExpectedErrorFlags::Contains, 1);
	FEntityTypeTable T;
	T.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"3001\":\n"
		"    mesh: boat/mako_655.glb\n"
		"    semantic_class: vessel\n"
		"  \"1010\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"    entity_category: character\n"
		"  \"2001\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"    semantic_class: tank\n"));
	TestEqual(TEXT("configured"), T.SemanticClassOf(3001), static_cast<uint8>(Vessel));
	TestEqual(TEXT("character default"), T.SemanticClassOf(1010), static_cast<uint8>(Person));
	TestEqual(TEXT("bad name falls back"), T.SemanticClassOf(2001), static_cast<uint8>(OtherEntity));
	TestEqual(TEXT("unknown type"), T.SemanticClassOf(7777), static_cast<uint8>(OtherEntity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticStencilTableTest,
	"CamSim.GroundTruth.Semantic.Classes.StencilTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSemanticStencilTableTest::RunTest(const FString& Parameters)
{
	TArray<FEntityAnnotationData> E;
	FEntityAnnotationData A; A.StencilValue = 3; A.EntityType = 2001; E.Add(A);
	FEntityAnnotationData B; B.StencilValue = 7; B.EntityType = 3001; E.Add(B);
	FEntityAnnotationData U; U.StencilValue = 0; U.EntityType = 3001; E.Add(U);   // untagged: ignored
	const TStaticArray<uint8, 256> T = BuildStencilClassTable(E, [](uint16 Type) -> uint8
	{
		return Type == 2001 ? Vehicle : Vessel;
	});
	TestEqual(TEXT("stencil 3"), T[3], static_cast<uint8>(Vehicle));
	TestEqual(TEXT("stencil 7"), T[7], static_cast<uint8>(Vessel));
	TestEqual(TEXT("stencil 0 never set"), T[0], static_cast<uint8>(0));
	TestEqual(TEXT("a stencil with no entity this frame: 0 (falls through to terrain)"), T[5], static_cast<uint8>(0));
	return true;
}
