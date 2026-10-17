// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/SkyDomeDriver.h"
#include "CamSimTest.h"
#include "Environment/CelestialEnables.h"
#include "Environment/Ephemeris.h"
#include "Environment/NightIllumination.h"
#include "Environment/StarField.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

namespace
{
	/** Transient non-sRGB RGBA16F texture, point filtered, no mips, never streamed (M_SkyDome reads it with Load). */
	UTexture2D* MakeStarTexture(int32 W, int32 H, const FFloat16Color* Texels)
	{
		UTexture2D* Tex = UTexture2D::CreateTransient(W, H, PF_FloatRGBA, TEXT("CamSimStarField"));
		if (!Tex) return nullptr;
		FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
		void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dest, Texels, static_cast<SIZE_T>(W) * H * sizeof(FFloat16Color));
		Mip.BulkData.Unlock();
		Tex->SRGB = false;
		Tex->Filter = TF_Nearest;
		Tex->NeverStream = true;
		Tex->CompressionSettings = TC_HDR;
		Tex->UpdateResource();
		return Tex;
	}

	/** Transient linear G8 texture of the moon albedo with a box-filtered mip chain (M_SkyDome samples it at an explicit
	 *  LOD, reading .r); longitude wraps, latitude clamps. A 1 x 1 white texture for an empty map. */
	UTexture2D* MakeMoonTexture(const FMoonAlbedoMap& Map)
	{
		const int32 W = Map.IsEmpty() ? 1 : Map.Width, H = Map.IsEmpty() ? 1 : Map.Height;
		UTexture2D* Tex = UTexture2D::CreateTransient(W, H, PF_G8, TEXT("CamSimMoonAlbedo"));
		if (!Tex) return nullptr;
		TArray<uint8> Level = Map.IsEmpty() ? TArray<uint8>({ 255 }) : Map.Texels;
		int32 Lw = W, Lh = H;
		FTexturePlatformData* Pd = Tex->GetPlatformData();
		for (int32 Mip = 0;; ++Mip)
		{
			FTexture2DMipMap* MipMap = Mip == 0 ? &Pd->Mips[0] : new FTexture2DMipMap(Lw, Lh, 1);
			if (Mip > 0) Pd->Mips.Add(MipMap);
			MipMap->BulkData.Lock(LOCK_READ_WRITE);
			uint8* Dest = static_cast<uint8*>(MipMap->BulkData.Realloc(static_cast<int64>(Lw) * Lh));
			FMemory::Memcpy(Dest, Level.GetData(), static_cast<SIZE_T>(Lw) * Lh);
			MipMap->BulkData.Unlock();
			if (Lw == 1 && Lh == 1) break;
			const int32 Nw = FMath::Max(Lw / 2, 1), Nh = FMath::Max(Lh / 2, 1);
			TArray<uint8> Next;
			Next.SetNumUninitialized(Nw * Nh);
			for (int32 Y = 0; Y < Nh; ++Y)
			{
				for (int32 X = 0; X < Nw; ++X)
				{
					const int32 X0 = FMath::Min(2 * X, Lw - 1), X1 = FMath::Min(2 * X + 1, Lw - 1);
					const int32 Y0 = FMath::Min(2 * Y, Lh - 1), Y1 = FMath::Min(2 * Y + 1, Lh - 1);
					const int32 Sum = Level[Y0 * Lw + X0] + Level[Y0 * Lw + X1] + Level[Y1 * Lw + X0] + Level[Y1 * Lw + X1];
					Next[Y * Nw + X] = static_cast<uint8>((Sum + 2) / 4);
				}
			}
			Level = MoveTemp(Next);
			Lw = Nw;
			Lh = Nh;
		}
		Tex->SRGB = false;
		Tex->Filter = TF_Trilinear;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Clamp;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		return Tex;
	}
}

FVector FSkyDomeDriver::EnuToWorldDir(const FMatrix& EsuToUnreal, const FVector3d& Enu)
{
	return EsuToUnreal.TransformVector(FVector(Enu.X, -Enu.Y, Enu.Z)).GetSafeNormal();
}

double FSkyDomeDriver::CenturiesSinceJ2000(const FDateTime& Utc)
{
	return (FEphemeris::JulianDayUtc(Utc) - 2451545.0) / 36525.0;
}

