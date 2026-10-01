// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalSky.h"
#include "Thermal/LandCoverGeometry.h"
#include "Time/SimClock.h"

static_assert(FThermalMaterialTable::MaxClasses == FThermalFrameParams::MaxClasses, "class table sizes must agree");

double FThermalFrameBuilder::LocalSolarSeconds(uint64 UtcMicros, double LonDeg)
{
	const double S = FMath::Fmod(static_cast<double>(UtcMicros) * 1e-6 + LonDeg * 240.0, FThermalModel::DaySeconds);
	return S < 0.0 ? S + FThermalModel::DaySeconds : S;
}

FDateTime FThermalFrameBuilder::LocalSolarDate(uint64 UtcMicros, double LonDeg)
{
	return FSimClock::FromMicros(UtcMicros) + FTimespan::FromSeconds(LonDeg * 240.0);
}

double FThermalFrameBuilder::GaussianRadiusM(double LatDeg)
{
	constexpr double A = 6378137.0, E2 = 6.69437999014e-3;
	const double S = FMath::Sin(FMath::DegreesToRadians(LatDeg));
	const double W = 1.0 - E2 * S * S;
	const double M = A * (1.0 - E2) / (W * FMath::Sqrt(W));   // meridional
	const double N = A / FMath::Sqrt(W);                       // prime vertical
	return FMath::Sqrt(M * N);
}

void FThermalFrameBuilder::Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm)
{
	if (!Band.IsBuilt() || Band.GetLoUm() != BandLoUm || Band.GetHiUm() != BandHiUm)
	{
		Band.Build(BandLoUm, BandHiUm);
	}
	const bool bMaterials = !bConfigured || !(Config.Materials == Cfg.Materials);
	if (bMaterials)
	{
		PendingWarnings.Append(Materials.Build(Cfg.Materials));
	}
	if (bMaterials || !(Config.LandCover.Classes == Cfg.LandCover.Classes))
	{
		PendingWarnings.Append(LandCoverTable.Build(Cfg.LandCover.Classes, Materials));
	}
	if (bConfigured && Config.LandCover.Dir != Cfg.LandCover.Dir)
	{
		bWarpAnchor = false;   // new land-cover data: the next window starts a new warp session
	}
	Config = Cfg;
	bConfigured = true;
}

