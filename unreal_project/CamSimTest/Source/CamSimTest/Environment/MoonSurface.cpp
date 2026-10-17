// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/MoonSurface.h"
#include "CamSimTest.h"
#include "Environment/NightIllumination.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Albedo map
// ---------------------------------------------------------------------------

double FMoonAlbedoMap::Sample(double LonRad, double LatRad) const
{
	if (IsEmpty())
	{
		return 1.0;
	}
	// Texture coordinates as CamSimMoonDisc: u = lon / 2pi + 0.5, v = 0.5 - lat / pi; bilinear between texel centres.
	const double U = LonRad / (2.0 * UE_DOUBLE_PI) + 0.5;
	const double V = 0.5 - LatRad / UE_DOUBLE_PI;
	const double X = U * Width - 0.5;
	const double Y = FMath::Clamp(V * Height - 0.5, 0.0, static_cast<double>(Height - 1));
	const double X0f = FMath::FloorToDouble(X), Y0f = FMath::FloorToDouble(Y);
	const double Fx = X - X0f, Fy = Y - Y0f;
	auto Wrap = [this](int64 I) { return static_cast<int32>(((I % Width) + Width) % Width); };
	const int32 X0 = Wrap(static_cast<int64>(X0f)), X1 = Wrap(static_cast<int64>(X0f) + 1);
	const int32 Y0 = static_cast<int32>(Y0f), Y1 = FMath::Min(Y0 + 1, Height - 1);
	auto T = [this](int32 Xi, int32 Yi) { return static_cast<double>(Texels[Yi * Width + Xi]); };
	const double Top = T(X0, Y0) * (1.0 - Fx) + T(X1, Y0) * Fx;
	const double Bot = T(X0, Y1) * (1.0 - Fx) + T(X1, Y1) * Fx;
	return (Top * (1.0 - Fy) + Bot * Fy) / 255.0;
}

FMoonAlbedoMap FMoonAlbedoMap::ParsePgm(const TArray<uint8>& Bytes, FString& OutError)
{
	FMoonAlbedoMap Map;
	int32 Pos = 0;
	auto SkipSpaceAndComments = [&]()
	{
		while (Pos < Bytes.Num())
		{
			if (Bytes[Pos] == '#')
			{
				while (Pos < Bytes.Num() && Bytes[Pos] != '\n') ++Pos;
			}
			else if (FChar::IsWhitespace(static_cast<TCHAR>(Bytes[Pos])))
			{
				++Pos;
			}
			else
			{
				break;
			}
		}
	};
	auto ReadInt = [&](int32& Out) -> bool
	{
		SkipSpaceAndComments();
		int64 Value = 0;
		const int32 Start = Pos;
		while (Pos < Bytes.Num() && Bytes[Pos] >= '0' && Bytes[Pos] <= '9' && Pos - Start < 9)
		{
			Value = Value * 10 + (Bytes[Pos] - '0');
			++Pos;
		}
		Out = static_cast<int32>(Value);
		return Pos > Start;
	};
	if (Bytes.Num() < 2 || Bytes[0] != 'P' || Bytes[1] != '5')
	{
		OutError = TEXT("not a binary PGM (P5)");
		return Map;
	}
	Pos = 2;
	int32 W = 0, H = 0, MaxVal = 0;
	if (!ReadInt(W) || !ReadInt(H) || !ReadInt(MaxVal) || W <= 0 || H <= 0 || W > 16384 || H > 16384 || MaxVal != 255)
	{
		OutError = TEXT("bad PGM header (need 8-bit, at most 16384 x 16384)");
		return Map;
	}
	++Pos;   // the single whitespace byte after maxval
	const int64 Size = static_cast<int64>(W) * H;
	if (Bytes.Num() - Pos < Size)
	{
		OutError = FString::Printf(TEXT("truncated PGM (%d of %lld bytes)"), Bytes.Num() - Pos, Size);
		return Map;
	}
	Map.Width = W;
	Map.Height = H;
	Map.Texels.Append(Bytes.GetData() + Pos, static_cast<int32>(Size));
	return Map;
}

