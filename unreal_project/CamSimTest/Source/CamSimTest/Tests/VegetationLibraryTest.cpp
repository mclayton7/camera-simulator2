// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/StaticMesh.h"
#include "Entity/EntityMeshLoader.h"
#include "Vegetation/VegetationLibrary.h"

using namespace CamSimVegetation;

namespace
{
	FString LibTempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Vegetation"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}
	FString Entry(const FString& Sha) 
	{
		return FString::Printf(TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9.0,")
			TEXT("\"thermal_class\":\"tree_canopy\",\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"%s\"}]}"), *Sha);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibSha256Test, "CamSim.Vegetation.Library.Sha256",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibSha256Test::RunTest(const FString& Parameters)
{
	const FTCHARToUTF8 Abc(TEXT("abc"));
	TestEqual(TEXT("FIPS 180-2 'abc'"), Sha256Hex(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Abc.Get()), Abc.Length())),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("empty"), Sha256Hex({}), FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibParseTest, "CamSim.Vegetation.Library.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibParseTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	TestTrue(TEXT("valid"), ParseLibrary(Entry(FString::ChrN(64, 'a')), TEXT("/lib"), L, Err));
	TestEqual(TEXT("one type"), L.Types.Num(), 1);
	if (L.Types.Num() == 1)
	{
		TestEqual(TEXT("absolute gltf path"), L.Types[0].GltfPath, FString(TEXT("/lib/t.glb")));
		TestEqual(TEXT("height"), L.Types[0].HeightM, 9.0f);
		TestFalse(TEXT("not procedural"), L.Types[0].bProcedural);
	}
	TestTrue(TEXT("procedural without gltf/sha"), ParseLibrary(TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"p\",")
		TEXT("\"procedural\":true,\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"generated\"}]}"), TEXT("/lib"), L, Err));
	TestTrue(TEXT("procedural flag"), L.Types.Num() == 1 && L.Types[0].bProcedural);
	TestEqual(TEXT("thermal class default"), L.Types.Num() == 1 ? L.Types[0].ThermalClass : FString(), FString(TEXT("tree_canopy")));

