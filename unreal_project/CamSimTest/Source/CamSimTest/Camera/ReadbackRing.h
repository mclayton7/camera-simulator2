// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Per-slot readback state:
 *   Idle     → InFlight  (game thread, Acquire)
 *   InFlight → Complete  (render thread, after the pixels were copied out)
 *   InFlight → Failed    (render thread: lock failed, bad format, timed out)
 *   Complete/Failed → Idle (game thread, Release after delivery)
 */
enum class EReadbackSlotState : uint8 { Idle, InFlight, Complete, Failed };

/**
 * FReadbackRing (ROADMAP 3A.1)
 *
 * Bookkeeping for up to NumSlots GPU readbacks in flight, so a capture can
 * start every tick instead of waiting for the previous readback (15 → 30 fps).
 * Frames are delivered strictly in capture order: a newer frame whose
 * readback lands first waits for the older ones, which keeps PTS and KLV
 * timestamps monotonic. A slot is reused only after it's released, so a
 * slow consumer backs up the ring and new frames are dropped (Acquire fails)
 * rather than stalling the game thread.
 *
 * Threading: Acquire/PeekFinished/Release and the order queue are game-thread
 * only. MarkComplete/MarkFailed are called from the render thread; the state
 * store is SeqCst so the slot's pixel data written before it is visible to the
 * game thread's SeqCst load in PeekFinished.
 */
class FReadbackRing
{
public:
	static constexpr int32 NumSlots = 3;

	FReadbackRing()
	{
		for (TAtomic<EReadbackSlotState>& S : State) { S.Store(EReadbackSlotState::Idle); }
	}

	/** True when the next slot is free (game thread). */
	bool CanAcquire() const
	{
		return State[NextSlot].Load(EMemoryOrder::SequentiallyConsistent) == EReadbackSlotState::Idle;
	}

	/** Claim the next slot for FrameIndex, or INDEX_NONE when the ring is full (game thread). */
	int32 Acquire(uint64 FrameIndex)
	{
		if (!CanAcquire()) return INDEX_NONE;
		const int32 Slot = NextSlot;
		FrameIndexOf[Slot] = FrameIndex;
		State[Slot].Store(EReadbackSlotState::InFlight, EMemoryOrder::SequentiallyConsistent);
		Order.Add(Slot);
		NextSlot = (NextSlot + 1) % NumSlots;
		return Slot;
	}

	/** Render thread: the slot's pixels are ready. */
	void MarkComplete(int32 Slot) { State[Slot].Store(EReadbackSlotState::Complete, EMemoryOrder::SequentiallyConsistent); }

	/** Render thread: the slot will never produce pixels. */
	void MarkFailed(int32 Slot)   { State[Slot].Store(EReadbackSlotState::Failed,   EMemoryOrder::SequentiallyConsistent); }

	/**
	 * The oldest captured frame, if its readback has finished (game thread).
	 * Doesn't consume: call Release(Slot) once it's delivered.
	 */
	bool PeekFinished(int32& OutSlot, bool& bOutFailed) const
	{
		if (Order.Num() == 0) return false;
		const int32 Slot = Order[0];
		const EReadbackSlotState S = State[Slot].Load(EMemoryOrder::SequentiallyConsistent);
		if (S != EReadbackSlotState::Complete && S != EReadbackSlotState::Failed) return false;
		OutSlot = Slot;
		bOutFailed = (S == EReadbackSlotState::Failed);
		return true;
	}

	/** Free the oldest slot after delivery (game thread). */
	void Release(int32 Slot)
	{
		check(Order.Num() > 0 && Order[0] == Slot);
		Order.RemoveAt(0, EAllowShrinking::No);
		State[Slot].Store(EReadbackSlotState::Idle, EMemoryOrder::SequentiallyConsistent);
	}

	EReadbackSlotState GetState(int32 Slot) const { return State[Slot].Load(EMemoryOrder::SequentiallyConsistent); }
	uint64 GetFrameIndex(int32 Slot) const { return FrameIndexOf[Slot]; }

	/** Captured frames not yet released, oldest first (game thread). */
	const TArray<int32>& GetOrder() const { return Order; }
	int32 NumInFlight() const { return Order.Num(); }
	int32 GetNextSlot() const { return NextSlot; }

private:
	TAtomic<EReadbackSlotState> State[NumSlots];
	uint64 FrameIndexOf[NumSlots] = {};
	TArray<int32> Order;   // game thread: slots in capture order
	int32 NextSlot = 0;    // game thread
};
