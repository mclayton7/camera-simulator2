// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalReference.h"

namespace CamSimThermalRef
{
	namespace
	{
		constexpr float PiF = 3.14159265f;   // THERMAL_PI in CamSimThermalCommon.ush

		float Lum709(const FVector3f& C) { return 0.2126f * C.X + 0.7152f * C.Y + 0.0722f * C.Z; }

		FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z)
		{
			const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(Nx, Ny, Z, 1.0f));
			return FVector3f(H.X / H.W, H.Y / H.W, H.Z / H.W);
		}
	}

	float LutRadiance(const FThermalFrameParams& P, float TK) { return ThermalLutRadiance(P.LogLut, TK); }

	float SkyTemperatureK(const FThermalFrameParams& P, float SinEl)
	{
		const float S = FMath::Max(SinEl, 0.05f);
		const float Eps = 1.0f - FMath::Pow(1.0f - P.SkyEpsZ, 1.0f / S);
		const float Ratio = (1.0f - P.Cloud) * Eps + P.Cloud;
		return P.TairK * FMath::Sqrt(FMath::Sqrt(Ratio));
	}

	float SrgbToLinear(float C)
	{
		return C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
	}

	FPixelResult EvaluatePixel(const FThermalFrameParams& P, const FPixelSample& S)
	{
		FPixelResult R;
		const float BAir = LutRadiance(P, P.TairK);
		if (!FMath::IsFinite(S.DeviceZ) || !FMath::IsFinite(S.Color.X) || !FMath::IsFinite(S.Color.Y) || !FMath::IsFinite(S.Color.Z))
		{
			R.Class = EPixelClass::Invalid;
			R.TempK = P.TairK;
			R.Radiance = BAir;
			return R;
		}
		const float Nx = S.U * 2.0f - 1.0f, Ny = 1.0f - S.V * 2.0f;
		if (S.DeviceZ <= 0.0f)
		{
			const FVector3f Pn = ClipToWorld(P, Nx, Ny, 1.0f);
			const float Len = FMath::Sqrt(FVector3f::DotProduct(Pn, Pn));
			const float SinEl = (Len > 0.0f) ? FVector3f::DotProduct(Pn, P.Up) / Len : 1.0f;
			R.Class = EPixelClass::Sky;
			R.TempK = SkyTemperatureK(P, SinEl);
			R.Radiance = LutRadiance(P, R.TempK);
			return R;
		}
		const FVector3f Pw = ClipToWorld(P, Nx, Ny, S.DeviceZ);
		const float Range = FMath::Sqrt(FVector3f::DotProduct(Pw, Pw));
		uint32 Class = P.TerrainClass;
		float Offset = 0.0f;
		R.Class = EPixelClass::Terrain;
		const uint32 Stencil = S.Stencil & 0xFFu;
		if (Stencil > 0u && S.CustomZ >= S.DeviceZ * P.EntityDepthRatio)
		{
			Class = P.StencilClass[Stencil];
			Offset = P.StencilOffsetK[Stencil];
			R.Class = EPixelClass::Entity;
		}
		else if (P.bWater != 0u)
		{
			const float Vert = FVector3f::DotProduct(Pw, P.Up);
			const float D2 = FMath::Max(Range * Range - Vert * Vert, 0.0f);
			const float Height = P.CamHeightCm + Vert + D2 / (2.0f * P.SeaRadiusCm);
			if (Height < P.WaterBandCm)
			{
				Class = P.WaterClass;
				R.Class = EPixelClass::Water;
			}
		}
		Class = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		float T = P.ClassTempK[Class] + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const FVector3f Base = (P.bBaseColorSrgb != 0u)
				? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(PiF * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * P.ClassKFast[Class] * (SAbs - P.ClassSAbsRef[Class]);
		}
		const float Eps = P.ClassEmissivity[Class];
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		R.TempK = T;
		R.Radiance = Tau * LSurf + (1.0f - Tau) * BAir;
		return R;
	}

	TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P)
	{
		check(In.SceneColor && In.SceneDepth && In.CustomDepth && In.Stencil);
		check(In.SceneColor->Num() == In.W * In.H);
		check(In.SceneDepth->Num() == In.DepthW * In.DepthH && In.CustomDepth->Num() == In.DepthW * In.DepthH && In.Stencil->Num() == In.DepthW * In.DepthH);
		check(!In.BaseColor || In.BaseColor->Num() == In.DepthW * In.DepthH);
		TArray<FPixelResult> Out;
		Out.SetNum(In.W * In.H);
		for (int32 Y = 0; Y < In.H; ++Y)
		{
			for (int32 X = 0; X < In.W; ++X)
			{
				FPixelSample S;
				S.U = (static_cast<float>(X) + 0.5f) / static_cast<float>(In.W);
				S.V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(In.H);
				const int32 Tx = FMath::Clamp(FMath::FloorToInt32(S.U * static_cast<float>(In.DepthW)), 0, In.DepthW - 1);
				const int32 Ty = FMath::Clamp(FMath::FloorToInt32(S.V * static_cast<float>(In.DepthH)), 0, In.DepthH - 1);
				const int32 Ti = Ty * In.DepthW + Tx;
				S.DeviceZ = (*In.SceneDepth)[Ti];
				S.CustomZ = (*In.CustomDepth)[Ti];
				S.Stencil = (*In.Stencil)[Ti];
				const FLinearColor& C = (*In.SceneColor)[Y * In.W + X];
				S.Color = FVector3f(C.R, C.G, C.B) * P.InputScale;
				if (In.BaseColor)
				{
					const FLinearColor& B = (*In.BaseColor)[Ti];
					S.Base = FVector3f(B.R, B.G, B.B);
					S.bHasBase = true;
				}
				Out[Y * In.W + X] = EvaluatePixel(P, S);
			}
		}
		return Out;
	}

	FMatrix44f MakeClipToTranslatedWorld(const FRotator& ViewRotation, float HFovDeg, int32 W, int32 H, float NearCm)
	{
		// View space: X right, Y up, Z forward (the axis swap UE applies after the inverse view rotation).
		const FMatrix ViewRot = FInverseRotationMatrix(ViewRotation) * FMatrix(
			FPlane(0.0, 0.0, 1.0, 0.0), FPlane(1.0, 0.0, 0.0, 0.0), FPlane(0.0, 1.0, 0.0, 0.0), FPlane(0.0, 0.0, 0.0, 1.0));
		const FMatrix Proj = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(0.5 * HFovDeg), double(W), double(H), double(NearCm));
		return FMatrix44f((ViewRot * Proj).Inverse());
	}
}
