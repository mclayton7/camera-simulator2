// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SpscQueue.h"
#include "Templates/Atomic.h"

/**
 * TBoundedSpscQueue — thin wrapper over UE's lock-free single-producer /
 * single-consumer queue that caps memory growth and tracks drops.
 *
 * Back-pressure strategy is drop-newest: when the queue is full, Enqueue()
 * returns false and bumps DropCount. Drop-oldest would require the producer
 * to also dequeue, which violates the SPSC invariant that makes the inner
 * queue lock-free. For network receivers, drop-newest is usually preferable
 * anyway — it keeps the oldest packet ordering intact, which helps
 * reconstruction logic (e.g. dead-reckoning) that smooths over gaps.
 *
 * Thread safety: same as TSpscQueue<T>. Enqueue() is producer-thread only,
 * Dequeue() is consumer-thread only. Num() / GetDropCount() are lock-free
 * and safe to call from any thread; they read relaxed atomics so the value
 * may lag real state slightly.
 */
template <typename T>
class TBoundedSpscQueue
{
public:
	static constexpr int32 DefaultCapacity = 1024;

	explicit TBoundedSpscQueue(int32 InCapacity = DefaultCapacity)
		: Capacity(FMath::Max(InCapacity, 1))
	{
	}

	TBoundedSpscQueue(const TBoundedSpscQueue&) = delete;
	TBoundedSpscQueue& operator=(const TBoundedSpscQueue&) = delete;

	/**
	 * Enqueue one item from the producer thread.
	 * Returns false (and bumps DropCount) when the queue has reached Capacity.
	 */
	bool Enqueue(T Item)
	{
		if (Count.Load(EMemoryOrder::Relaxed) >= Capacity)
		{
			DropCount.Store(
				DropCount.Load(EMemoryOrder::Relaxed) + 1,
				EMemoryOrder::Relaxed);
			return false;
		}
		Inner.Enqueue(MoveTemp(Item));
		Count.Store(Count.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::Relaxed);
		return true;
	}

	/** Dequeue one item from the consumer thread. Returns false if empty. */
	bool Dequeue(T& Out)
	{
		if (Inner.Dequeue(Out))
		{
			Count.Store(Count.Load(EMemoryOrder::Relaxed) - 1, EMemoryOrder::Relaxed);
			return true;
		}
		return false;
	}

	/** Approximate queue depth (can lag by one op; never negative under correct SPSC use). */
	int32 Num() const { return FMath::Max(0, Count.Load(EMemoryOrder::Relaxed)); }

	int32 GetCapacity()  const { return Capacity; }
	bool  IsEmpty()      const { return Inner.IsEmpty(); }
	uint64 GetDropCount() const { return DropCount.Load(EMemoryOrder::Relaxed); }

private:
	TSpscQueue<T>   Inner;
	int32           Capacity;
	TAtomic<int32>  Count    { 0 };
	TAtomic<uint64> DropCount{ 0 };
};
