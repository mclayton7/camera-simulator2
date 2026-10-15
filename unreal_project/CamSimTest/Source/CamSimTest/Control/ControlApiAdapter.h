// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sim/Commands.h"

class ISimCommandSink;
namespace CamSimControl { struct FEntityRequest; }

/**
 * FControlApiAdapter
 *
 * The control API's host adapter (ROADMAP 2.4): API entities live in their own namespace (EHostSource::ControlApi,
 * "api:N"). It keeps each entity's last state and re-submits it every tick, as a host at frame rate would, so a
 * ground-clamped entity is clamped again once its tiles load. Removals and component changes go out once. Game
 * thread only (the HTTP handlers run on the game thread).
 */
class CAMSIMTEST_API FControlApiAdapter
{
public:
	/** Merge a parsed request into the entity's state. False (OutError set) for a new entity without type_id or position. */
	bool PutEntity(const CamSimControl::FEntityRequest& Req, FString& OutError);
	/** False if the entity is unknown. */
	bool RemoveEntity(uint16 Id);
	void RemoveAll();

	/** Submit every entity's state, then pending removals and component changes. FCamSimEntityManager's ordered pass. */
	void Poll(ISimCommandSink& Sink);

	TArray<uint16> GetIds() const;
	bool HasEntity(uint16 Id) const { return Entities.Contains(Id); }
	/** The type an entity was put with (0 if unknown). */
	uint16 GetTypeId(uint16 Id) const;

	/** A camera or entity command was applied in this GFrameCounter frame (the capture gate waits for a quiet frame). */
	void NoteCommand(uint64 FrameCounter) { LastCommandFrame = FrameCounter; bAnyCommand = true; ++CommandSerial; }
	/** Bumped by every command: a change since the last look means "a command was applied". */
	uint64 GetCommandSerial() const { return CommandSerial; }
	bool WasCommandedThisFrame(uint64 FrameCounter) const { return bAnyCommand && LastCommandFrame == FrameCounter; }

	static FEntityKey KeyFor(uint16 Id) { return FEntityKey(EHostSource::ControlApi, Id); }

private:
	struct FState
	{
		uint16 TypeId = 0;
		CamSimFrames::FGeoPose Pose;
		ESurfaceMode Surface = ESurfaceMode::None;
		bool bHidden = false;
	};
	TMap<uint16, FState> Entities;
	TArray<uint16> PendingRemovals;
	TArray<FComponentCommand> PendingComponents;
	uint64 LastCommandFrame = 0;
	bool bAnyCommand = false;
	uint64 CommandSerial = 0;
};
