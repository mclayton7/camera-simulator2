// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