	const TCHAR* Bad[] = {
		TEXT("not json"),
		TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"u\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":0,\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"source_url\":\"u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}"),
		TEXT("{\"format\":\"camsim-vegetation-1\",\"types\":[{\"name\":\"t\",\"gltf\":\"t.glb\",\"height_m\":9,\"licence\":\"CC0-1.0\",\"source_url\":\"u\",\"sha256\":\"xyz\"}]}"),
	};
	for (const TCHAR* J : Bad)
	{
		FString E;
		TestFalse(FString::Printf(TEXT("rejects %s"), J), ParseLibrary(J, TEXT("/lib"), L, E));
		TestFalse(TEXT("with a reason"), E.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibLoadTest, "CamSim.Vegetation.Library.LoadVerifiesSha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibLoadTest::RunTest(const FString& Parameters)
{
	const FString D = LibTempDir(TEXT("Load"));
	const FTCHARToUTF8 Body(TEXT("glb bytes"));
	TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Body.Get()), Body.Length());
	FFileHelper::SaveArrayToFile(Bytes, *(D / TEXT("t.glb")));
	FVegetationLibrary L; FString Err;

	FFileHelper::SaveStringToFile(Entry(Sha256Hex(Bytes)), *(D / TEXT("library.json")));
	TestTrue(TEXT("matching sha loads"), LoadLibrary(D, L, Err));

	FFileHelper::SaveStringToFile(Entry(FString::ChrN(64, 'b')), *(D / TEXT("library.json")));
	TestFalse(TEXT("sha mismatch refused"), LoadLibrary(D, L, Err));
	TestTrue(TEXT("names the file"), Err.Contains(TEXT("t.glb")));

	const FTCHARToUTF8 Ptr(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:00\nsize 1\n"));
	FFileHelper::SaveArrayToFile(TArray<uint8>(reinterpret_cast<const uint8*>(Ptr.Get()), Ptr.Length()), *(D / TEXT("t.glb")));
	FFileHelper::SaveStringToFile(Entry(Sha256Hex(Bytes)), *(D / TEXT("library.json")));
	TestFalse(TEXT("LFS pointer refused"), LoadLibrary(D, L, Err));
	TestTrue(TEXT("hints git lfs pull"), Err.Contains(TEXT("git lfs pull")));

	IFileManager::Get().Delete(*(D / TEXT("t.glb")));
	TestFalse(TEXT("missing file refused"), LoadLibrary(D, L, Err));
	TestFalse(TEXT("missing library.json refused"), LoadLibrary(LibTempDir(TEXT("Empty")), L, Err));

	TestTrue(TEXT("built-in dir default"), ResolveLibraryDir(TEXT("")).EndsWith(TEXT("Content/NonUFS/Vegetation")));
	TestEqual(TEXT("absolute kept"), ResolveLibraryDir(TEXT("/mnt/veg")), FString(TEXT("/mnt/veg")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibShippedTest, "CamSim.Vegetation.Library.ShippedLibraryLoads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibShippedTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	const bool bOk = LoadLibrary(ResolveLibraryDir(TEXT("")), L, Err);
	TestTrue(FString::Printf(TEXT("Content/NonUFS/Vegetation loads (%s; run git lfs pull if it is a pointer)"), *Err), bOk);
	TestTrue(TEXT("at least one type"), L.Types.Num() >= 1);
	return true;
}

namespace
{
	FString V2(const FString& TypesJson) { return TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[") + TypesJson + TEXT("]}"); }
	FString TypeJson(const TCHAR* Name, const TCHAR* Role, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"name\":\"%s\",\"role\":\"%s\",\"gltf\":\"%s.glb\",\"sha256\":\"%s\",\"height_m\":10.0,")
			TEXT("\"licence\":\"CC0-1.0\",\"source_url\":\"u\"%s}"), Name, Role, Name, *FString::ChrN(64, 'a'), *Extra);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibV2Test, "CamSim.Vegetation.Library.V2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibV2Test::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	const FString Far = FString::Printf(TEXT(",\"far_gltf\":\"p_far.glb\",\"far_sha256\":\"%s\",\"crown_radius_m\":2.5"), *FString::ChrN(64, 'b'));
	TestTrue(TEXT("v2 parses"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree"), Far) + TEXT(",") + TypeJson(TEXT("s"), TEXT("shrub"))), TEXT("/lib"), L, Err));
	if (TestEqual(TEXT("two types"), L.Types.Num(), 2))
	{
		TestTrue(TEXT("tree role"), L.Types[0].Role == EVegRole::Tree);
		TestEqual(TEXT("crown given"), L.Types[0].CrownRadiusM, 2.5f);
		TestEqual(TEXT("far path"), L.Types[0].FarGltfPath, FString(TEXT("/lib/p_far.glb")));
		TestTrue(TEXT("has far"), L.Types[0].HasFarMesh());
		TestTrue(TEXT("shrub role"), L.Types[1].Role == EVegRole::Shrub);
		TestEqual(TEXT("shrub crown default 0.5 h"), L.Types[1].CrownRadiusM, 5.0f);
		TestFalse(TEXT("shrub no far"), L.Types[1].HasFarMesh());
		TestEqual(TEXT("find"), FindType(L, TEXT("s")), 1);
		TestEqual(TEXT("find missing"), FindType(L, TEXT("x")), -1);
	}
	TestFalse(TEXT("unknown role"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("grass"))), TEXT("/lib"), L, Err));
	TestTrue(TEXT("unknown role message"), Err.Contains(TEXT("role")));
	TestFalse(TEXT("duplicate name"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree")) + TEXT(",") + TypeJson(TEXT("p"), TEXT("shrub"))), TEXT("/lib"), L, Err));
	TestFalse(TEXT("shrub with far mesh"), ParseLibrary(V2(TypeJson(TEXT("s"), TEXT("shrub"), Far)), TEXT("/lib"), L, Err));
	TestFalse(TEXT("far without sha"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree"), TEXT(",\"far_gltf\":\"p_far.glb\""))), TEXT("/lib"), L, Err));
	TestFalse(TEXT("crown out of range"), ParseLibrary(V2(TypeJson(TEXT("p"), TEXT("tree"), TEXT(",\"crown_radius_m\":0"))), TEXT("/lib"), L, Err));
	TestFalse(TEXT("unknown format"), ParseLibrary(TEXT("{\"format\":\"camsim-vegetation-3\",\"types\":[]}"), TEXT("/lib"), L, Err));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibV1CompatTest, "CamSim.Vegetation.Library.V1Compat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibV1CompatTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	TestTrue(TEXT("v1 parses"), ParseLibrary(Entry(FString::ChrN(64, 'a')), TEXT("/lib"), L, Err));
	if (TestEqual(TEXT("one"), L.Types.Num(), 1))
	{
		TestTrue(TEXT("v1 = tree"), L.Types[0].Role == EVegRole::Tree);
		TestEqual(TEXT("v1 crown default 0.3 h"), L.Types[0].CrownRadiusM, 2.7f);
		TestFalse(TEXT("v1 no far"), L.Types[0].HasFarMesh());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibFarShaTest, "CamSim.Vegetation.Library.FarMeshSha",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibFarShaTest::RunTest(const FString& Parameters)
{
	const FString Dir = LibTempDir(TEXT("FarSha"));
	const TArray<uint8> Near = { 1, 2, 3 }, FarB = { 4, 5 };
	FFileHelper::SaveArrayToFile(Near, *FPaths::Combine(Dir, TEXT("p.glb")));
	FFileHelper::SaveArrayToFile(FarB, *FPaths::Combine(Dir, TEXT("p_far.glb")));
	const FString Good = FString::Printf(TEXT("{\"format\":\"camsim-vegetation-2\",\"types\":[{\"name\":\"p\",\"role\":\"tree\",\"gltf\":\"p.glb\",\"sha256\":\"%s\",")
		TEXT("\"far_gltf\":\"p_far.glb\",\"far_sha256\":\"%s\",\"height_m\":10,\"licence\":\"CC0-1.0\"}]}"), *Sha256Hex(Near), *Sha256Hex(FarB));
	FFileHelper::SaveStringToFile(Good, *FPaths::Combine(Dir, TEXT("library.json")));
	FVegetationLibrary L; FString Err;
	TestTrue(*FString::Printf(TEXT("both shas match (%s)"), *Err), LoadLibrary(Dir, L, Err));
	FFileHelper::SaveStringToFile(Good.Replace(*Sha256Hex(FarB), *FString::ChrN(64, 'c')), *FPaths::Combine(Dir, TEXT("library.json")));
	TestFalse(TEXT("far sha mismatch refused"), LoadLibrary(Dir, L, Err));
	TestTrue(TEXT("message names the far file"), Err.Contains(TEXT("p_far.glb")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLibShippedMeshesTest, "CamSim.Vegetation.Library.ShippedMeshesLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLibShippedMeshesTest::RunTest(const FString& Parameters)
{
	FVegetationLibrary L; FString Err;
	if (!TestTrue(FString::Printf(TEXT("library loads (%s)"), *Err), LoadLibrary(ResolveLibraryDir(TEXT("")), L, Err))) return true;
	TestEqual(TEXT("tree_broadleaf_a is index 0 (R4 identity)"), FindType(L, TEXT("tree_broadleaf_a")), 0);
	for (const FVegetationType& T : L.Types)
	{
		for (const FString& Path : { T.GltfPath, T.FarGltfPath })
		{
			if (Path.IsEmpty()) continue;
			UStaticMesh* Mesh = CamSimMeshLoader::LoadStaticMesh(Path);
			if (!TestNotNull(*FString::Printf(TEXT("%s loads"), *Path), Mesh)) continue;
			const FBox B = Mesh->GetBoundingBox();
			const double HeightM = (B.Max.Z - B.Min.Z) / 100.0;
			TestTrue(*FString::Printf(TEXT("%s height %.2f m vs %.2f m (2%%)"), *Path, HeightM, T.HeightM),
				FMath::Abs(HeightM - T.HeightM) <= 0.02 * T.HeightM);
			TestTrue(*FString::Printf(TEXT("%s base at 0 (%.3f m)"), *Path, B.Min.Z / 100.0), FMath::Abs(B.Min.Z) <= 0.02 * T.HeightM * 100.0);
		}
	}
	return true;
}