FString FMoonAlbedoMap::DefaultPath()
{
	return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NonUFS/Sky/moon_albedo.pgm"));
}

FMoonAlbedoMap FMoonAlbedoMap::Load(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogCamSim, Warning, TEXT("moon albedo map %s missing: uniform moon disc (git lfs pull)"), *Path);
		return {};
	}
	static const char LfsPrefix[] = "version https://git-lfs";
	if (Bytes.Num() >= static_cast<int32>(sizeof(LfsPrefix) - 1) && FMemory::Memcmp(Bytes.GetData(), LfsPrefix, sizeof(LfsPrefix) - 1) == 0)
	{
		UE_LOG(LogCamSim, Warning, TEXT("moon albedo map %s is a git LFS pointer: uniform moon disc (git lfs pull)"), *Path);
		return {};
	}
	FString Err;
	FMoonAlbedoMap Map = ParsePgm(Bytes, Err);
	if (Map.IsEmpty())
	{
		UE_LOG(LogCamSim, Warning, TEXT("moon albedo map %s: %s: uniform moon disc"), *Path, *Err);
	}
	return Map;
}

// ---------------------------------------------------------------------------
// IAU orientation (Archinal et al. 2011, table 2; NAIF pck00010.tpc BODY301)
// ---------------------------------------------------------------------------

void FMoonOrientation::IauAxesJ2000(double D, FVector3d OutAxes[3])
{
	const double T = D / 36525.0;
	auto E = [D](double A0, double Rate) { return FMath::DegreesToRadians(A0 + Rate * D); };
	const double E1 = E(125.045, -0.0529921), E2 = E(250.089, -0.1059842), E3 = E(260.008, 13.0120009);
	const double E4 = E(176.625, 13.3407154), E5 = E(357.529, 0.9856003), E6 = E(311.589, 26.4057084);
	const double E7 = E(134.963, 13.0649930), E8 = E(276.617, 0.3287146), E9 = E(34.226, 1.7484877);
	const double E10 = E(15.134, -0.1589763), E11 = E(119.743, 0.0036096), E12 = E(239.961, 0.1643573);
	const double E13 = E(25.053, 12.9590088);
	const double Ra = 269.9949 + 0.0031 * T - 3.8787 * FMath::Sin(E1) - 0.1204 * FMath::Sin(E2) + 0.0700 * FMath::Sin(E3)
		- 0.0172 * FMath::Sin(E4) + 0.0072 * FMath::Sin(E6) - 0.0052 * FMath::Sin(E10) + 0.0043 * FMath::Sin(E13);
	const double Dec = 66.5392 + 0.0130 * T + 1.5419 * FMath::Cos(E1) + 0.0239 * FMath::Cos(E2) - 0.0278 * FMath::Cos(E3)
		+ 0.0068 * FMath::Cos(E4) - 0.0029 * FMath::Cos(E6) + 0.0009 * FMath::Cos(E7) + 0.0008 * FMath::Cos(E10)
		- 0.0009 * FMath::Cos(E13);
	const double W = 38.3213 + 13.17635815 * D - 1.4e-12 * D * D + 3.5610 * FMath::Sin(E1) + 0.1208 * FMath::Sin(E2)
		- 0.0642 * FMath::Sin(E3) + 0.0158 * FMath::Sin(E4) + 0.0252 * FMath::Sin(E5) - 0.0066 * FMath::Sin(E6)
		- 0.0047 * FMath::Sin(E7) - 0.0046 * FMath::Sin(E8) + 0.0028 * FMath::Sin(E9) + 0.0052 * FMath::Sin(E10)
		+ 0.0040 * FMath::Sin(E11) + 0.0019 * FMath::Sin(E12) - 0.0044 * FMath::Sin(E13);
	const double A = FMath::DegreesToRadians(Ra), Dl = FMath::DegreesToRadians(Dec), Wr = FMath::DegreesToRadians(FMath::Fmod(W, 360.0));
	// Pole; the node Q of the body equator on the J2000 equator (RA + 90 deg); the prime meridian is W east of Q.
	const FVector3d Pole(FMath::Cos(Dl) * FMath::Cos(A), FMath::Cos(Dl) * FMath::Sin(A), FMath::Sin(Dl));
	const FVector3d Node(-FMath::Sin(A), FMath::Cos(A), 0.0);
	const FVector3d NodeEast = Pole.Cross(Node);   // 90 deg east of the node in the body equator
	OutAxes[0] = Node * FMath::Cos(Wr) + NodeEast * FMath::Sin(Wr);
	OutAxes[2] = Pole;
	OutAxes[1] = Pole.Cross(OutAxes[0]);
}

