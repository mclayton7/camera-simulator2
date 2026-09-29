// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "Entity/EntityTypeTable.h"
#include "Engine/StaticMesh.h"
#include "Config/CamSimConfig.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityModelFacingGpuTest, "CamSim.GPU.Entity.ModelFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEntityModelFacingGpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)"));
		return true;
	}

	// The shipped config: each vehicle, after its rotation and scale, is longest along
	// UE +X (the entity's forward) and matches its configured footprint within 10%.
	FString Yaml;
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml"));
	if (!TestTrue(TEXT("config read"), FFileHelper::LoadFileToString(Yaml, *Path))) return false;
	FEntityTypeTable Table;
	Table.LoadFromYamlString(Yaml);
	Table.PreloadGltfMeshes();

	for (const uint16 Type : { (uint16)2001, (uint16)3001 })
	{
		const FEntityTypeEntry* E = Table.FindEntry(Type);
		if (!TestNotNull(*FString::Printf(TEXT("type %u configured"), Type), E)) continue;
		UStaticMesh* Mesh = Table.GetCachedStaticMesh(Type);
		if (!TestNotNull(*FString::Printf(TEXT("type %u loaded"), Type), Mesh)) continue;

		const FBox Local = Mesh->GetBoundingBox();
		const FTransform Xf(E->ModelRotation, FVector::ZeroVector, FVector(E->ModelScale));
		const FVector Ext = Local.TransformBy(Xf).GetExtent();  // cm, entity space
		AddInfo(FString::Printf(TEXT("type %u extent (cm) X=%.1f Y=%.1f Z=%.1f"), Type, Ext.X, Ext.Y, Ext.Z));
		TestTrue(*FString::Printf(TEXT("type %u longest along +X"), Type), Ext.X > Ext.Y);
		TestEqual(*FString::Printf(TEXT("type %u half length"), Type), Ext.X, (double)E->HalfLengthCm, 0.1 * E->HalfLengthCm);
		TestEqual(*FString::Printf(TEXT("type %u half beam"), Type), Ext.Y, (double)E->HalfBeamCm, 0.1 * E->HalfBeamCm);
	}
	return true;
}
