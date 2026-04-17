// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "CIGI/BoundedSpscQueue.h"
#include "DIS/DisPduTypes.h"

struct FCamSimConfig;

/**
 * FDisReceiver
 *
 * Background FRunnable thread that:
 *   - Binds a UDP socket on the DIS port (multicast join if configured)
 *   - Reads raw DIS PDUs from UDP datagrams
 *   - Filters by exercise ID
 *   - Parses Entity State PDUs and enqueues them into a SPSC queue
 *   - Optionally logs raw PDU bytes to a file (binary recording)
 *
 * Thread safety: Only the game thread calls Dequeue*; only the receiver
 * thread calls the enqueue side.
 *
 * Follows the same pattern as FCigiReceiver.
 */
class FDisReceiver : public FRunnable
{
public:
	explicit FDisReceiver(const FCamSimConfig& InConfig);
	virtual ~FDisReceiver();

	/** Starts the background thread. Returns false if socket bind fails. */
	bool Start();

	/** Signals the thread to exit and waits for it to finish. */
	void Stop();

	/** Number of DIS PDUs successfully parsed. */
	uint64 GetReceivedPduCount() const { return ReceivedPduCount.Load(); }

	/** Number of UDP datagrams successfully read from the DIS socket (before parse/filter). */
	uint64 GetReceivedPacketCount() const { return ReceivedPacketCount.Load(); }

	/** Dequeue the next parsed Entity State PDU. Returns false if empty. */
	bool DequeueEntityStatePdu(FDisEntityStatePdu& Out) { return EntityStatePduQueue.Dequeue(Out); }

	/** Dequeue the next parsed Designator PDU. Returns false if empty. */
	bool DequeueDesignatorPdu(FDisDesignatorPdu& Out) { return DesignatorPduQueue.Dequeue(Out); }

	// FRunnable interface
	virtual bool   Init() override;
	virtual uint32 Run() override;
	virtual void   Exit() override;

private:
	const FCamSimConfig& Config;

	FRunnableThread* Thread    = nullptr;
	FSocket*         Socket    = nullptr;
	TAtomic<bool>    bShouldRun;
	// Auto-reset event: Stop() triggers it so the receiver wakes instantly
	// from its non-blocking recv poll instead of stalling shutdown by up to
	// one poll interval.
	FEvent*          ShutdownEvent = nullptr;
	TAtomic<uint64>  ReceivedPduCount { 0 };
	TAtomic<uint64>  ReceivedPacketCount { 0 };

	// Bounded SPSC queues: receiver thread produces, game thread consumes.
	// 2048 entity states absorbs a ~1 s game-thread stall at typical DIS fan-in
	// (many exercises broadcast ~500 PDUs/sec). Drop-newest on overflow.
	static constexpr int32 DisEntityQueueCapacity     = 2048;
	static constexpr int32 DisDesignatorQueueCapacity = 256;
	TBoundedSpscQueue<FDisEntityStatePdu> EntityStatePduQueue { DisEntityQueueCapacity };
	TBoundedSpscQueue<FDisDesignatorPdu>  DesignatorPduQueue  { DisDesignatorQueueCapacity };

public:
	/** Total drops across DIS queues — for /metrics exposure. */
	uint64 GetTotalDropCount() const
	{
		return EntityStatePduQueue.GetDropCount() + DesignatorPduQueue.GetDropCount();
	}

private:
	bool CreateSocket();
};