FVector2d FMoonOrientation::SubPointDeg(const FVector3d Axes[3], const FVector3d& ToObserver)
{
	const FVector3d B(ToObserver.Dot(Axes[0]), ToObserver.Dot(Axes[1]), ToObserver.Dot(Axes[2]));
	const double N = FMath::Max(B.Length(), 1e-12);
	return FVector2d(FMath::RadiansToDegrees(FMath::Atan2(B.Y, B.X)), FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(B.Z / N, -1.0, 1.0))));
}

// ---------------------------------------------------------------------------
// Disc model (mirrors CamSimMoonDisc)
// ---------------------------------------------------------------------------

void FMoonDisc::DiscBasis(const FMoonDiscFrame& F, FVector3d& OutRight, FVector3d& OutUp)
{
	FVector3d P = F.SunDir - F.SunDir.Dot(F.MoonDir) * F.MoonDir;
	if (P.SquaredLength() < 1e-12)
	{
		P = F.MoonDir.Cross(FMath::Abs(F.MoonDir.Z) < 0.9 ? FVector3d::UnitZ() : FVector3d::UnitX());
	}
	OutRight = P.GetSafeNormal();
	OutUp = F.MoonDir.Cross(OutRight);
}

double FMoonDisc::Radiance(const FMoonDiscFrame& F, const FMoonAlbedoMap& Map, double U, double V)
{
	const double R2 = U * U + V * V;
	if (R2 > 1.0)
	{
		return 0.0;
	}
	FVector3d Right, Up;
	DiscBasis(F, Right, Up);
	const double Nz = FMath::Sqrt(1.0 - R2);
	const FVector3d N = U * Right + V * Up - Nz * F.MoonDir;
	const double Shade = Photometric(N.Dot(F.SunDir), Nz, F.L);
	if (Shade <= 0.0)
	{
		return 0.0;
	}
	const double Lon = FMath::Atan2(N.Dot(F.Axes[1]), N.Dot(F.Axes[0]));
	const double Lat = FMath::Asin(FMath::Clamp(N.Dot(F.Axes[2]), -1.0, 1.0));
	return Map.Sample(Lon, Lat) * Shade;
}

double FMoonDisc::MeanRadiance(const FMoonDiscFrame& F, const FMoonAlbedoMap& Map, int32 N)
{
	if (N <= 0)
	{
		const double CosPhase = -F.SunDir.Dot(F.MoonDir);   // sun-moon-observer angle (the sun's distance >> the moon's)
		N = CosPhase < FMath::Cos(FMath::DegreesToRadians(100.0)) ? 256 : 128;
	}
	N = FMath::Max(N, 4);
	double Sum = 0.0;
	int32 Count = 0;
	for (int32 J = 0; J < N; ++J)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const double U = -1.0 + 2.0 * (I + 0.5) / N, V = -1.0 + 2.0 * (J + 0.5) / N;
			if (U * U + V * V > 1.0) continue;
			Sum += Radiance(F, Map, U, V);
			++Count;
		}
	}
	return FMath::Max(Count ? Sum / Count : 0.0, 1e-9);
}
