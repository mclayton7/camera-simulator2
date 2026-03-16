// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SpscQueue.h"

/**
 * FCamSimJsonLogger
 *
 * Writes structured JSON lines to a sidecar file for ELK/Datadog ingestion.
 * Thread-safe via lock-free SPSC queue: producers enqueue entries, game-thread
 * Flush() writes them to disk. Call Flush() periodically from the subsystem tick.
 *
 * Log rotation: when the file exceeds StructuredLogMaxMB, it is closed,
 * renamed to .1, and a fresh file is opened.
 */
struct FCamSimJsonLogger
{
	bool Open(const FString& FilePath, int32 MaxSizeMB = 100);
	void Close();
	bool IsOpen() const { return FileHandle != nullptr; }

	/** Enqueue a structured log entry. Safe to call from any thread. */
	void Log(const TCHAR* Severity, const TCHAR* Category,
	         const TCHAR* Message, const TMap<FString, FString>& Fields = {});

	/** Flush queued entries to disk. Call from game thread (e.g., subsystem tick). */
	void Flush();

private:
	struct FLogEntry
	{
		FString Severity;
		FString Category;
		FString Message;
		TMap<FString, FString> Fields;
		FDateTime Timestamp;
	};

	IFileHandle* FileHandle = nullptr;
	FString FilePath;
	int64 MaxSizeBytes = 100 * 1024 * 1024;
	int64 CurrentSizeBytes = 0;
	TSpscQueue<FLogEntry> Queue;

	void WriteEntry(const FLogEntry& Entry);
	void RotateIfNeeded();
	FString FormatEntry(const FLogEntry& Entry) const;
};
