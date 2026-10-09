// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Config/ScenePackage.h"

namespace
{
	/** An empty directory under Saved/Automation/Scene (absolute). */
	FString SceneTempDir(const TCHAR* Name)
	{
		const FString D = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Scene"), Name));
		IFileManager::Get().DeleteDirectory(*D, false, true);
		IFileManager::Get().MakeDirectory(*D, true);
		return D;
	}

	void WriteFile(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path);
	}

	/** A package with the given layers' entry files (contents are never parsed by CamSim). */
	FString MakePackage(const TCHAR* Name, bool bTerrain, bool bImagery, bool bLandCover,
		const FString& Manifest = TEXT("{\"schema_version\": 1, \"name\": \"testpkg\"}"))
	{
		const FString D = SceneTempDir(Name);
		WriteFile(D / TEXT("manifest.json"), Manifest);
		if (bTerrain)   WriteFile(D / TEXT("terrain/layer.json"), TEXT("{}"));
		if (bImagery)   WriteFile(D / TEXT("imagery/tilemapresource.xml"), TEXT("<TileMap/>"));
		if (bLandCover) WriteFile(D / TEXT("landcover/index.json"), TEXT("{}"));
		return D;
	}

	FCamSimConfig ResolvedConfigFor(const FString& Dir)
	{
		FCamSimConfig Cfg;
		Cfg.Scene.Dir = Dir;
		CamSimScene::ResolvePackage(Cfg);
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneFileUrlRoundTripTest,
	"CamSim.Scene.FileUrl.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneFileUrlRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace CamSimScene;
	TestEqual(TEXT("plain"), PathToFileUrl(TEXT("/data/pkg/terrain/layer.json")),
		FString(TEXT("file:///data/pkg/terrain/layer.json")));
	TestEqual(TEXT("space, plus, hash, percent"), PathToFileUrl(TEXT("/a b/c+d#e%f")),
		FString(TEXT("file:///a%20b/c%2Bd%23e%25f")));
	TestEqual(TEXT("UTF-8"), PathToFileUrl(TEXT("/caf\u00e9")), FString(TEXT("file:///caf%C3%A9")));
	TestEqual(TEXT("Windows drive"), PathToFileUrl(TEXT("C:\\x y\\l.json")), FString(TEXT("file:///C:/x%20y/l.json")));

	for (const TCHAR* P : { TEXT("/data/pkg/terrain/layer.json"), TEXT("/a b/c+d#e%f"), TEXT("/caf\u00e9"), TEXT("C:/x y/l.json") })
	{
		FString Back;
		TestTrue(FString::Printf(TEXT("decodes %s"), P), FileUrlToPath(PathToFileUrl(P), Back));
		TestEqual(FString::Printf(TEXT("round trip %s"), P), Back, FString(P));
	}

	FString Ignored;
	TestFalse(TEXT("https is not a file URL"), FileUrlToPath(TEXT("https://api.cesium.com/x"), Ignored));
	TestFalse(TEXT("file://host/ is not routed by Cesium"), FileUrlToPath(TEXT("file://host/x"), Ignored));
	TestFalse(TEXT("empty"), FileUrlToPath(TEXT(""), Ignored));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveFullPackageTest,
	"CamSim.Scene.Resolve.FullPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveFullPackageTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("full"), true, true, true);
	const FCamSimConfig Cfg = ResolvedConfigFor(D + TEXT("/"));   // trailing slash is accepted
	TestEqual(TEXT("no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	TestEqual(TEXT("no warnings"), Cfg.Scene.ResolveWarnings.Num(), 0);
	TestEqual(TEXT("package name"), Cfg.Scene.PackageName, FString(TEXT("testpkg")));
	TestEqual(TEXT("terrain source"), Cfg.CesiumBackend.Terrain.Source, FString(TEXT("url")));
	TestEqual(TEXT("terrain url"), Cfg.CesiumBackend.Terrain.Url,
		CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
	TestEqual(TEXT("imagery source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
	TestEqual(TEXT("imagery url"), Cfg.CesiumBackend.Imagery.Url,
		CamSimScene::PathToFileUrl(D / TEXT("imagery/tilemapresource.xml")));
	TestEqual(TEXT("land cover dir"), Cfg.Thermal.LandCover.Dir, D / TEXT("landcover"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveMissingLayersTest,
	"CamSim.Scene.Resolve.MissingLayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveMissingLayersTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Defaults;
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("terrain_only"), true, false, false));
		TestEqual(TEXT("terrain from package"), Cfg.CesiumBackend.Terrain.Source, FString(TEXT("url")));
		TestEqual(TEXT("imagery unchanged"), Cfg.CesiumBackend.Imagery.Source, Defaults.CesiumBackend.Imagery.Source);
		TestEqual(TEXT("land cover unchanged"), Cfg.Thermal.LandCover.Dir, Defaults.Thermal.LandCover.Dir);
	}
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("imagery_only"), false, true, false));
		TestEqual(TEXT("terrain unchanged"), Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
		TestEqual(TEXT("imagery from package"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
	}
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(MakePackage(TEXT("landcover_only"), false, false, true));
		TestEqual(TEXT("terrain unchanged"), Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
		TestTrue(TEXT("land cover from package"), Cfg.Thermal.LandCover.Dir.EndsWith(TEXT("landcover_only/landcover")));
		TestEqual(TEXT("no errors"), Cfg.Scene.ResolveErrors.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveNotAPackageTest,
	"CamSim.Scene.Resolve.NotAPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveNotAPackageTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Defaults;
	auto ExpectError = [&](const TCHAR* What, const FString& Dir, const TCHAR* Needle)
	{
		const FCamSimConfig Cfg = ResolvedConfigFor(Dir);
		TestEqual(FString::Printf(TEXT("%s: one error"), What), Cfg.Scene.ResolveErrors.Num(), 1);
		if (Cfg.Scene.ResolveErrors.Num() == 1)
		{
			TestTrue(FString::Printf(TEXT("%s: message mentions '%s' (%s)"), What, Needle, *Cfg.Scene.ResolveErrors[0]),
				Cfg.Scene.ResolveErrors[0].Contains(Needle));
		}
		TestEqual(FString::Printf(TEXT("%s: terrain untouched"), What),
			Cfg.CesiumBackend.Terrain.Source, Defaults.CesiumBackend.Terrain.Source);
	};

	ExpectError(TEXT("relative"), TEXT("pkg/pendleton"), TEXT("absolute"));
	// UE's NormalizePath expands '~' to the home directory, so "~/pkg" is an absolute path to a
	// (here missing) package; the error names the expanded path.
	ExpectError(TEXT("tilde"), TEXT("~/camsim-no-such-package"), TEXT("manifest.json"));
	TestFalse(TEXT("tilde: error shows the expanded path"),
		ResolvedConfigFor(TEXT("~/camsim-no-such-package")).Scene.ResolveErrors[0].Contains(TEXT("~")));
	ExpectError(TEXT("no manifest"), SceneTempDir(TEXT("no_manifest")), TEXT("manifest.json"));
	ExpectError(TEXT("bad json"), MakePackage(TEXT("bad_json"), true, true, true, TEXT("{not json")), TEXT("JSON"));
	ExpectError(TEXT("schema 2"), MakePackage(TEXT("schema2"), true, true, true, TEXT("{\"schema_version\": 2}")),
		TEXT("schema_version"));
	ExpectError(TEXT("no schema"), MakePackage(TEXT("noschema"), true, true, true, TEXT("{\"name\": \"x\"}")),
		TEXT("schema_version"));

	const FCamSimConfig Empty = ResolvedConfigFor(TEXT(""));
	TestEqual(TEXT("empty dir: no-op"), Empty.Scene.ResolveErrors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveOverrideWarningTest,
	"CamSim.Scene.Resolve.OverrideWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveOverrideWarningTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("override"), true, true, false);
	FCamSimConfig Cfg;
	Cfg.Scene.Dir = D;
	Cfg.CesiumBackend.Terrain.Source = TEXT("url");
	Cfg.CesiumBackend.Terrain.Url = TEXT("https://example.com/layer.json");
	CamSimScene::ResolvePackage(Cfg);
	TestEqual(TEXT("one warning (terrain)"), Cfg.Scene.ResolveWarnings.Num(), 1);
	if (Cfg.Scene.ResolveWarnings.Num() == 1)
	{
		TestTrue(TEXT("names the key"), Cfg.Scene.ResolveWarnings[0].Contains(TEXT("cesium.terrain")));
	}
	TestEqual(TEXT("package wins"), Cfg.CesiumBackend.Terrain.Url, CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneResolveAtLoadTest,
	"CamSim.Scene.Resolve.AtLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneResolveAtLoadTest::RunTest(const FString& Parameters)
{
	const FString D = MakePackage(TEXT("at load"), true, true, true);   // a space in the path
	{
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(FString::Printf(TEXT("scene:\n  dir: \"%s\"\n"), *D));
		TestEqual(TEXT("yaml: terrain url"), Cfg.CesiumBackend.Terrain.Url, CamSimScene::PathToFileUrl(D / TEXT("terrain/layer.json")));
		TestTrue(TEXT("yaml: space encoded"), Cfg.CesiumBackend.Terrain.Url.Contains(TEXT("at%20load")));
	}
	{
		ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_DIR"), TEXT("")); };
		FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_SCENE_DIR"), *D);
		const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("{}"));
		TestEqual(TEXT("env: imagery source"), Cfg.CesiumBackend.Imagery.Source, FString(TEXT("tms")));
		TestEqual(TEXT("env: package name"), Cfg.Scene.PackageName, FString(TEXT("testpkg")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneOfflineRejectsNetworkTest,
	"CamSim.Scene.Offline.RejectsNetworkSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneOfflineRejectsNetworkTest::RunTest(const FString& Parameters)
{
	using CamSimScene::ValidateSources;
	{
		FCamSimConfig Cfg;   // ion terrain + ion imagery
		TestEqual(TEXT("online default: no errors"), ValidateSources(Cfg).Num(), 0);
		Cfg.Scene.bOffline = true;
		const TArray<FString> E = ValidateSources(Cfg);
		TestEqual(TEXT("offline default: terrain + imagery"), E.Num(), 2);
		TestTrue(TEXT("names terrain"), E.Num() == 2 && E[0].Contains(TEXT("cesium.terrain.source")));
		TestTrue(TEXT("names imagery"), E.Num() == 2 && E[1].Contains(TEXT("cesium.imagery.source")));
	}
	const FString D = MakePackage(TEXT("offline_ok"), true, true, false);
	auto Offline = [&](TFunctionRef<void(FCamSimConfig&)> Edit)
	{
		FCamSimConfig Cfg = ResolvedConfigFor(D);
		Cfg.Scene.bOffline = true;
		Edit(Cfg);
		return ValidateSources(Cfg);
	};
	TestEqual(TEXT("package: ok"), Offline([](FCamSimConfig&) {}).Num(), 0);
	TestEqual(TEXT("flat + none: ok"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Source = TEXT("flat"); C.CesiumBackend.Imagery.Source = TEXT("none"); }).Num(), 0);
	TestEqual(TEXT("wms rejected"), Offline([](FCamSimConfig& C) { C.CesiumBackend.Imagery.Source = TEXT("wms"); }).Num(), 1);
	// FString == ignores case, in ApplyCesiumBackendConfig too: "TMS" is the tms source there and here.
	TestEqual(TEXT("upper-case TMS accepted, as the backend does"),
		Offline([](FCamSimConfig& C) { C.CesiumBackend.Imagery.Source = TEXT("TMS"); }).Num(), 0);
	TestEqual(TEXT("upper-case WMS still rejected"),
		Offline([](FCamSimConfig& C) { C.CesiumBackend.Imagery.Source = TEXT("WMS"); }).Num(), 1);
	TestEqual(TEXT("https terrain rejected"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Url = TEXT("https://example.com/layer.json"); }).Num(), 1);
	TestEqual(TEXT("file://host rejected"), Offline([](FCamSimConfig& C) {
		C.CesiumBackend.Terrain.Url = TEXT("file://host/layer.json"); }).Num(), 1);
	{
		const TArray<FString> E = Offline([&](FCamSimConfig& C) {
			C.CesiumBackend.Imagery.Url = CamSimScene::PathToFileUrl(D / TEXT("imagery/missing.xml")); });
		TestEqual(TEXT("missing file rejected"), E.Num(), 1);
		TestTrue(TEXT("says it does not exist"), E.Num() == 1 && E[0].Contains(TEXT("does not exist")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneTmsNeedsUrlTest,
	"CamSim.Scene.Offline.TmsNeedsUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneTmsNeedsUrlTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CesiumBackend.Imagery.Source = TEXT("tms");
	const TArray<FString> E = Cfg.Validate();
	TestTrue(TEXT("online tms without url is a Validate() error"),
		E.ContainsByPredicate([](const FString& S) { return S.Contains(TEXT("cesium.imagery.url")); }));
	return true;
}

// scene.offline exits on config errors only outside the editor: in PIE, _Exit would take the
// whole editor (and unsaved work) down.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneOfflineExitPolicyTest,
	"CamSim.Scene.Offline.ExitPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSceneOfflineExitPolicyTest::RunTest(const FString& Parameters)
{
	using CamSimScene::ShouldExitOnConfigErrors;
	FCamSimConfig Cfg;
	TestFalse(TEXT("online, game: run on"), ShouldExitOnConfigErrors(Cfg, /*bIsEditor=*/false));
	Cfg.Scene.bOffline = true;
	TestTrue(TEXT("offline, game (-game / packaged): exit"), ShouldExitOnConfigErrors(Cfg, false));
	TestFalse(TEXT("offline, editor (PIE): never exit the editor"), ShouldExitOnConfigErrors(Cfg, true));
	return true;
}
