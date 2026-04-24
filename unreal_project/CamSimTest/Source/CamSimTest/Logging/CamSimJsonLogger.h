// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CIGI/BoundedSpscQueue.h"
#include "HAL/Runnable.h"
#include "HAL/Event.h"

class FRunnableThread;
class IFileHandle;

/**
 * FCamSimJsonLogger
 *
 * Writes structured JSON lines to a sidecar file for ELK/Datadog ingestion.
 *
 * Threading: Open() starts a dedicated consumer thread that drains the SPSC
 * queue and performs all file I/O. Log() (single-producer) enqueues without
 * blocking, so the producer (game thread or any other single thread) never
 * pays the cost of disk I/O. Flush() is a synchronous drain used by tests
 * and by Close(); in steady state the consumer thread is self-driven.
 *
 * Log rotation: when the file exceeds StructuredLogMaxMB, it is closed,
 * renamed to .1, and a fresh file is opened.
 */
struct FCamSimJsonLogger : public FRunnable
{
	~FCamSimJsonLogger() { Close(); }

	bool Open(const FString& FilePath, int32 MaxSizeMB = 100);
	void Close();
	bool IsOpen() const { return FileHandle != nullptr; }

	/** Enqueue a structured log entry. Must be called from a single producer thread only (SPSC). */
	void Log(const TCHAR* Severity, const TCHAR* Category,
	         const TCHAR* Message, const TMap<FString, FString>& Fields = {});

	/**
	 * Synchronously drain the queue to disk. In normal operation the consumer
	 * thread handles drains automatically; Flush() exists for shutdown and
	 * tests that need to assert on-disk state immediately.
	 */
	void Flush();

	/** Number of log entries dropped because the queue was saturated. */
	uint64 GetDropCount() const { return Queue.GetDropCount(); }

	// FRunnable interface (consumer thread entry point)
	virtual uint32 Run() override;

private:
	struct FLogEntry
	{
		FString Severity;
		FString Category;
		FString Message;
		TMap<FString, FString> Fields;
		FDateTime Timestamp;
	};

	static constexpr int32 QueueCapacity = 4096;

	IFileHandle* FileHandle = nullptr;
	FString FilePath;
	int64 MaxSizeBytes = 100 * 1024 * 1024;
	int64 CurrentSizeBytes = 0;
	TBoundedSpscQueue<FLogEntry> Queue { QueueCapacity };

	TUniquePtr<FRunnableThread> ConsumerThread;
	FEvent*          WakeEvent      = nullptr;  // owned by platform pool; not deleted
	TAtomic<bool>    bShouldRun     { false };

	// Reused by the consumer thread on every WriteEntry() call so the logger
	// doesn't allocate a fresh FString per log line. Only touched from Run().
	FString ScratchJsonBuffer;

	void WriteEntry(const FLogEntry& Entry);
	void RotateIfNeeded();
	// Appends one JSON line (terminated by '\n') into Out. Out is NOT reset —
	// the caller is expected to Reset() the buffer before reuse.
	void AppendFormattedEntry(FString& Out, const FLogEntry& Entry) const;
	// RFC 7159 compliant string escape — replaces the previous \ and " only path.
	static void AppendEscapedJson(FString& Out, const FString& In);
};