void FSkyDomeDriver::PrecessionJ2000ToDate(double T, FVector OutP[3])
{
	// Meeus (21.2): zeta, z, theta in arcseconds for a J2000.0 starting epoch.
	const double ArcsecToRad = UE_DOUBLE_PI / (180.0 * 3600.0);
	const double Zeta  = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) * ArcsecToRad;
	const double Z     = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) * ArcsecToRad;
	const double Theta = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) * ArcsecToRad;
	// (21.4) as rotations: v_date = Rz(z) · R(theta) · Rz(zeta) · v_J2000, Rz(a) adding a to the right ascension and
	// R(theta) = [[cos, 0, -sin], [0, 1, 0], [sin, 0, cos]] (A, B, C of Meeus).
	auto Rz = [](double A) { const double C = FMath::Cos(A), S = FMath::Sin(A); return FMatrix(FPlane(C, -S, 0, 0), FPlane(S, C, 0, 0), FPlane(0, 0, 1, 0), FPlane(0, 0, 0, 1)); };
	const double Ct = FMath::Cos(Theta), St = FMath::Sin(Theta);
	const FMatrix Rt(FPlane(Ct, 0, -St, 0), FPlane(0, 1, 0, 0), FPlane(St, 0, Ct, 0), FPlane(0, 0, 0, 1));
	// FMatrix rows written as above are math rows; compose M = Rz(z) · Rt · Rz(zeta) with row-major products.
	auto Mul = [](const FMatrix& A, const FMatrix& B)
	{
		FMatrix R = FMatrix::Identity;
		for (int32 i = 0; i < 3; ++i)
			for (int32 j = 0; j < 3; ++j)
				R.M[i][j] = A.M[i][0] * B.M[0][j] + A.M[i][1] * B.M[1][j] + A.M[i][2] * B.M[2][j];
		return R;
	};
	const FMatrix P = Mul(Mul(Rz(Z), Rt), Rz(Zeta));
	for (int32 i = 0; i < 3; ++i)
	{
		OutP[i] = FVector(P.M[i][0], P.M[i][1], P.M[i][2]);
	}
}

void FSkyDomeDriver::SkyRotationRows(const FMatrix& EsuToUnreal, double LatDeg, double LastDeg, double CenturiesJ2000, FVector OutRows[3])
{
	// World → ENU: the components of a world direction along East, North, Up.
	const FVector EastW  = EnuToWorldDir(EsuToUnreal, FVector3d(1, 0, 0));
	const FVector NorthW = EnuToWorldDir(EsuToUnreal, FVector3d(0, 1, 0));
	const FVector UpW    = EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1));
	// ENU → equatorial: the local axes in the equatorial frame. The zenith is at (RA θ, Dec φ); East = pole × zenith;
	// North = zenith × East.
	double SinP, CosP, SinT, CosT;
	FMath::SinCos(&SinP, &CosP, FMath::DegreesToRadians(LatDeg));
	FMath::SinCos(&SinT, &CosT, FMath::DegreesToRadians(LastDeg));
	const FVector EastEq(-SinT, CosT, 0.0);
	const FVector NorthEq(-SinP * CosT, -SinP * SinT, CosP);
	const FVector ZenEq(CosP * CosT, CosP * SinT, SinP);
	FVector DateRows[3];   // world → equatorial of date
	for (int32 i = 0; i < 3; ++i)
	{
		DateRows[i] = EastEq[i] * EastW + NorthEq[i] * NorthW + ZenEq[i] * UpW;
	}
	// Of date → J2000 is the transpose of the precession: J2000 row i = sum_k P[k][i] · date row k.
	FVector P[3];
	PrecessionJ2000ToDate(CenturiesJ2000, P);
	for (int32 i = 0; i < 3; ++i)
	{
		OutRows[i] = P[0][i] * DateRows[0] + P[1][i] * DateRows[1] + P[2][i] * DateRows[2];
	}
}

double FSkyDomeDriver::DaysTtFromCenturiesUt(double CenturiesJ2000)
{
	const double Days = CenturiesJ2000 * 36525.0;
	const double Year = 2000.0 + Days / 365.25;
	const int32 Y = FMath::FloorToInt32(Year);
	const int32 M = FMath::Clamp(1 + FMath::FloorToInt32((Year - Y) * 12.0), 1, 12);
	return Days + FEphemeris::DeltaTSeconds(Y, M) / 86400.0;
}

