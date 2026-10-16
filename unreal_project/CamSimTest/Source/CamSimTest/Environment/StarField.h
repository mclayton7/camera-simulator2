// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Yale Bright Star Catalogue (BSC5) binned into an equirectangular (RA, Dec) radiance texture (ROADMAP 6A).
 * Pure CPU; the sky dome upload is a later task.
 */
struct FStar
{
	double RaDeg = 0, DecDeg = 0, Vmag = 0, BminusV = 0;   // J2000
};

struct FStarFieldImage
{
	int32 Width = 0, Height = 0;              // x = RA (0 at left, increasing east), y = Dec (+90 top)
	TArray<FFloat16Color> Texels;             // radiance cd/m2 per texel (RGB), A = 1
	double TotalLux = 0;                      // sum of star illuminance (tests, log)
	bool IsEmpty() const { return Texels.Num() == 0; }
};

namespace FStarField
{
	/** Parse the BSC5 binary (28-byte header, 32-byte entries, J2000, little-endian). */
	bool ParseBsc5(const TArray<uint8>& Bytes, TArray<FStar>& Out, FString& OutError);
	double StarLux(double Vmag);                        // 10^(-0.4 (V + 13.98))
	FLinearColor ColorFromBminusV(double BminusV);      // normalised so 0.2126 R + 0.7152 G + 0.0722 B = 1
	FStarFieldImage Bin(const TArray<FStar>& Stars, int32 Width, int32 Height);
	/** Load + parse + bin; on any failure returns an empty image and logs one warning naming the path. */
	FStarFieldImage LoadDefault(const FString& Path, int32 Width = 4096, int32 Height = 2048);
	FString DefaultPath();                              // ProjectContentDir()/NonUFS/Sky/bsc5.dat
}
