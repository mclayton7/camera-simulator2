// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Hosts/DisCommands.h"

// CamSim.Thermal.Entity.DisAppearance.*: DIS Entity State appearance -> canonical component commands (ROADMAP 4C).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisAppearanceDecodeTest, "CamSim.Thermal.Entity.DisAppearance.Decode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDisAppearanceDecodeTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Dis;
	const uint32 Power = 1u << 22, Flaming = 1u << 15;
	auto Dmg = [](uint32 D) { return (D & 3u) << 3; };
	const TOptional<FPlatformAppearance> A = DecodePlatformAppearance(1, 1, Power | Dmg(3) | Flaming);
	if (!TestTrue(TEXT("land decoded"), A.IsSet())) return false;
	TestTrue(TEXT("power"), A->bPowerPlant);
	TestEqual(TEXT("destroyed -> 2"), A->Damage, static_cast<uint8>(2));
	TestTrue(TEXT("flaming"), A->bFlaming);
	TestEqual(TEXT("slight -> 1"), DecodePlatformAppearance(1, 3, Dmg(1))->Damage, static_cast<uint8>(1));
	TestEqual(TEXT("moderate -> 1"), DecodePlatformAppearance(1, 2, Dmg(2))->Damage, static_cast<uint8>(1));
	const TOptional<FPlatformAppearance> Z = DecodePlatformAppearance(1, 1, 0u);
	TestTrue(TEXT("none -> all off"), Z.IsSet() && Z->Damage == 0 && !Z->bPowerPlant && !Z->bFlaming);
	TestFalse(TEXT("munition not decoded"), DecodePlatformAppearance(2, 1, Power).IsSet());
	TestFalse(TEXT("subsurface not decoded"), DecodePlatformAppearance(1, 4, Power).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDisAppearanceCommandsTest, "CamSim.Thermal.Entity.DisAppearance.OnChangeOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDisAppearanceCommandsTest::RunTest(const FString& Parameters)
{
	using namespace CamSim::Dis;
	const FEntityKey Key(EHostSource::Dis, 42);
	TArray<FComponentCommand> Out;
	const FPlatformAppearance Zero;
	AppearanceCommands(Key, {}, Zero, Out);
	TestEqual(TEXT("first PDU at defaults: nothing"), Out.Num(), 0);
	FPlatformAppearance On; On.bPowerPlant = true;
	AppearanceCommands(Key, {}, On, Out);
	if (!TestEqual(TEXT("first PDU engine on: one command"), Out.Num(), 1)) return false;
	TestEqual(TEXT("comp 11"), Out[0].ComponentId, static_cast<uint16>(11));
	TestEqual(TEXT("state 1"), Out[0].State, static_cast<uint8>(1));
	TestEqual(TEXT("class 0"), Out[0].ComponentClass, static_cast<uint8>(0));
	TestTrue(TEXT("key"), Out[0].Key == Key);
	Out.Reset();
	AppearanceCommands(Key, On, On, Out);
	TestEqual(TEXT("heartbeat: nothing"), Out.Num(), 0);
	FPlatformAppearance Dead = On; Dead.bPowerPlant = false; Dead.Damage = 2; Dead.bFlaming = true;
	AppearanceCommands(Key, On, Dead, Out);
	TestEqual(TEXT("three changes"), Out.Num(), 3);
	TMap<uint16, uint8> ById;
	for (const FComponentCommand& C : Out) ById.Add(C.ComponentId, C.State);
	TestTrue(TEXT("10 = 2"), ById.Contains(10) && ById[10] == 2);
	TestTrue(TEXT("11 = 0"), ById.Contains(11) && ById[11] == 0);
	TestTrue(TEXT("12 = 1"), ById.Contains(12) && ById[12] == 1);
	return true;
}