FMoonDiscFrame FSkyDomeDriver::MoonFrame(const FEphemerisResult& E, const FMatrix& EsuToUnreal, const FVector Rows[3], double DaysTt)
{
	FMoonDiscFrame F;
	F.MoonDir = EnuToWorldDir(EsuToUnreal, E.MoonEnu);
	F.SunDir = EnuToWorldDir(EsuToUnreal, E.SunEnu);
	FVector3d AxesJ2000[3];
	FMoonOrientation::IauAxesJ2000(DaysTt, AxesJ2000);
	for (int32 K = 0; K < 3; ++K)
	{
		// Rows map world → J2000 (orthonormal), so a J2000 vector a is Σ_i a_i Rows[i] in world.
		F.Axes[K] = (AxesJ2000[K].X * Rows[0] + AxesJ2000[K].Y * Rows[1] + AxesJ2000[K].Z * Rows[2]).GetSafeNormal();
	}
	F.L = FMoonDisc::LunarLambertL(E.MoonPhaseAngleDeg);
	return F;
}

void FSkyDomeDriver::Init(AActor* Owner, const FCamSimConfig::FCelestialConfig& Cfg)
{
	if (Dome || !Owner) return;

	Mpc = LoadObject<UMaterialParameterCollection>(nullptr, MpcPath);
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, MaterialPath);
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, MeshPath);
	if (!Mpc || !Parent || !Sphere)
	{
		UE_LOG(LogCamSim, Warning, TEXT("6A sky dome: missing %s%s%s — no dome (atmosphere only, no moon disc or stars)"),
			Mpc ? TEXT("") : MpcPath, Parent ? TEXT("") : MaterialPath, Sphere ? TEXT("") : MeshPath);
		return;
	}

	Dome = NewObject<UStaticMeshComponent>(Owner, TEXT("CamSimSkyDome"));
	Dome->SetStaticMesh(Sphere);
	Dome->SetMobility(EComponentMobility::Movable);
	Dome->SetUsingAbsoluteLocation(true);
	Dome->SetUsingAbsoluteRotation(true);
	Dome->SetUsingAbsoluteScale(true);
	const double MeshRadiusCm = FMath::Max(Sphere->GetBounds().SphereRadius, 1.0);
	Dome->SetWorldScale3D(FVector(DomeRadiusCm / MeshRadiusCm));
	Dome->SetCastShadow(false);
	Dome->bCastDynamicShadow = false;
	Dome->bAffectDistanceFieldLighting = false;
	Dome->bAffectDynamicIndirectLighting = false;
	Dome->bVisibleInRayTracing = false;
	Dome->bReceivesDecals = false;
	Dome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Dome->SetCanEverAffectNavigation(false);
	if (USceneComponent* Root = Owner->GetRootComponent())
	{
		Dome->SetupAttachment(Root);
	}
	Material = UMaterialInstanceDynamic::Create(Parent, Dome);
	Dome->SetMaterial(0, Material);

	const FString Path = Cfg.StarCatalog.IsEmpty() ? FStarField::DefaultPath() : Cfg.StarCatalog;
	const FStarFieldImage Stars = FStarField::LoadDefault(Path);
	if (!Stars.IsEmpty())
	{
		StarTexture = MakeStarTexture(Stars.Width, Stars.Height, Stars.Texels.GetData());
		for (const FFloat16Color& T : Stars.Texels)
		{
			if (T.R.GetFloat() > 0.0f || T.G.GetFloat() > 0.0f || T.B.GetFloat() > 0.0f) ++StarTexelCount;
		}
	}
	bHasStars = StarTexture != nullptr;
	if (!StarTexture)
	{
		const FFloat16Color Black(FLinearColor::Black);
		StarTexture = MakeStarTexture(1, 1, &Black);
	}
	Material->SetTextureParameterValue(TEXT("StarTex"), StarTexture);

	MoonAlbedo = FMoonAlbedoMap::Load(FMoonAlbedoMap::DefaultPath());
	MoonTexture = MakeMoonTexture(MoonAlbedo);
	if (MoonTexture)
	{
		Material->SetTextureParameterValue(TEXT("MoonTex"), MoonTexture);
	}

	Dome->RegisterComponent();
	Owner->AddInstanceComponent(Dome);
}

