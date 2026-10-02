// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityTypeTable.h"
#include "Entity/EntityMeshLoader.h"
#include "Entity/EntityPaths.h"
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
		const FString AbsPath = CamSimEntityPaths::Resolve(Path);
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

/** A finite float scalar (yaml .nan/.inf and non-numbers rejected). */
static bool ParseFiniteScalar(ryml::ConstNodeRef N, float& Out)
{
	if (!N.has_val()) return false;
	const FString S = RymlToFString(N.val()).TrimStartAndEnd();
	if (S.IsEmpty() || !S.IsNumeric()) return false;
	Out = FCString::Atof(*S);
	return FMath::IsFinite(Out);
}

/** One thermal_parts entry (ROADMAP 4C). False with a reason for unknown kind/shape, a missing or non-finite vector. */
static bool ParseThermalPart(ryml::ConstNodeRef N, FEntityThermalPartSpec& Out, FString& OutWhy)
{
	if (!N.is_map()) { OutWhy = TEXT("not a map"); return false; }
	FString Kind, Shape;
	YamlString(N, "kind", Kind);
	YamlString(N, "shape", Shape);
	if (!CamSimEntityThermal::ParseKind(Kind, Out.Kind))   { OutWhy = FString::Printf(TEXT("unknown kind '%s'"), *Kind); return false; }
	if (!CamSimEntityThermal::ParseShape(Shape, Out.Shape)) { OutWhy = FString::Printf(TEXT("unknown shape '%s'"), *Shape); return false; }
	auto Vec3 = [&N](c4::csubstr Key, FVector3f& V) -> bool
	{
		if (!N.has_child(Key)) return false;
		ryml::ConstNodeRef A = N[Key];
		if (!A.is_seq() || A.num_children() != 3) return false;
		float C[3];
		for (int32 I = 0; I < 3; ++I)
		{
			if (!ParseFiniteScalar(A[I], C[I])) return false;
		}
		V = FVector3f(C[0], C[1], C[2]);
		return true;
	};
	if (!Vec3("centre_m", Out.CentreM)) { OutWhy = TEXT("centre_m must be 3 finite numbers"); return false; }
	if (!Vec3("half_m", Out.HalfM))     { OutWhy = TEXT("half_m must be 3 finite numbers"); return false; }
	if (N.has_child("falloff_m") && !ParseFiniteScalar(N["falloff_m"], Out.FalloffM)) { OutWhy = TEXT("falloff_m not a finite number"); return false; }
	const float Min = FEntityThermalPartSpec::MinExtentM;
	Out.HalfM = FVector3f(FMath::Max(FMath::Abs(Out.HalfM.X), Min), FMath::Max(FMath::Abs(Out.HalfM.Y), Min), FMath::Max(FMath::Abs(Out.HalfM.Z), Min));
	Out.FalloffM = FMath::Max(Out.FalloffM, Min);
	float V = 0.0f;
	if (N.has_child("delta_k") && ParseFiniteScalar(N["delta_k"], V))     Out.DeltaK  = FMath::Clamp(V, -50.0f, 500.0f);
	if (N.has_child("temp_k") && ParseFiniteScalar(N["temp_k"], V))       Out.TempK   = FMath::Clamp(V, 150.0f, 1000.0f);
	if (N.has_child("k_per_mps") && ParseFiniteScalar(N["k_per_mps"], V)) Out.KPerMps = FMath::Clamp(V, 0.0f, 50.0f);
	if (N.has_child("max_k") && ParseFiniteScalar(N["max_k"], V))         Out.MaxK    = FMath::Clamp(V, 0.0f, 500.0f);
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

	LoadFromYamlString(YamlContent);
}

