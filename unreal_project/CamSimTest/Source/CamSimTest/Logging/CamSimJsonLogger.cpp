// Copyright CamSim Contributors. All Rights Reserved.

#include "Logging/CamSimJsonLogger.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

bool FCamSimJsonLogger::Open(const FString& InPath, int32 MaxSizeMB)
{
	Close();
	FilePath = InPath;
	MaxSizeBytes = static_cast<int64>(MaxSizeMB) * 1024 * 1024;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FileHandle = PlatformFile.OpenWrite(*FilePath, /*bAppend=*/true, /*bAllowRead=*/true);
	if (FileHandle)
	{
		CurrentSizeBytes = PlatformFile.FileSize(*FilePath);
	}
	return FileHandle != nullptr;
}

void FCamSimJsonLogger::Close()
{
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
}

void FCamSimJsonLogger::Flush()
{
	if (!FileHandle) return;

	FLogEntry Entry;
	while (Queue.Dequeue(Entry))
	{
		RotateIfNeeded();
		WriteEntry(Entry);
	}
}

FString FCamSimJsonLogger::FormatEntry(const FLogEntry& Entry) const
{
	// Build JSON manually — avoids FJsonObject allocation overhead for simple flat objects
	FString Json = TEXT("{");
	Json += FString::Printf(TEXT("\"ts\":\"%s\""), *Entry.Timestamp.ToIso8601());
	Json += FString::Printf(TEXT(",\"severity\":\"%s\""), *Entry.Severity);
	Json += FString::Printf(TEXT(",\"category\":\"%s\""), *Entry.Category);

	// Escape quotes in message
	FString EscapedMsg = Entry.Message;
	EscapedMsg.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedMsg.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Json += FString::Printf(TEXT(",\"msg\":\"%s\""), *EscapedMsg);

	for (const auto& Pair : Entry.Fields)
	{
		FString EscapedVal = Pair.Value;
		EscapedVal.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedVal.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Json += FString::Printf(TEXT(",\"%s\":\"%s\""), *Pair.Key, *EscapedVal);
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
