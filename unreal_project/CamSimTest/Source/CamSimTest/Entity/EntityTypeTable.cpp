// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityTypeTable.h"
#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wundef"
#endif
// UE5 CoreDefines.h defines DEFAULTS as 0, which collides with a ryml enum member.
#pragma push_macro("DEFAULTS")
#undef DEFAULTS
#include "Config/ryml/ryml_all.hpp"
#pragma pop_macro("DEFAULTS")
#ifdef __clang__
#pragma clang diagnostic pop
#endif

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

// Thin ryml helpers (duplicated from CamSimConfig.cpp to avoid leaking ryml into headers)
static FString RymlToFString(c4::csubstr S)
{
	return FString(static_cast<int32>(S.len), UTF8_TO_TCHAR(S.str));
}

static bool YamlString(ryml::ConstNodeRef Node, c4::csubstr Key, FString& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	Out = RymlToFString(Child.val());
	return true;
}

static bool YamlBool(ryml::ConstNodeRef Node, c4::csubstr Key, bool& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	c4::csubstr Val = Child.val();
	Out = (Val == "true" || Val == "True" || Val == "TRUE" ||
	       Val == "yes"  || Val == "Yes"  || Val == "YES"  ||
	       Val == "on"   || Val == "On"   || Val == "ON"   ||
	       Val == "1");
	return true;
}

static bool YamlFloat(ryml::ConstNodeRef Node, c4::csubstr Key, float& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atof(*Str);
	return true;
}

static bool YamlDouble(ryml::ConstNodeRef Node, c4::csubstr Key, double& Out)
{
	if (!Node.has_child(Key)) return false;
	ryml::ConstNodeRef Child = Node[Key];
	if (!Child.has_val()) return false;
	FString Str = RymlToFString(Child.val());
	Out = FCString::Atod(*Str);
	return true;
}
}

void FEntityTypeTable::LoadFromConfig()
{
	const FString YamlPath = FCamSimConfig::GetConfigFilePath();

	FString YamlContent;
	if (!FFileHelper::LoadFileToString(YamlContent, *YamlPath))
	{
		UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: config file not found: %s"), *YamlPath);
		return;
	}

	FTCHARToUTF8 Utf8(*YamlContent);
	c4::csubstr Src(Utf8.Get(), Utf8.Length());

	ryml::Tree Tree;
	try
	{
		Tree = ryml::parse_in_arena(Src);
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: failed to parse %s: %hs"), *YamlPath, Ex.what());
		return;
	}

	ryml::ConstNodeRef Root = Tree.rootref();
	if (!Root.has_child("entity_types")) return;

	ryml::ConstNodeRef TypesNode = Root["entity_types"];
	if (!TypesNode.is_map()) return;

	TypeMap.Empty();

	int32 SkippedEntries = 0;
	for (ryml::ConstNodeRef EntryNode : TypesNode)
	{
		// Key is the type ID string (e.g. "1001")
		FString KeyStr = RymlToFString(EntryNode.key());
		const int32 ParsedTypeId = FCString::Atoi(*KeyStr);
		if (ParsedTypeId <= 0 || ParsedTypeId > 65535)
		{
			++SkippedEntries;
			UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: invalid type key '%s' (must be 1..65535)"), *KeyStr);
			continue;
		}
		const uint16 TypeId = static_cast<uint16>(ParsedTypeId);

		if (!EntryNode.is_map())
		{
			++SkippedEntries;
			UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u entry is not a YAML map"), TypeId);
			continue;
		}

		FEntityTypeEntry Entry;
		YamlString(EntryNode, "mesh",            Entry.AssetPath);
		YamlString(EntryNode, "mesh_damaged",    Entry.DamagedAssetPath);
		YamlString(EntryNode, "mesh_destroyed",  Entry.DestroyedAssetPath);
		YamlBool  (EntryNode, "skeletal",        Entry.bSkeletal);
		// Optional class label for ML annotation (Phase 17); falls back to "type_NNNN"
		if (!YamlString(EntryNode, "class_name", Entry.ClassName) || Entry.ClassName.IsEmpty())
			Entry.ClassName = FString::Printf(TEXT("type_%u"), TypeId);

		// Optional model-space correction for glTF assets
		YamlFloat(EntryNode, "scale", Entry.ModelScale);

		if (EntryNode.has_child("rotation"))
		{
			ryml::ConstNodeRef RotNode = EntryNode["rotation"];
			double P = 0.0, Y = 0.0, R = 0.0;
			YamlDouble(RotNode, "pitch", P);
			YamlDouble(RotNode, "yaw",   Y);
			YamlDouble(RotNode, "roll",  R);
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

UStaticMesh* FEntityTypeTable::GetCachedStaticMesh(uint16 TypeId) const
{
	if (const TWeakObjectPtr<UStaticMesh>* Ptr = StaticMeshCache.Find(TypeId))
	{
		return Ptr->Get();
	}
	return nullptr;
}

void FEntityTypeTable::SetCachedStaticMesh(uint16 TypeId, UStaticMesh* Mesh)
{
	StaticMeshCache.Add(TypeId, Mesh);
}

USkeletalMesh* FEntityTypeTable::GetCachedSkeletalMesh(uint16 TypeId) const
{
	if (const TWeakObjectPtr<USkeletalMesh>* Ptr = SkeletalMeshCache.Find(TypeId))
	{
		return Ptr->Get();
	}
	return nullptr;
}

void FEntityTypeTable::SetCachedSkeletalMesh(uint16 TypeId, USkeletalMesh* Mesh)
{
	SkeletalMeshCache.Add(TypeId, Mesh);
}
