// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entity/EntityPaths.h"

// -------------------------------------------------------------------------
// CamSimEntityPaths::Resolve: relative paths land under entities/, absolute
// paths (vegetation library models, REALISM R4) pass through unchanged.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityPathsResolveTest,
	"CamSim.Entity.Paths.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityPathsResolveTest::RunTest(const FString& Parameters)
{
	const FString Abs = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Content/NonUFS/Vegetation/tree.glb")));
	TestEqual(TEXT("Absolute path unchanged"), CamSimEntityPaths::Resolve(Abs), Abs);

	const FString Rel = CamSimEntityPaths::Resolve(TEXT("vehicles/truck.glb"));
	TestFalse(TEXT("Relative path resolves to an absolute one"), FPaths::IsRelative(Rel));
	TestTrue(TEXT("Relative path lands under entities/"), Rel.EndsWith(TEXT("/entities/vehicles/truck.glb")));
	return true;
}
