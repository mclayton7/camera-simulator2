// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/SemanticClasses.h"
#include "GroundTruth/AnnotationTypes.h"

namespace CamSimSemantic
{
namespace
{
	struct FNamed { uint8 Class; const TCHAR* Name; };
	const FNamed GClasses[] = {
		{ Unlabeled, TEXT("unlabeled") }, { Sky, TEXT("sky") }, { Water, TEXT("water") }, { Road, TEXT("road") },
		{ Rail, TEXT("rail") }, { Building, TEXT("building") }, { BuiltUp, TEXT("built_up") }, { Tree, TEXT("tree") },
		{ LowVegetation, TEXT("low_vegetation") }, { Bare, TEXT("bare") }, { SnowIce, TEXT("snow_ice") },
		{ Vehicle, TEXT("vehicle") }, { Vessel, TEXT("vessel") }, { Aircraft, TEXT("aircraft") }, { Person, TEXT("person") },
		{ Marker, TEXT("marker") }, { OtherEntity, TEXT("other_entity") },
	};
}

const TCHAR* Name(uint8 Class)
{
	for (const FNamed& N : GClasses) if (N.Class == Class) return N.Name;
	return TEXT("");
}

TArray<uint8> AllClasses()
{
	TArray<uint8> Out;
	for (const FNamed& N : GClasses) Out.Add(N.Class);
	return Out;
}

TStaticArray<uint8, 256> BuildCodeLut()
{
	TStaticArray<uint8, 256> L(InPlace, static_cast<uint8>(Unlabeled));
	L[10] = Tree;
	for (const int32 C : { 20, 30, 40, 90, 95, 100 }) L[C] = LowVegetation;
	L[50] = BuiltUp;
	L[60] = Bare;
	L[70] = SnowIce;
	L[80] = Water;
	L[200] = Road;
	L[201] = Road;
	L[210] = Rail;
	L[220] = Water;
	L[230] = Building;
	return L;
}

bool ParseEntityClass(const FString& InName, uint8& OutClass)
{
	const TPair<const TCHAR*, uint8> Names[] = { { TEXT("vehicle"), Vehicle }, { TEXT("vessel"), Vessel },
		{ TEXT("aircraft"), Aircraft }, { TEXT("person"), Person }, { TEXT("marker"), Marker }, { TEXT("other"), OtherEntity } };
	for (const auto& N : Names)
	{
		if (InName == N.Key) { OutClass = N.Value; return true; }
	}
	return false;
}

uint8 DefaultEntityClass(const FString& EntityCategory)
{
	return EntityCategory == TEXT("character") ? static_cast<uint8>(Person) : static_cast<uint8>(OtherEntity);
}

TStaticArray<uint8, 256> BuildStencilClassTable(TConstArrayView<FEntityAnnotationData> Entities,
	TFunctionRef<uint8(uint16 EntityType)> ClassOfType)
{
	TStaticArray<uint8, 256> T(InPlace, static_cast<uint8>(0));
	for (const FEntityAnnotationData& E : Entities)
	{
		if (E.StencilValue != 0) T[E.StencilValue] = ClassOfType(E.EntityType);
	}
	return T;
}
}
