// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/StarField.h"
#include "CamSimTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	constexpr int32 HeaderBytes = 28;

	template <typename T>
	T ReadLE(const uint8* P)
	{
		T V;
		FMemory::Memcpy(&V, P, sizeof(T));   // x86-64 / arm64 are little-endian
		return V;
	}

	double BminusVFromClass(char C)
	{
		switch (C)
		{
		case 'O': return -0.30;
		case 'B': return -0.18;
		case 'A': return 0.0;
		case 'F': return 0.40;
		case 'G': return 0.65;
		case 'K': return 1.10;
		case 'M': return 1.55;
		default:  return 0.6;
		}
	}

	double TexelSolidAngle(int32 Y, int32 Width, int32 Height)
	{
		const double Dec0 = 90.0 - 180.0 * Y / Height, Dec1 = 90.0 - 180.0 * (Y + 1) / Height;
		return (2.0 * UE_DOUBLE_PI / Width)
			* (FMath::Sin(FMath::DegreesToRadians(Dec0)) - FMath::Sin(FMath::DegreesToRadians(Dec1)));
	}
}

bool FStarField::ParseBsc5(const TArray<uint8>& Bytes, TArray<FStar>& Out, FString& OutError)
{
	Out.Reset();
	if (Bytes.Num() < HeaderBytes)
	{
		OutError = TEXT("truncated header");
		return false;
	}
	const uint8* D = Bytes.GetData();
	const int32 StarN = ReadLE<int32>(D + 8);
	const int32 EntryBytes = ReadLE<int32>(D + 24);
	if (StarN >= 0)
	{
		OutError = TEXT("not a J2000 catalogue (STARN >= 0)");
		return false;
	}
	if (EntryBytes < 32)
	{
		OutError = FString::Printf(TEXT("bad entry size %d"), EntryBytes);
		return false;
	}
	const int64 Count = -static_cast<int64>(StarN);
	if (HeaderBytes + Count * EntryBytes > Bytes.Num())
	{
		OutError = TEXT("truncated");
		return false;
	}
	Out.Reserve(static_cast<int32>(Count));
	for (int64 I = 0; I < Count; ++I)
	{
		const uint8* E = D + HeaderBytes + I * EntryBytes;
		const double Ra = ReadLE<double>(E + 4);
		const double Dec = ReadLE<double>(E + 12);
		const char Class0 = static_cast<char>(E[20]);
		const int16 MagV = ReadLE<int16>(E + 22);
		if (MagV == 0 && Ra == 0.0 && Dec == 0.0)
		{
			continue;   // novae / non-stellar placeholders
		}
		FStar S;
		S.RaDeg = FMath::RadiansToDegrees(Ra);
		S.DecDeg = FMath::RadiansToDegrees(Dec);
		S.Vmag = MagV / 100.0;
		S.BminusV = BminusVFromClass(Class0);
		Out.Add(S);
	}
	return true;
}

double FStarField::StarLux(double Vmag)
{
	return FMath::Pow(10.0, -0.4 * (Vmag + 13.98));
}

FLinearColor FStarField::ColorFromBminusV(double BminusV)
{
	const double T = 4600.0 * (1.0 / (0.92 * BminusV + 1.7) + 1.0 / (0.92 * BminusV + 0.62));
	FLinearColor C = FLinearColor::MakeFromColorTemperature(static_cast<float>(T));
	const double Lum = 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B;
	if (Lum <= 0.0)
	{
		return FLinearColor::White;
	}
	return FLinearColor(static_cast<float>(C.R / Lum), static_cast<float>(C.G / Lum), static_cast<float>(C.B / Lum), 1.f);
}

FStarFieldImage FStarField::Bin(const TArray<FStar>& Stars, int32 Width, int32 Height)
{
	FStarFieldImage Img;
	if (Width <= 0 || Height <= 0)
	{
		return Img;
	}
	Img.Width = Width;
	Img.Height = Height;
	TArray<FVector3d> Accum;   // double accumulation, then convert once
	Accum.SetNumZeroed(Width * Height);
	for (const FStar& S : Stars)
	{
		const double Ra = FMath::Fmod(FMath::Fmod(S.RaDeg, 360.0) + 360.0, 360.0);
		const int32 X = FMath::Clamp(FMath::FloorToInt32(Ra / 360.0 * Width), 0, Width - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32((90.0 - S.DecDeg) / 180.0 * Height), 0, Height - 1);
		const double Lux = StarLux(S.Vmag);
		const FLinearColor C = ColorFromBminusV(S.BminusV);
		const double Scale = Lux / TexelSolidAngle(Y, Width, Height);
		Accum[Y * Width + X] += FVector3d(C.R, C.G, C.B) * Scale;
		Img.TotalLux += Lux;
	}
	Img.Texels.SetNumUninitialized(Width * Height);
	for (int32 I = 0; I < Width * Height; ++I)
	{
		Img.Texels[I] = FFloat16Color(FLinearColor(static_cast<float>(Accum[I].X), static_cast<float>(Accum[I].Y), static_cast<float>(Accum[I].Z), 1.f));
	}
	return Img;
}

FString FStarField::DefaultPath()
{
	return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NonUFS/Sky/bsc5.dat"));
}

FStarFieldImage FStarField::LoadDefault(const FString& Path, int32 Width, int32 Height)
{
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogCamSim, Warning, TEXT("star catalogue %s missing: no stars (git lfs pull)"), *Path);
		return {};
	}
	static const char LfsPrefix[] = "version https://git-lfs";
	if (Bytes.Num() >= static_cast<int32>(sizeof(LfsPrefix) - 1) && FMemory::Memcmp(Bytes.GetData(), LfsPrefix, sizeof(LfsPrefix) - 1) == 0)
	{
		UE_LOG(LogCamSim, Warning, TEXT("star catalogue %s is a git LFS pointer: no stars (git lfs pull)"), *Path);
		return {};
	}
	TArray<FStar> Stars;
	FString Err;
	if (!ParseBsc5(Bytes, Stars, Err))
	{
		UE_LOG(LogCamSim, Warning, TEXT("star catalogue %s: %s: no stars"), *Path, *Err);
		return {};
	}
	return Bin(Stars, Width, Height);
}
