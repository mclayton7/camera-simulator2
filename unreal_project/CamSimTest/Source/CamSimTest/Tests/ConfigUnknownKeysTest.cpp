// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Keys no setting reads are reported, so a typo doesn't silently fall back to
// the default.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigUnknownKeysTest,
	"CamSim.Config.UnknownKeysReported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConfigUnknownKeysTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* Key : { TEXT("'cigi_prot'"), TEXT("'operational.health_http_prot'"), TEXT("'no_such_section'") })
	{
		AddExpectedMessage(FString::Printf(TEXT("Config: unknown key %s"), Key), EAutomationExpectedErrorFlags::Contains, 1);
	}

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"cigi_port: 9999\n"
		"cigi_prot: 1234\n"                    // typo at the top level
		"operational:\n"
		"  health_http_enabled: false\n"
		"  health_http_prot: 9090\n"           // typo in a section
		"dis:\n"
		"  entity_type_map:\n"
		"    \"1:2:225:1:0:0:0\": 1001\n"      // data keys, not settings
		"no_such_section:\n"
		"  anything: 1\n"));                   // reported once, not per child

	TestEqual(TEXT("known key applied"), Cfg.CigiPort, 9999);
	TestFalse(TEXT("known nested key applied"), Cfg.Operational.bHealthHttpEnabled);
	TestEqual(TEXT("unknown keys"), Cfg.UnknownYamlKeys,
		TArray<FString>{ TEXT("cigi_prot"), TEXT("operational.health_http_prot"), TEXT("no_such_section") });
	return true;
}

// The shipped config must only contain settings CamSim actually reads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigCanonicalKeysTest,
	"CamSim.Config.CanonicalConfigHasNoUnknownKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConfigCanonicalKeysTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml")));
	FString Yaml;
	if (!TestTrue(FString::Printf(TEXT("read %s"), *Path), FFileHelper::LoadFileToString(Yaml, *Path)))
	{
		return false;
	}
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml, Path);
	for (const FString& Key : Cfg.UnknownYamlKeys)
	{
		AddError(FString::Printf(TEXT("deploy/camsim_config.yaml: '%s' is not read by FCamSimConfig"), *Key));
	}
	return true;
}
