// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "Entity/EntityTypeTable.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityPreloadGpuTest, "CamSim.GPU.Entity.PreloadTwiceKeepsMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityPreloadGpuTest::RunTest(const FString& Parameters)
{
	// glTFRuntime mesh builds need a real RHI (see SensorGpuTest.cpp SkipWithoutGpu).
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)"));
		return true;
	}

	// The "missing/not_there.glb" entry below is intentionally invalid, to
	// exercise the skip path — expect its preflight warning, not a failure.
	AddExpectedMessage(TEXT("glTF file not found"), EAutomationExpectedErrorFlags::Contains, 1);

	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"1001\":\n"
		"    mesh: f16/f16-c_falcon.glb\n"
		"  \"1002\":\n"
		"    mesh: missing/not_there.glb\n"));
	TestNotNull(TEXT("valid entry kept"), Table.FindEntry(1001));
	TestNull(TEXT("missing file skipped"), Table.FindEntry(1002));

	TestEqual(TEXT("one glTF preloaded"), Table.PreloadGltfMeshes(), 1);
	CollectGarbage(RF_NoFlags, true);
	UStaticMesh* First = Table.GetCachedStaticMesh(1001);
	TestNotNull(TEXT("survives GC"), First);

	// Hot reload path: preloading again keeps a resident mesh (no reload, no drop).
	TestEqual(TEXT("preload again"), Table.PreloadGltfMeshes(), 1);
	CollectGarbage(RF_NoFlags, true);
	TestTrue(TEXT("same mesh after second preload"), Table.GetCachedStaticMesh(1001) == First);
	return true;
}