void FSkyDomeDriver::SetVector(UWorld* World, const TCHAR* Name, const FLinearColor& Value)
{
	UMaterialParameterCollectionInstance* Instance = World ? World->GetParameterCollectionInstance(Mpc) : nullptr;
	if ((!Instance || !Instance->SetVectorParameterValue(FName(Name), Value)) && !bWarnedMpc)
	{
		bWarnedMpc = true;
		UE_LOG(LogCamSim, Warning, TEXT("6A sky dome: MPC_Sky has no vector parameter %s (regenerate with scripts/sky/make_sky_material.sh)"), Name);
	}
}

void FSkyDomeDriver::Update(const FEphemerisResult& E, const FNightIlluminationResult& I, const FCelestialEnables& En,
	const FMatrix& EsuToUnreal, const FVector& CameraWorld, double LatDeg, double CenturiesJ2000)
{
	if (!Dome || !Mpc) return;
	UWorld* World = Dome->GetWorld();

	Dome->SetWorldLocation(CameraWorld);

	const FVector MoonDir = EnuToWorldDir(EsuToUnreal, E.MoonEnu);
	const FVector SunDir  = EnuToWorldDir(EsuToUnreal, E.SunEnu);
	const FVector UpWorld = EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1));

	FVector Rows[3];
	SkyRotationRows(EsuToUnreal, LatDeg, E.LocalSiderealDeg, CenturiesJ2000, Rows);

	// Moon disc: angular radius r, solid angle Ω = 2π(1 − cos r); luminance scale so that the disc (albedo × lunar-Lambert,
	// the IAU orientation) integrates to the moon's top-of-atmosphere illuminance (the material applies the transmittance
	// along the view direction).
	const double Radius = FMath::Asin(FMath::Clamp(MoonRadiusKm / FMath::Max(E.MoonDistanceKm, MoonRadiusKm), 0.0, 1.0));
	const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(Radius));
	const bool bMoon = En.bMoon && I.MoonToaLux > 0.0;
	const FMoonDiscFrame Frame = MoonFrame(E, EsuToUnreal, Rows, DaysTtFromCenturiesUt(CenturiesJ2000));
	// Below the horizon the disc isn't drawn: keep the last mean rather than recompute for nothing.
	static constexpr double DrawnAboveDeg = -2.0;
	const double Mean = (bMoon && E.MoonElDeg > DrawnAboveDeg) ? MoonNormaliser.Get(Frame, MoonAlbedo) : LastMoonMean;
	LastMoonMean = Mean;
	const double LumScale = (bMoon && Omega > 0.0) ? I.MoonToaLux / (Omega * Mean) : 0.0;

	SetVector(World, TEXT("MoonDir"), FLinearColor(MoonDir.X, MoonDir.Y, MoonDir.Z, 0.0f));
	SetVector(World, TEXT("SunDir"), FLinearColor(SunDir.X, SunDir.Y, SunDir.Z, 0.0f));
	SetVector(World, TEXT("MoonParams"), FLinearColor(static_cast<float>(Radius), static_cast<float>(LumScale),
		static_cast<float>(Frame.L), bMoon ? 1.0f : 0.0f));
	SetVector(World, TEXT("MoonAxisX"), FLinearColor(Frame.Axes[0].X, Frame.Axes[0].Y, Frame.Axes[0].Z, 0.0f));
	SetVector(World, TEXT("MoonAxisY"), FLinearColor(Frame.Axes[1].X, Frame.Axes[1].Y, Frame.Axes[1].Z, 0.0f));
	SetVector(World, TEXT("MoonAxisZ"), FLinearColor(Frame.Axes[2].X, Frame.Axes[2].Y, Frame.Axes[2].Z, 0.0f));
	SetVector(World, TEXT("SkyRot0"), FLinearColor(Rows[0].X, Rows[0].Y, Rows[0].Z, 0.0f));
	SetVector(World, TEXT("SkyRot1"), FLinearColor(Rows[1].X, Rows[1].Y, Rows[1].Z, 0.0f));
	SetVector(World, TEXT("SkyRot2"), FLinearColor(Rows[2].X, Rows[2].Y, Rows[2].Z, 0.0f));
	SetVector(World, TEXT("Floor"), FLinearColor(static_cast<float>(I.FloorRadianceCdM2), En.bStars ? En.StarScale : 0.0f, ExtinctionKV, 0.0f));
	SetVector(World, TEXT("UpWorld"), FLinearColor(UpWorld.X, UpWorld.Y, UpWorld.Z, 0.0f));
}
