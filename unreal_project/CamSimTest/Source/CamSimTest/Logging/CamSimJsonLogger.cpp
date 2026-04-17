// Copyright CamSim Contributors. All Rights Reserved.

#include "Logging/CamSimJsonLogger.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"

bool FCamSimJsonLogger::Open(const FString& InPath, int32 MaxSizeMB)
{
	Close();
	FilePath = InPath;
	MaxSizeBytes = static_cast<int64>(MaxSizeMB) * 1024 * 1024;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FileHandle = PlatformFile.OpenWrite(*FilePath, /*bAppend=*/true, /*bAllowRead=*/true);
	if (!FileHandle) return false;

	CurrentSizeBytes = PlatformFile.FileSize(*FilePath);

	// Start the consumer thread. The producer (Log()) never touches the
	// file handle directly — all writes are serialised through Run().
	bShouldRun = true;
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);
	ConsumerThread = FRunnableThread::Create(this, TEXT("CamSimJsonLogger"),
		64 * 1024, TPri_BelowNormal,
		FPlatformAffinity::GetTaskGraphBackgroundTaskMask());

	return ConsumerThread != nullptr;
}

void FCamSimJsonLogger::Close()
{
	// Signal the consumer to exit and wake it so it doesn't wait for the event.
	bShouldRun = false;
	if (WakeEvent) WakeEvent->Trigger();
	if (ConsumerThread)
	{
		ConsumerThread->WaitForCompletion();
		delete ConsumerThread;
		ConsumerThread = nullptr;
	}
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}

	// Final drain of anything the consumer might have missed between its last
	// wake and shutdown.
	Flush();
	if (FileHandle)
	{
		delete FileHandle;
		FileHandle = nullptr;
	}
	CurrentSizeBytes = 0;
}

void FCamSimJsonLogger::Log(const TCHAR* Severity, const TCHAR* Category,
                            const TCHAR* Message, const TMap<FString, FString>& Fields)
{
	if (!FileHandle) return;

	FLogEntry Entry;
	Entry.Severity = Severity;
	Entry.Category = Category;
	Entry.Message = Message;
	Entry.Fields = Fields;
	Entry.Timestamp = FDateTime::UtcNow();
	Queue.Enqueue(MoveTemp(Entry));
	if (WakeEvent) WakeEvent->Trigger();
}

uint32 FCamSimJsonLogger::Run()
{
	while (bShouldRun)
	{
		FLogEntry Entry;
		while (Queue.Dequeue(Entry))
		{
			RotateIfNeeded();
			WriteEntry(Entry);
		}
		// Sleep until a new Log() call or Close() fires the event. 1 s poll
		// is a safety net in case a trigger is lost.
		if (WakeEvent) WakeEvent->Wait(1000);
		else           break;
	}

	// Drain once more before returning — Close() still calls Flush() but this
	// covers any entries enqueued between the last dequeue and bShouldRun=false.
	FLogEntry Entry;
	while (Queue.Dequeue(Entry))
	{
		RotateIfNeeded();
		WriteEntry(Entry);
	}
	return 0;
}

void FCamSimJsonLogger::Flush()
{
	if (!FileHandle) return;

	if (ConsumerThread)
	{
		// Consumer is the SPSC reader; Flush() must not Dequeue directly or
		// we'd violate the invariant. Nudge the consumer and poll until the
		// queue drains (or we hit a reasonable ceiling).
		if (WakeEvent) WakeEvent->Trigger();
		constexpr int32 MaxWaitMs = 500;
		for (int32 i = 0; i < MaxWaitMs && !Queue.IsEmpty(); ++i)
		{
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}
	else
	{
		// Consumer has stopped (Close() teardown path) — safe to drain directly.
		FLogEntry Entry;
		while (Queue.Dequeue(Entry))
		{
			RotateIfNeeded();
			WriteEntry(Entry);
		}
	}
}

void FCamSimJsonLogger::AppendEscapedJson(FString& Out, const FString& In)
{
	Out.Reserve(Out.Len() + In.Len() + 2);
	for (TCHAR Ch : In)
	{
		switch (Ch)
		{
			case TEXT('\"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\b'): Out += TEXT("\\b");  break;
			case TEXT('\f'): Out += TEXT("\\f");  break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				// RFC 7159 requires \uXXXX for every control char < 0x20.
				if (static_cast<uint32>(Ch) < 0x20u)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
				}
				else
				{
					Out.AppendChar(Ch);
				}
				break;
		}
	}
}

FString FCamSimJsonLogger::FormatEntry(const FLogEntry& Entry) const
{
	// Flat-object JSON line. Keys are ASCII and controlled internally, so
	// they don't need escaping; all dynamic strings (values) go through
	// AppendEscapedJson so newlines and control chars can't break the JSONL
	// framing downstream.
	FString Json;
	Json.Reserve(128 + Entry.Message.Len());
	Json += TEXT("{\"ts\":\"");
	AppendEscapedJson(Json, Entry.Timestamp.ToIso8601());
	Json += TEXT("\",\"severity\":\"");
	AppendEscapedJson(Json, Entry.Severity);
	Json += TEXT("\",\"category\":\"");
	AppendEscapedJson(Json, Entry.Category);
	Json += TEXT("\",\"msg\":\"");
	AppendEscapedJson(Json, Entry.Message);
	Json += TEXT("\"");

	for (const auto& Pair : Entry.Fields)
	{
		Json += TEXT(",\"");
		AppendEscapedJson(Json, Pair.Key);
		Json += TEXT("\":\"");
		AppendEscapedJson(Json, Pair.Value);
		Json += TEXT("\"");
	}
	Json += TEXT("}\n");
	return Json;
}

void FCamSimJsonLogger::WriteEntry(const FLogEntry& Entry)
{
	if (!FileHandle) return;
	const FString Line = FormatEntry(Entry);
	auto Utf8 = StringCast<UTF8CHAR>(*Line);
	const int32 Len = Utf8.Length();
	FileHandle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Len);
	CurrentSizeBytes += Len;
}

void FCamSimJsonLogger::RotateIfNeeded()
{
	if (CurrentSizeBytes < MaxSizeBytes) return;

	delete FileHandle;
	FileHandle = nullptr;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString RotatedPath = FilePath + TEXT(".1");
	PlatformFile.DeleteFile(*RotatedPath);
	PlatformFile.MoveFile(*RotatedPath, *FilePath);

	FileHandle = PlatformFile.OpenWrite(*FilePath, /*bAppend=*/false, /*bAllowRead=*/true);
	CurrentSizeBytes = 0;
}
