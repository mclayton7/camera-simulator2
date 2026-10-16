// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"

struct FEntityAnnotationData;

/**
 * Semantic classes of the ground-truth class image (ROADMAP 4D, docs/ground-truth.md). The ids are stable: datasets
 * from different runs mean the same thing by them. Terrain classes come from the land-cover code under a pixel
 * (BuildCodeLut), entity classes from entity_types.<id>.semantic_class.
 */
namespace CamSimSemantic
{
	enum EClass : uint8
	{
		Unlabeled = 0, Sky = 1, Water = 2, Road = 3, Rail = 4, Building = 5, BuiltUp = 6, Tree = 7, LowVegetation = 8,
		Bare = 9, SnowIce = 10,
		Vehicle = 20, Vessel = 21, Aircraft = 22, Person = 23, Marker = 24, OtherEntity = 25,
	};

	/** "road", ...; "" for an id that isn't a class. */
	CAMSIMTEST_API const TCHAR* Name(uint8 Class);
	/** Every class id, ascending. */
	CAMSIMTEST_API TArray<uint8> AllClasses();
	/** Land-cover code (WorldCover 10..100, features 200..230) -> class; any other code -> Unlabeled. */
	CAMSIMTEST_API TStaticArray<uint8, 256> BuildCodeLut();
	/** entity_types semantic_class name (vehicle, vessel, aircraft, person, marker, other) -> class. */
	CAMSIMTEST_API bool ParseEntityClass(const FString& InName, uint8& OutClass);
	/** Without semantic_class: "character" -> Person, else OtherEntity. */
	CAMSIMTEST_API uint8 DefaultEntityClass(const FString& EntityCategory);
	/** Per stencil value, the class of the entity holding it in this frame's snapshot; 0 = none (terrain decides). */
	CAMSIMTEST_API TStaticArray<uint8, 256> BuildStencilClassTable(TConstArrayView<FEntityAnnotationData> Entities,
		TFunctionRef<uint8(uint16 EntityType)> ClassOfType);
}
