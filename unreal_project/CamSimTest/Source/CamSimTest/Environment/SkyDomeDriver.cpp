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
}

FVector FSkyDomeDriver::EnuToWorldDir(const FMatrix& EsuToUnreal, const FVector3d& Enu)
{
	return EsuToUnreal.TransformVector(FVector(Enu.X, -Enu.Y, Enu.Z)).GetSafeNormal();
}

void FSkyDomeDriver::SkyRotationRows(const FMatrix& EsuToUnreal, double LatDeg, double LastDeg, FVector OutRows[3])
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
	for (int32 i = 0; i < 3; ++i)
	{
		OutRows[i] = EastEq[i] * EastW + NorthEq[i] * NorthW + ZenEq[i] * UpW;
	}
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
	const FMatrix& EsuToUnreal, const FVector& CameraWorld, double LatDeg)
{
	if (!Dome || !Mpc) return;
	UWorld* World = Dome->GetWorld();

	Dome->SetWorldLocation(CameraWorld);

	const FVector MoonDir = EnuToWorldDir(EsuToUnreal, E.MoonEnu);
	const FVector SunDir  = EnuToWorldDir(EsuToUnreal, E.SunEnu);
	const FVector UpWorld = EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1));

	// Moon disc: angular radius r, solid angle Ω = 2π(1 − cos r); luminance scale so that the shaded disc integrates to the
	// moon's top-of-atmosphere illuminance (the material applies the transmittance along the view direction).
	const double Radius = FMath::Asin(FMath::Clamp(MoonRadiusKm / FMath::Max(E.MoonDistanceKm, MoonRadiusKm), 0.0, 1.0));
	const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(Radius));
	const bool bMoon = En.bMoon && I.MoonToaLux > 0.0;
	const double LumScale = (bMoon && Omega > 0.0) ? I.MoonToaLux / Omega / FMoonDisc::MeanShading(E.MoonPhaseAngleDeg) : 0.0;

	FVector Rows[3];
	SkyRotationRows(EsuToUnreal, LatDeg, E.LocalSiderealDeg, Rows);

	SetVector(World, TEXT("MoonDir"), FLinearColor(MoonDir.X, MoonDir.Y, MoonDir.Z, 0.0f));
	SetVector(World, TEXT("SunDir"), FLinearColor(SunDir.X, SunDir.Y, SunDir.Z, 0.0f));
	SetVector(World, TEXT("MoonParams"), FLinearColor(static_cast<float>(Radius), static_cast<float>(LumScale),
		static_cast<float>(FMath::DegreesToRadians(E.MoonPhaseAngleDeg)), bMoon ? 1.0f : 0.0f));
	SetVector(World, TEXT("SkyRot0"), FLinearColor(Rows[0].X, Rows[0].Y, Rows[0].Z, 0.0f));
	SetVector(World, TEXT("SkyRot1"), FLinearColor(Rows[1].X, Rows[1].Y, Rows[1].Z, 0.0f));
	SetVector(World, TEXT("SkyRot2"), FLinearColor(Rows[2].X, Rows[2].Y, Rows[2].Z, 0.0f));
	SetVector(World, TEXT("Floor"), FLinearColor(static_cast<float>(I.FloorRadianceCdM2), En.bStars ? En.StarScale : 0.0f, ExtinctionKV, 0.0f));
	SetVector(World, TEXT("UpWorld"), FLinearColor(UpWorld.X, UpWorld.Y, UpWorld.Z, 0.0f));
}
