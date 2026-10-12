// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Vegetation/VegetationLibrary.h"
#include "Vegetation/VegetationMix.h"

using namespace CamSimVegetation;

namespace
{
	FVegetationLibrary MixLib()
	{
		FVegetationLibrary L;
		auto Add = [&L](const TCHAR* N, EVegRole R, float H) { FVegetationType T; T.Name = N; T.Role = R; T.HeightM = H; T.CrownRadiusM = 0.3f * H; L.Types.Add(T); };
		Add(TEXT("tree_broadleaf_a"), EVegRole::Tree, 9.0f);
		Add(TEXT("tree_pine_a"), EVegRole::Tree, 16.0f);
		Add(TEXT("shrub_chaparral"), EVegRole::Shrub, 1.8f);
		return L;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixParseTest, "CamSim.Vegetation.Mix.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixParseTest::RunTest(const FString& Parameters)
{
	const FString J = TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_a\":3,\"tree_broadleaf_a\":1,\"shrub_chaparral\":2},")
		TEXT("\"20\":{\"shrub_chaparral\":1}},\"shrub_density\":{\"20\":8}}");
	FVegetationMix M; TArray<FString> Unknown; FString Err;
	TestTrue(*FString::Printf(TEXT("parses (%s)"), *Err), ParseMix(J, MixLib(), M, Unknown, Err));
	TestEqual(TEXT("no unknown names"), Unknown.Num(), 0);
	const FCodeMix* C10 = M.Find(10);
	if (TestNotNull(TEXT("code 10"), C10) && TestEqual(TEXT("two trees"), C10->Trees.Num(), 2))
	{
		TestEqual(TEXT("names in alphabetical order: broadleaf first"), C10->Trees[0].Type, 0);
		TestEqual(TEXT("broadleaf cum 0.25"), C10->Trees[0].CumWeight, 0.25f);
		TestEqual(TEXT("pine last"), C10->Trees[1].Type, 1);
		TestEqual(TEXT("last cum 1"), C10->Trees[1].CumWeight, 1.0f);
		TestEqual(TEXT("shrub list normalised alone"), C10->Shrubs.Num() == 1 ? C10->Shrubs[0].CumWeight : -1.0f, 1.0f);
		TestEqual(TEXT("10 has shrubs but no density: default"), C10->ShrubsPer100M2, DefaultShrubsPer100M2);
		TestEqual(TEXT("PickType low -> entry 0 (broadleaf)"), C10->Trees[PickType(C10->Trees, 0.1f)].Type, 0);
		TestEqual(TEXT("PickType high -> entry 1 (pine)"), C10->Trees[PickType(C10->Trees, 0.9f)].Type, 1);
		TestEqual(TEXT("PickType at 0.25 -> pine (U < Cum)"), C10->Trees[PickType(C10->Trees, 0.25f)].Type, 1);
	}
	const FCodeMix* C20 = M.Find(20);
	if (TestNotNull(TEXT("code 20"), C20)) { TestEqual(TEXT("no trees"), C20->Trees.Num(), 0); TestEqual(TEXT("density"), C20->ShrubsPer100M2, 8.0f); }
	TestNull(TEXT("code 30 absent"), M.Find(30));
	TestEqual(TEXT("type heights carried"), M.TypeHeightM.Num() == 3 ? M.TypeHeightM[1] : 0.0f, 16.0f);
	TestEqual(TEXT("type crowns carried"), M.TypeCrownRadiusM.Num() == 3 ? M.TypeCrownRadiusM[1] : 0.0f, 4.8f);
	TestEqual(TEXT("PickType empty"), PickType({}, 0.5f), -1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixUnknownTest, "CamSim.Vegetation.Mix.UnknownNamesDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixUnknownTest::RunTest(const FString& Parameters)
{
	const FString J = TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_b\":1,\"tree_pine_a\":1},\"30\":{\"cactus_a\":1}}}");
	FVegetationMix M; TArray<FString> Unknown; FString Err;
	TestTrue(TEXT("parses"), ParseMix(J, MixLib(), M, Unknown, Err));
	TestEqual(TEXT("two unknown"), Unknown.Num(), 2);
	TestTrue(TEXT("named"), Unknown.Contains(TEXT("tree_pine_b")) && Unknown.Contains(TEXT("cactus_a")));
	const FCodeMix* C10 = M.Find(10);
	if (TestNotNull(TEXT("10 kept"), C10)) { TestEqual(TEXT("renormalised"), C10->Trees.Num(), 1); TestEqual(TEXT("cum 1"), C10->Trees[0].CumWeight, 1.0f); }
	TestNull(TEXT("30 empty -> absent"), M.Find(30));
	TestFalse(TEXT("bad format"), ParseMix(TEXT("{\"format\":\"x\",\"mix\":{}}"), MixLib(), M, Unknown, Err));
	TestFalse(TEXT("bad code"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"300\":{\"tree_pine_a\":1}}}"), MixLib(), M, Unknown, Err));
	TestFalse(TEXT("non-numeric code"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"x1\":{\"tree_pine_a\":1}}}"), MixLib(), M, Unknown, Err));
	TestFalse(TEXT("non-positive weight"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_a\":0}}}"), MixLib(), M, Unknown, Err));
	TestFalse(TEXT("density out of range"), ParseMix(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"20\":{\"shrub_chaparral\":1}},\"shrub_density\":{\"20\":101}}"), MixLib(), M, Unknown, Err));
	TestFalse(TEXT("not json"), ParseMix(TEXT("nope"), MixLib(), M, Unknown, Err));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixDefaultTest, "CamSim.Vegetation.Mix.Default",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixDefaultTest::RunTest(const FString& Parameters)
{
	const FVegetationMix M = DefaultMix(MixLib());
	if (TestNotNull(TEXT("10"), M.Find(10)))
	{
		TestEqual(TEXT("10 -> type 0 (R4)"), M.Find(10)->Trees[0].Type, 0);
		TestEqual(TEXT("10 only one tree"), M.Find(10)->Trees.Num(), 1);
		TestEqual(TEXT("10 no shrubs"), M.Find(10)->Shrubs.Num(), 0);
	}
	if (TestNotNull(TEXT("20"), M.Find(20))) TestEqual(TEXT("20 shrubs 6"), M.Find(20)->ShrubsPer100M2, 6.0f);
	if (TestNotNull(TEXT("30"), M.Find(30))) TestEqual(TEXT("30 shrubs 1"), M.Find(30)->ShrubsPer100M2, 1.0f);
	FVegetationLibrary NoShrubs = MixLib(); NoShrubs.Types.RemoveAt(2);
	const FVegetationMix M2 = DefaultMix(NoShrubs);
	TestNull(TEXT("no shrub type: no code 20"), M2.Find(20));
	TestEqual(TEXT("still R4 trees"), M2.Find(10) ? M2.Find(10)->Trees[0].Type : -1, 0);
	FVegetationLibrary ShrubFirst = MixLib(); ShrubFirst.Types.Swap(0, 2);
	TestNull(TEXT("index 0 not a tree: no code 10"), DefaultMix(ShrubFirst).Find(10));
	const FVegetationMix S = SingleTypeMix(10, 0, 9.0f, 2.7f);
	TestEqual(TEXT("single: one code"), S.Codes.Num(), 1);
	TestEqual(TEXT("single: height"), S.TypeHeightM.Num() == 1 ? S.TypeHeightM[0] : 0.0f, 9.0f);
	return true;
}