void FThermalFrameBuilder::Build(const FThermalFrameInputs& In, FThermalFrameParams& Out, TArray<FString>* OutWarnings)
{
	check(bConfigured);
	auto Warn = [OutWarnings](FString W) { if (OutWarnings) OutWarnings->Add(MoveTemp(W)); };
	for (FString& W : PendingWarnings) Warn(MoveTemp(W));
	PendingWarnings.Reset();

	// Sanitise (ruling R4): non-finite inputs fall back to the defaults, and cloud (0.01) / air temperature (0.05 K)
	// are quantised so FThermalModel's exact-compare cache does not refit every frame when the inputs jitter.
	const FThermalFrameInputs Defaults;
	const double AirC     = FMath::IsFinite(In.AirTempC)     ? In.AirTempC     : Defaults.AirTempC;
	const double CloudIn  = FMath::IsFinite(In.CloudCover01) ? In.CloudCover01 : Defaults.CloudCover01;
	const double WaterC   = FMath::IsFinite(In.WaterTempC)   ? In.WaterTempC   : Defaults.WaterTempC;
	const double VisM     = FMath::IsFinite(In.VisibilityM)  ? In.VisibilityM  : Defaults.VisibilityM;
	const double AirMeanK = FMath::FloorToDouble((FMath::Clamp(AirC, -80.0, 70.0) + 273.15) / AirQuantumK + 0.5) * AirQuantumK;
	const double Cloud    = FMath::FloorToDouble(FMath::Clamp(CloudIn, 0.0, 1.0) / CloudQuantum + 0.5) * CloudQuantum;

	// Site and time (local solar).
	const double LocalSec = LocalSolarSeconds(In.UtcMicros, In.CamLonDeg);
	const double Hour = LocalSec / 3600.0;
	const FDateTime Date = LocalSolarDate(In.UtcMicros, In.CamLonDeg);
	FThermalSite Site;
	Site.Year      = Date.GetYear();
	Site.DayOfYear = Date.GetDayOfYear();
	Site.LatDeg    = In.CamLatDeg;
	Site.LonDeg    = In.CamLonDeg;
	Site.TairMeanK = AirMeanK;
	Site.AirSwingK = Config.AirDiurnalSwingK;
	Site.Cloud     = Cloud;
	Model.Update(Site, Materials);

	const double TairK  = FThermalModel::AirTemperatureK(Site, Hour);
	const double SunEl  = FThermalModel::SunElevationDeg(Site, Hour);
	const double SClear = FThermalModel::ClearSkyGhi(SunEl);
	const double SRef0  = SClear * FThermalModel::CloudFactor(Site.Cloud);   // what a horizontal class surface absorbs / (1 - a)
	const double WaterK = FMath::Clamp(WaterC, -2.0, 60.0) + 273.15;

	// LUT and classes.
	FMemory::Memcpy(Out.LogLut, Band.GetLogLut(), sizeof(Out.LogLut));
	Out.NumClasses = static_cast<uint32>(Materials.Num());
	for (int32 C = 0; C < Materials.Num(); ++C)
	{
		const FThermalMaterial& M = Materials.Get(C);
		Out.ClassTempK[C]      = static_cast<float>(Model.TemperatureK(C, LocalSec, WaterK));
		Out.ClassEmissivity[C] = M.Emissivity;
		Out.ClassKFast[C]      = M.KFast;
		Out.ClassSAbsRef[C]    = static_cast<float>((1.0 - M.Albedo) * SRef0);
	}
	Out.TerrainClass = FThermalMaterialTable::TerrainDefault;
	Out.WaterClass   = FThermalMaterialTable::Water;

	// Stencil table: rebuilt from the live entities every frame (a released stencil reverts to the default).
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S)
	{
		Out.StencilClass[S]   = static_cast<uint8>(FThermalMaterialTable::VehiclePaint);
		Out.StencilOffsetK[S] = 0.0f;
	}
	for (const FThermalStencilEntity& E : In.Entities)
	{
		if (E.Stencil == 0) continue;
		int32 Class = FThermalMaterialTable::VehiclePaint;
		if (!E.ThermalMaterial.IsEmpty())
		{
			Class = Materials.Find(E.ThermalMaterial);
			if (Class == INDEX_NONE)
			{
				Class = FThermalMaterialTable::VehiclePaint;
				if (!WarnedMaterials.Contains(E.ThermalMaterial))
				{
					WarnedMaterials.Add(E.ThermalMaterial);
					Warn(FString::Printf(TEXT("entity_types thermal_material '%s' is not a thermal class; using vehicle_paint (warned once)"), *E.ThermalMaterial));
				}
			}
		}
		Out.StencilClass[E.Stencil]   = static_cast<uint8>(Class);
		Out.StencilOffsetK[E.Stencil] = E.ThermalOffsetK.IsSet() ? *E.ThermalOffsetK : (E.bSurfaceVehicle ? DefaultVehicleOffsetK : 0.0f);
	}
	Out.EntityDepthRatio = 0.99f;

	// Sky and atmosphere.
	Out.TairK           = static_cast<float>(TairK);
	Out.SkyEpsZ         = static_cast<float>(FThermalSky::ZenithEmissivity(TairK));
	Out.Cloud           = static_cast<float>(Site.Cloud);
	Out.SkyHemiRadiance = static_cast<float>(FThermalSky::HemisphereBandRadiance(Band, TairK, Site.Cloud));
	double BetaKm = IsMwir() ? Config.ExtinctionPerKmMwir : Config.ExtinctionPerKmLwir;
	if (In.bFogActive)
	{
		BetaKm += FogVisibilityK / FMath::Max(VisM / 1000.0, 0.01) * Config.FogIrFactor;
	}
	Out.BetaPerCm = static_cast<float>(BetaKm / 1e5);

	// Solar fast term: K_lum maps the sun light's horizontal illuminance onto the class model's reference flux.
	const double SinEl = FMath::Max(FMath::Sin(FMath::DegreesToRadians(SunEl)), 0.0);
	const double SunLuxHoriz = FMath::Max(In.SunIlluminanceLux, 0.0) * SinEl;
	Out.KLum       = static_cast<float>(SRef0 > 1.0 ? FMath::Max(SunLuxHoriz, 1e-3) / SRef0 : 1.0);
	Out.EClampWm2  = static_cast<float>(1.5 * SClear);
	Out.KFastScale = (In.bBaseColorAvailable && In.SunIlluminanceLux > 0.0) ? 1.0f : 0.0f;
	Out.bBaseColorSrgb = In.bBaseColorSrgb ? 1u : 0u;

	// Geometry (the render thread sets ClipToTranslatedWorld).
	Out.Up          = FVector3f(In.UpWorld.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
	Out.bWater      = In.bHasSea ? 1u : 0u;
	Out.CamHeightCm = In.bHasSea ? static_cast<float>((In.CamAltHaeM - In.SeaLevelHaeM) * 100.0) : 0.0f;
	Out.SeaRadiusCm = static_cast<float>(GaussianRadiusM(In.CamLatDeg) * 100.0);
	Out.WaterBandCm = static_cast<float>((WaterBandBaseM + FMath::Max(In.MaxWaveAmplitudeM, 0.0)) * 100.0);
	Out.InputScale  = 1.0f;

	// Land cover (ROADMAP 4B). The tables and thresholds are filled every frame; without an enabled, valid window the mapping
	// fields are reset to their defaults and bLandCover = 0, so ThermalCS runs 4A's terrain path.
	FMemory::Memcpy(Out.LandCoverClass, LandCoverTable.Class, sizeof(Out.LandCoverClass));
	FMemory::Memcpy(Out.LandCoverFamily, LandCoverTable.Family, sizeof(Out.LandCoverFamily));
	Out.VegetationClass  = FThermalMaterialTable::Vegetation;
	Out.BareSoilClass    = FThermalMaterialTable::BareSoil;
	Out.AsphaltClass     = FThermalMaterialTable::Asphalt;
	Out.ConcreteClass    = FThermalMaterialTable::Concrete;
	Out.VegIndexLo       = Config.LandCover.VegIndexLo;
	Out.VegIndexHi       = Config.LandCover.VegIndexHi;
	Out.AsphaltMaxLuma   = Config.LandCover.AsphaltMaxLuma;
	Out.AsphaltRampLuma  = AsphaltRampLuma;
	Out.VegBlurM         = FMath::IsFinite(Config.LandCover.VegBlurM) ? FMath::Clamp(Config.LandCover.VegBlurM, 0.0f, MaxVegBlurM) : 0.0f;
	Out.bLandCoverRefine = In.bBaseColorAvailable ? 1u : 0u;   // not the fast term: base colour is valid at night too
	const FThermalLandCoverInput& L = In.LandCover;
	if (Config.LandCover.bEnabled && L.bValid && L.WindowId != 0u && L.Texels >= 2 && L.TexelM > 0.0f
		&& CamSimLandCover::IsWindowAllowed(L.CentreLatDeg))
	{
		CamSimLandCover::FWindowSpec Spec;
		Spec.CentreLatDeg = L.CentreLatDeg;
		Spec.CentreLonDeg = L.CentreLonDeg;
		Spec.Texels = L.Texels;
		Spec.TexelM = L.TexelM;
		const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(Spec, In.CamLatDeg, In.CamLonDeg);   // doubles
		Out.bLandCover          = 1u;
		Out.LandCoverWindowId   = L.WindowId;
		Out.LandCoverEast       = FVector3f(L.EastWorld);
		Out.LandCoverNorth      = FVector3f(L.NorthWorld);
		Out.LandCoverCamOffsetM = FVector2f(static_cast<float>(Off.X), static_cast<float>(Off.Y));
		Out.LandCoverTexelM     = L.TexelM;
		Out.LandCoverTexels     = static_cast<uint32>(L.Texels);

		// Geo-anchored warp (Task 13). Ground coordinates G = anchor offset + scale * (E, N) equal
		// GeodeticToWindowEN(session anchor, point) exactly (both mappings are linear in lat/lon), so the warp pattern is fixed
		// to the ground across re-centres; doubles here, floats in the shader.
		CamSimLandCover::FWindowSpec Anchor = Spec;
		Anchor.CentreLatDeg = WarpAnchorLatDeg;
		Anchor.CentreLonDeg = WarpAnchorLonDeg;
		FVector2D AnchorM = bWarpAnchor ? CamSimLandCover::GeodeticToWindowEN(Anchor, L.CentreLatDeg, L.CentreLonDeg) : FVector2D::ZeroVector;
		if (!bWarpAnchor || !FMath::IsFinite(AnchorM.X) || !FMath::IsFinite(AnchorM.Y)
			|| FMath::Abs(AnchorM.X) > MaxWarpAnchorM || FMath::Abs(AnchorM.Y) > MaxWarpAnchorM)
		{
			bWarpAnchor = true;   // the session's first window (or one too far from the anchor): it becomes the anchor
			WarpAnchorLatDeg = Anchor.CentreLatDeg = L.CentreLatDeg;
			WarpAnchorLonDeg = Anchor.CentreLonDeg = L.CentreLonDeg;
			AnchorM = FVector2D::ZeroVector;
		}
		const double CosA = FMath::Cos(FMath::DegreesToRadians(WarpAnchorLatDeg));
		const double CosC = FMath::Cos(FMath::DegreesToRadians(L.CentreLatDeg));
		const double ScaleE = (CamSimLandCover::PrimeVerticalRadiusM(WarpAnchorLatDeg) * CosA) / (CamSimLandCover::PrimeVerticalRadiusM(L.CentreLatDeg) * CosC);
		const double ScaleN = CamSimLandCover::MeridionalRadiusM(WarpAnchorLatDeg) / CamSimLandCover::MeridionalRadiusM(L.CentreLatDeg);
		const float Amp  = Config.LandCover.WarpAmplitudeM;
		const float Cell = Config.LandCover.WarpCellM;
		Out.LandCoverAnchorM     = FVector2f(static_cast<float>(AnchorM.X), static_cast<float>(AnchorM.Y));
		Out.LandCoverAnchorScale = FVector2f(static_cast<float>(ScaleE), static_cast<float>(ScaleN));
		Out.LandCoverWarpAmpM    = FMath::IsFinite(Amp) ? FMath::Clamp(Amp, 0.0f, MaxWarpAmpM) : 0.0f;
		Out.LandCoverWarpCellM   = FMath::IsFinite(Cell) ? FMath::Clamp(Cell, MinWarpCellM, MaxWarpCellM) : DefaultWarpCellM;
	}
	else
	{
		static const FThermalFrameParams LandCoverOff;
		Out.bLandCover          = 0u;
		Out.LandCoverWindowId   = 0u;
		Out.LandCoverEast       = LandCoverOff.LandCoverEast;
		Out.LandCoverNorth      = LandCoverOff.LandCoverNorth;
		Out.LandCoverCamOffsetM = LandCoverOff.LandCoverCamOffsetM;
		Out.LandCoverTexelM     = LandCoverOff.LandCoverTexelM;
		Out.LandCoverTexels     = LandCoverOff.LandCoverTexels;
		Out.LandCoverAnchorM     = LandCoverOff.LandCoverAnchorM;
		Out.LandCoverAnchorScale = LandCoverOff.LandCoverAnchorScale;
		Out.LandCoverWarpAmpM    = LandCoverOff.LandCoverWarpAmpM;
		Out.LandCoverWarpCellM   = LandCoverOff.LandCoverWarpCellM;
	}
}