void FEntityTypeTable::LoadFromYamlString(const FString& YamlContent)
{
	FTCHARToUTF8 Utf8(*YamlContent);
	c4::csubstr Src(Utf8.Get(), Utf8.Length());

	ryml::Tree Tree;
	try
	{
		Tree = ryml::parse_in_arena(Src);
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: failed to parse entity_types YAML: %hs"), Ex.what());
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
		float ZOffsetM = 0.0f;
		YamlFloat(EntryNode, "z_offset_m", ZOffsetM);
		Entry.ModelZOffsetCm = ZOffsetM * 100.0f;

		// Phase 19C — Vessel motion geometry
		float HalfLengthM = 0.0f;
		float HalfBeamM   = 0.0f;
		YamlFloat(EntryNode, "half_length_m", HalfLengthM);
		YamlFloat(EntryNode, "half_beam_m",   HalfBeamM);
		Entry.HalfLengthCm = HalfLengthM * 100.0f;
		Entry.HalfBeamCm   = HalfBeamM   * 100.0f;

		// Phase 22D — Character animation
		YamlBool  (EntryNode, "animated",       Entry.bAnimated);
		YamlString(EntryNode, "anim_blueprint", Entry.AnimBlueprintPath);
		YamlString(EntryNode, "entity_category", Entry.EntityCategory);

		// ROADMAP 4A — thermal class + offset (the class name is resolved by FThermalFrameBuilder, which warns once on an unknown name)
		YamlString(EntryNode, "thermal_material", Entry.ThermalMaterial);
		float OffsetK = 0.0f;
		if (YamlFloat(EntryNode, "thermal_offset_k", OffsetK))
		{
			if (FMath::IsFinite(OffsetK) && OffsetK >= -50.0f && OffsetK <= 500.0f)
			{
				Entry.ThermalOffsetK = OffsetK;
			}
			else
			{
				UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_offset_k %.2f out of range [-50, 500]; ignored"), TypeId, OffsetK);
			}
		}

		// ROADMAP 4C — thermal_parts: hot-spot volumes. Invalid parts are skipped with a warning; extras beyond MaxParts dropped.
		if (EntryNode.has_child("thermal_parts"))
		{
			int32 Index = 0;
			for (ryml::ConstNodeRef PN : EntryNode["thermal_parts"].children())
			{
				FEntityThermalPartSpec Spec;
				FString Why;
				if (!ParseThermalPart(PN, Spec, Why))
				{
					UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_parts[%d] skipped: %s"), TypeId, Index, *Why);
				}
				else if (Entry.ThermalParts.Num() >= FEntityThermalSettings::MaxParts)
				{
					UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_parts[%d] dropped: at most %d parts"),
						TypeId, Index, FEntityThermalSettings::MaxParts);
				}
				else
				{
					Entry.ThermalParts.Add(Spec);
				}
				++Index;
			}
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

void FEntityTypeTable::SetCachedStaticMesh(uint16 TypeId, UStaticMesh* Mesh) const
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

void FEntityTypeTable::SetCachedSkeletalMesh(uint16 TypeId, USkeletalMesh* Mesh) const
{
	SkeletalMeshCache.Add(TypeId, Mesh);
}

int32 FEntityTypeTable::PreloadGltfMeshes()
{
	TMap<uint16, TStrongObjectPtr<UObject>> Kept;
	for (const TPair<uint16, FEntityTypeEntry>& Pair : TypeMap)
	{
		const FEntityTypeEntry& E = Pair.Value;
		if (E.bAnimated || !CamSimMeshLoader::IsGltfPath(E.AssetPath)) continue;

		UObject* Mesh = E.bSkeletal ? static_cast<UObject*>(GetCachedSkeletalMesh(Pair.Key))
		                            : static_cast<UObject*>(GetCachedStaticMesh(Pair.Key));
		if (!Mesh)
		{
			const double T0 = FPlatformTime::Seconds();
			if (E.bSkeletal)
			{
				USkeletalMesh* S = CamSimMeshLoader::LoadSkeletalMesh(E.AssetPath);
				if (S) SetCachedSkeletalMesh(Pair.Key, S);
				Mesh = S;
			}
			else
			{
				UStaticMesh* S = CamSimMeshLoader::LoadStaticMesh(E.AssetPath);
				if (S) SetCachedStaticMesh(Pair.Key, S);
				Mesh = S;
			}
			UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: preloaded type %u '%s' in %.0f ms%s"),
				Pair.Key, *E.AssetPath, (FPlatformTime::Seconds() - T0) * 1000.0, Mesh ? TEXT("") : TEXT(" — FAILED"));
		}
		if (Mesh) Kept.Add(Pair.Key, TStrongObjectPtr<UObject>(Mesh));
	}
	PreloadedMeshes = MoveTemp(Kept);
	return PreloadedMeshes.Num();
}

TArray<UStaticMesh*> FEntityTypeTable::GetPreloadedStaticMeshes() const
{
	TArray<UStaticMesh*> Out;
	for (const TPair<uint16, TStrongObjectPtr<UObject>>& Pair : PreloadedMeshes)
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Pair.Value.Get())) Out.Add(Mesh);
	}
	return Out;
}