#include "Config/CamSimConfig.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Vegetation/VegetationManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMixResolveTest, "CamSim.Vegetation.Mix.ResolveOfflineIsFatal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMixResolveTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Vegetation"), TEXT("MixResolve")));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Path = Dir / TEXT("mix.json");
	FFileHelper::SaveStringToFile(TEXT("{\"format\":\"camsim-vegmix-1\",\"mix\":{\"10\":{\"tree_pine_a\":1,\"tree_pine_z\":1}}}"), *Path);
	AddExpectedMessagePlain(TEXT("mix names unknown type 'tree_pine_z'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("missing.json unusable"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FCamSimConfig Cfg;
	Cfg.Scene.VegetationMixPath = Path;
	FString Source, Why; bool bFatal = false;
	TSharedPtr<FVegetationMix, ESPMode::ThreadSafe> M = ResolveMix(Cfg, MixLib(), Source, Why, bFatal);
	TestTrue(TEXT("online: a mix"), M.IsValid());
	TestFalse(TEXT("online: not fatal"), bFatal);
	TestEqual(TEXT("source package"), Source, FString(TEXT("package")));
	TestEqual(TEXT("known type kept alone"), M.IsValid() && M->Find(10) ? M->Find(10)->Trees.Num() : -1, 1);
	Cfg.Scene.bOffline = true;
	M = ResolveMix(Cfg, MixLib(), Source, Why, bFatal);
	TestFalse(TEXT("offline: no mix"), M.IsValid());
	TestTrue(TEXT("offline: fatal"), bFatal);
	TestTrue(TEXT("names the type"), Why.Contains(TEXT("tree_pine_z")));
	Cfg.Scene.bOffline = false; bFatal = false;
	Cfg.Scene.VegetationMixPath.Reset();
	Cfg.Vegetation.MixFile = Dir / TEXT("missing.json");
	M = ResolveMix(Cfg, MixLib(), Source, Why, bFatal);
	TestTrue(TEXT("unreadable file online: default mix"), M.IsValid() && M->Find(10) != nullptr);
	TestEqual(TEXT("source default"), Source, FString(TEXT("default")));
	Cfg.Vegetation.MixFile.Reset();
	M = ResolveMix(Cfg, MixLib(), Source, Why, bFatal);
	TestEqual(TEXT("no mix anywhere: default"), Source, FString(TEXT("default")));
	return true;
}
