// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityTypeTable.h"
#include "CamSimTest.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

namespace
{
bool IsGltfPath(const FString& Path)
{
	return Path.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase) ||
	       Path.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase);
}

FString ResolveEntityAssetPath(const FString& RelPath)
{
	const FString Base = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../entities/"));
	return FPaths::ConvertRelativePathToFull(Base + RelPath);
}

bool ValidateMeshAssetPath(const FString& Path, bool bSkeletal, uint16 TypeId, const TCHAR* VariantLabel)
{
	if (Path.IsEmpty())
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("EntityTypeTable: type %u missing '%s' asset path"), TypeId, VariantLabel);
		return false;
	}

	if (IsGltfPath(Path))
	{
		const FString AbsPath = ResolveEntityAssetPath(Path);
		if (!FPaths::FileExists(AbsPath))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityTypeTable: type %u '%s' glTF file not found: %s"),
				TypeId, VariantLabel, *AbsPath);
			return false;
		}
		return true;
	}

	if (!Path.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("EntityTypeTable: type %u '%s' path must be '/Game/...' or .gltf/.glb: %s"),
			TypeId, VariantLabel, *Path);
		return false;
	}

	UObject* Loaded = FSoftObjectPath(Path).TryLoad();
	if (!Loaded)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("EntityTypeTable: type %u '%s' asset could not be loaded: %s"),
			TypeId, VariantLabel, *Path);
		return false;
	}

	if (bSkeletal)
	{
		if (!Cast<USkeletalMesh>(Loaded))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityTypeTable: type %u '%s' expected skeletal mesh but got %s (%s)"),
				TypeId, VariantLabel, *Loaded->GetClass()->GetName(), *Path);
			return false;
		}
	}
	else
	{
		if (!Cast<UStaticMesh>(Loaded))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityTypeTable: type %u '%s' expected static mesh but got %s (%s)"),
				TypeId, VariantLabel, *Loaded->GetClass()->GetName(), *Path);
			return false;
		}
	}

	return true;
}
}

void FEntityTypeTable::LoadFromConfig(const TSharedPtr<FJsonObject>& Root)
{
	if (!Root.IsValid()) return;

	const TSharedPtr<FJsonObject>* TypesObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("entity_types"), TypesObj) || !TypesObj) return;

	TypeMap.Empty();

	int32 SkippedEntries = 0;
	for (const auto& Pair : (*TypesObj)->Values)
	{
		// Key is the type ID string (e.g. "1001")
		const int32 ParsedTypeId = FCString::Atoi(*Pair.Key);
		if (ParsedTypeId <= 0 || ParsedTypeId > 65535)
		{
			++SkippedEntries;
			UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: invalid type key '%s' (must be 1..65535)"), *Pair.Key);
			continue;
		}
		const uint16 TypeId = static_cast<uint16>(ParsedTypeId);

		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!Pair.Value->TryGetObject(EntryObj) || !EntryObj)
		{
			++SkippedEntries;
			UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u entry is not a JSON object"), TypeId);
			continue;
		}

		FEntityTypeEntry Entry;
		(*EntryObj)->TryGetStringField(TEXT("mesh"),            Entry.AssetPath);
		(*EntryObj)->TryGetStringField(TEXT("mesh_damaged"),    Entry.DamagedAssetPath);
		(*EntryObj)->TryGetStringField(TEXT("mesh_destroyed"),  Entry.DestroyedAssetPath);
		(*EntryObj)->TryGetBoolField(TEXT("skeletal"),          Entry.bSkeletal);

		// Optional model-space correction for glTF assets
		double ScaleVal = 1.0;
		if ((*EntryObj)->TryGetNumberField(TEXT("scale"), ScaleVal))
		{
			Entry.ModelScale = static_cast<float>(ScaleVal);
		}
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if ((*EntryObj)->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
		{
			double P = 0.0, Y = 0.0, R = 0.0;
			(*RotObj)->TryGetNumberField(TEXT("pitch"), P);
			(*RotObj)->TryGetNumberField(TEXT("yaw"),   Y);
			(*RotObj)->TryGetNumberField(TEXT("roll"),  R);
			Entry.ModelRotation = FRotator(P, Y, R);
		}

		if (!ValidateMeshAssetPath(Entry.AssetPath, Entry.bSkeletal, TypeId, TEXT("mesh")))
		{
			++SkippedEntries;
			continue;
		}
		if (!Entry.DamagedAssetPath.IsEmpty() &&
			!ValidateMeshAssetPath(Entry.DamagedAssetPath, Entry.bSkeletal, TypeId, TEXT("mesh_damaged")))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityTypeTable: type %u ignoring invalid mesh_damaged '%s'"),
				TypeId, *Entry.DamagedAssetPath);
			Entry.DamagedAssetPath.Empty();
		}
		if (!Entry.DestroyedAssetPath.IsEmpty() &&
			!ValidateMeshAssetPath(Entry.DestroyedAssetPath, Entry.bSkeletal, TypeId, TEXT("mesh_destroyed")))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("EntityTypeTable: type %u ignoring invalid mesh_destroyed '%s'"),
				TypeId, *Entry.DestroyedAssetPath);
			Entry.DestroyedAssetPath.Empty();
		}

		TypeMap.Add(TypeId, MoveTemp(Entry));

		UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: type %u -> %s (%s)"),
			TypeId, *TypeMap[TypeId].AssetPath, TypeMap[TypeId].bSkeletal ? TEXT("skeletal") : TEXT("static"));
	}

	UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: loaded %d entries (%d skipped by preflight)"),
		TypeMap.Num(), SkippedEntries);
}

const FEntityTypeEntry* FEntityTypeTable::FindEntry(uint16 TypeId) const
{
	return TypeMap.Find(TypeId);
}
