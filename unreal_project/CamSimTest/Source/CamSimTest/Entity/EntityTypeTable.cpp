// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityTypeTable.h"
#include "CamSimTest.h"

void FEntityTypeTable::LoadFromConfig(const TSharedPtr<FJsonObject>& Root)
{
	if (!Root.IsValid()) return;

	const TSharedPtr<FJsonObject>* TypesObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("entity_types"), TypesObj) || !TypesObj) return;

	TypeMap.Empty();

	for (const auto& Pair : (*TypesObj)->Values)
	{
		// Key is the type ID string (e.g. "1001")
		uint16 TypeId = static_cast<uint16>(FCString::Atoi(*Pair.Key));

		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!Pair.Value->TryGetObject(EntryObj) || !EntryObj) continue;

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

		TypeMap.Add(TypeId, Entry);

		UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: type %u -> %s (%s)"),
			TypeId, *Entry.AssetPath, Entry.bSkeletal ? TEXT("skeletal") : TEXT("static"));
	}

	UE_LOG(LogCamSim, Log, TEXT("EntityTypeTable: loaded %d entries"), TypeMap.Num());
}

const FEntityTypeEntry* FEntityTypeTable::FindEntry(uint16 TypeId) const
{
	return TypeMap.Find(TypeId);
}
