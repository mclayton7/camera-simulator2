// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalReference.h"

#include "Thermal/LandCoverClasses.h"
#include "SensorHash.h"

static_assert(static_cast<uint8>(ELandCoverFamily::Vegetation) == FThermalFrameParams::LandCoverFamilyVegetation
	&& static_cast<uint8>(ELandCoverFamily::BuiltUp) == FThermalFrameParams::LandCoverFamilyBuiltUp
	&& static_cast<uint8>(ELandCoverFamily::Bare) == FThermalFrameParams::LandCoverFamilyBare
	&& static_cast<uint8>(ELandCoverFamily::None) == FThermalFrameParams::LandCoverFamilyNone,
	"land-cover families: ELandCoverFamily, FThermalFrameParams and LANDCOVER_FAMILY_* in CamSimThermalCommon.ush must agree");

namespace CamSimThermalRef
{
	namespace
	{
		constexpr float PiF = 3.14159265f;   // THERMAL_PI in CamSimThermalCommon.ush
		constexpr float WarpLatticeLimit = 4194304.0f;   // 2^22 cells: the int casts below stay defined (LANDCOVER_WARP_LATTICE_LIMIT)

		/** A corner hash's 16-bit half -> [-1, 1] (k / 32767.5 - 1; exact inputs). */
		float WarpCorner(uint32 Bits16) { return static_cast<float>(Bits16) / 32767.5f - 1.0f; }
	}

	float Lum709(const FVector3f& C) { return 0.2126f * C.X + 0.7152f * C.Y + 0.0722f * C.Z; }

	FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z)
	{
		const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(Nx, Ny, Z, 1.0f));
		return FVector3f(H.X / H.W, H.Y / H.W, H.Z / H.W);
	}

	FVector4f ClassData(const FThermalFrameParams& P, uint32 Class)
	{
		const uint32 C = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		return FVector4f(P.ClassTempK[C], P.ClassEmissivity[C], P.ClassKFast[C], P.ClassSAbsRef[C]);
	}

	FVector2f LandCoverWarpNoise(const FThermalFrameParams& P, float Gx, float Gy)
	{
		const float Cell = FMath::Max(P.LandCoverWarpCellM, 1.0f);
		const float Cx = FMath::Clamp(Gx / Cell, -WarpLatticeLimit, WarpLatticeLimit);
		const float Cy = FMath::Clamp(Gy / Cell, -WarpLatticeLimit, WarpLatticeLimit);
		const float Ix = FMath::FloorToFloat(Cx), Iy = FMath::FloorToFloat(Cy);
		const float Fx = Cx - Ix, Fy = Cy - Iy;
		const float Sx = Fx * Fx * (3.0f - 2.0f * Fx), Sy = Fy * Fy * (3.0f - 2.0f * Fy);
		const uint32 I = static_cast<uint32>(static_cast<int32>(Ix)), J = static_cast<uint32>(static_cast<int32>(Iy));
		const uint32 Key = CamSimHash::StreamKey(CamSimHash::FixedFrame, 0u, 2u * CamSimHash::LandCoverWarpStream);
		const uint32 KJ0 = CamSimHash::Pcg(J ^ Key), KJ1 = CamSimHash::Pcg((J + 1u) ^ Key);   // Hash(i, j, ...) row halves
		const uint32 H00 = CamSimHash::Pcg(I ^ KJ0), H10 = CamSimHash::Pcg((I + 1u) ^ KJ0);
		const uint32 H01 = CamSimHash::Pcg(I ^ KJ1), H11 = CamSimHash::Pcg((I + 1u) ^ KJ1);
		const float N1 = (WarpCorner(H00 >> 16u) * (1.0f - Sx) + WarpCorner(H10 >> 16u) * Sx) * (1.0f - Sy)
			+ (WarpCorner(H01 >> 16u) * (1.0f - Sx) + WarpCorner(H11 >> 16u) * Sx) * Sy;
		const float N2 = (WarpCorner(H00 & 0xFFFFu) * (1.0f - Sx) + WarpCorner(H10 & 0xFFFFu) * Sx) * (1.0f - Sy)
			+ (WarpCorner(H01 & 0xFFFFu) * (1.0f - Sx) + WarpCorner(H11 & 0xFFFFu) * Sx) * Sy;
		return FVector2f(N1, N2);
	}

	FVector2f WarpLandCoverEN(const FThermalFrameParams& P, float E, float N)
	{
		const float Amp = P.LandCoverWarpAmpM;
		if (!(FMath::IsFinite(Amp) && Amp > 0.0f)) return FVector2f(E, N);
		const float Gx = P.LandCoverAnchorM.X + P.LandCoverAnchorScale.X * E;
		const float Gy = P.LandCoverAnchorM.Y + P.LandCoverAnchorScale.Y * N;
		if (!FMath::IsFinite(Gx) || !FMath::IsFinite(Gy)) return FVector2f(Gx, Gy);
		const FVector2f Nz = LandCoverWarpNoise(P, Gx, Gy);
		return FVector2f(E + Amp * Nz.X, N + Amp * Nz.Y);
	}

	FLandCoverSample SampleLandCover(const FThermalFrameParams& P, const uint8* Codes, const FVector3f& Pw)
	{
		FLandCoverSample S;
		if (!Codes || P.bLandCover == 0u || P.LandCoverTexels < 2u) return S;
		const float E = FVector3f::DotProduct(Pw, P.LandCoverEast) / 100.0f + P.LandCoverCamOffsetM.X;
		const float N = FVector3f::DotProduct(Pw, P.LandCoverNorth) / 100.0f + P.LandCoverCamOffsetM.Y;
		const FVector2f Wp = WarpLandCoverEN(P, E, N);
		const float Half = static_cast<float>(P.LandCoverTexels) * 0.5f;
		const float X = Wp.X / P.LandCoverTexelM + Half - 0.5f;
		const float Y = Half - Wp.Y / P.LandCoverTexelM - 0.5f;
		const float MaxI = static_cast<float>(P.LandCoverTexels) - 1.0f;
		// Explicit non-finite test first (HLSL: asuint bit test, since Metal fast-math may fold NaN comparisons); then the
		// texel-centre grid (the comparisons also reject NaN on the CPU).
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y)) return S;
		if (!(X >= 0.0f && X <= MaxI && Y >= 0.0f && Y <= MaxI)) return S;
		const float X0 = FMath::Min(FMath::FloorToFloat(X), MaxI - 1.0f);
		const float Y0 = FMath::Min(FMath::FloorToFloat(Y), MaxI - 1.0f);
		const float Fx = X - X0, Fy = Y - Y0;
		const float Sx = Fx * Fx * (3.0f - 2.0f * Fx), Sy = Fy * Fy * (3.0f - 2.0f * Fy);   // smoothstep: C1 at texel centres
		const int32 I = static_cast<int32>(X0), J = static_cast<int32>(Y0), W = static_cast<int32>(P.LandCoverTexels);
		S.bInside = true;
		S.Codes[0] = Codes[J * W + I];
		S.Codes[1] = Codes[J * W + I + 1];
		S.Codes[2] = Codes[(J + 1) * W + I];
		S.Codes[3] = Codes[(J + 1) * W + I + 1];
		S.Weights[0] = (1.0f - Sx) * (1.0f - Sy);
		S.Weights[1] = Sx * (1.0f - Sy);
		S.Weights[2] = (1.0f - Sx) * Sy;
		S.Weights[3] = Sx * Sy;
		return S;
	}

	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base)
	{
		return RefinementWeights(P, Base, Base);
	}

	bool IsVegBlurOn(const FThermalFrameParams& P)
	{
		return FMath::IsFinite(P.VegBlurM) && P.VegBlurM > 0.0f && FMath::IsFinite(P.BaseTexelAngle) && P.BaseTexelAngle > 0.0f;
	}

	int32 VegBlurRadiusPx(const FThermalFrameParams& P, float RangeCm)
	{
		const float Rf = FMath::Clamp(P.VegBlurM * 100.0f / FMath::Max(RangeCm * P.BaseTexelAngle, 1e-6f), 1.0f, 32.0f);
		return static_cast<int32>(FMath::FloorToFloat(Rf + 0.5f));
	}

	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base, const FVector3f& VegBase)
	{
		const float Sum = VegBase.X + VegBase.Y + VegBase.Z;
		const float ExG = (2.0f * VegBase.Y - VegBase.X - VegBase.Z) / (Sum + 1e-4f);
		const float Veg = FMath::Clamp((ExG - P.VegIndexLo) / FMath::Max(P.VegIndexHi - P.VegIndexLo, 1e-4f), 0.0f, 1.0f);
		const float Concrete = FMath::Clamp((Lum709(Base) - P.AsphaltMaxLuma) / FMath::Max(P.AsphaltRampLuma, 1e-4f) + 0.5f, 0.0f, 1.0f);
		return FVector2f(Veg, Concrete);
	}

	FVector4f RefinedClassData(const FThermalFrameParams& P, uint8 Code, const FVector2f& Weights, bool bRefine)
	{
		const FVector4f Own = ClassData(P, P.LandCoverClass[Code]);
		if (!bRefine) return Own;
		const uint8 Family = P.LandCoverFamily[Code];
		const float V = Weights.X, C = Weights.Y;
		if (Family == FThermalFrameParams::LandCoverFamilyVegetation)
		{
			return Own * V + ClassData(P, P.BareSoilClass) * (1.0f - V);
		}
		if (Family == FThermalFrameParams::LandCoverFamilyBuiltUp)
		{
			return ClassData(P, P.VegetationClass) * V
				+ (ClassData(P, P.AsphaltClass) * (1.0f - C) + ClassData(P, P.ConcreteClass) * C) * (1.0f - V);
		}
		if (Family == FThermalFrameParams::LandCoverFamilyBare)
		{
			return ClassData(P, P.VegetationClass) * V + Own * (1.0f - V);
		}
		return Own;
	}

	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, bool bRefine)
	{
		return BlendLandCover(P, S, Base, Base, bRefine);
	}

	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, const FVector3f& VegBase, bool bRefine)
	{
		const FVector2f Wt = bRefine ? RefinementWeights(P, Base, VegBase) : FVector2f::ZeroVector;
		return RefinedClassData(P, S.Codes[0], Wt, bRefine) * S.Weights[0] + RefinedClassData(P, S.Codes[1], Wt, bRefine) * S.Weights[1]
			+ RefinedClassData(P, S.Codes[2], Wt, bRefine) * S.Weights[2] + RefinedClassData(P, S.Codes[3], Wt, bRefine) * S.Weights[3];
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
		bool bEntityRecord = false;   // ROADMAP 4C: T from EntityPartTemp instead of T_class + offset
		if (Stencil > 0u && S.CustomZ >= S.DeviceZ * P.EntityDepthRatio)
		{
			const FVector4f& R3 = P.EntityRecords[Stencil * FThermalFrameParams::EntityRecordFloat4s + 3];
			if (R3.W != 0.0f)
			{
				Class = static_cast<uint32>(R3.Y);
				bEntityRecord = true;
			}
			else
			{
				Class = P.StencilClass[Stencil];
				Offset = P.StencilOffsetK[Stencil];
			}
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
		const FVector3f Base = (P.bBaseColorSrgb != 0u)
			? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
		FVector4f Cd = ClassData(P, Class);
		if (R.Class == EPixelClass::Terrain)
		{
			const FLandCoverSample L = SampleLandCover(P, S.LandCover, Pw);
			if (L.bInside)
			{
				const bool bRefine = S.bHasBase && P.bLandCoverRefine != 0u;
				FVector3f VegBase = Base;
				if (bRefine && S.bBaseTaps && IsVegBlurOn(P))
				{
					FVector3f T[4];
					for (int32 K = 0; K < 4; ++K)
					{
						const FVector3f& B = S.BaseTaps[K];
						T[K] = (P.bBaseColorSrgb != 0u) ? FVector3f(SrgbToLinear(B.X), SrgbToLinear(B.Y), SrgbToLinear(B.Z)) : B;
					}
					VegBase = ((((Base + T[0]) + T[1]) + T[2]) + T[3]) * 0.2f;
				}
				Cd = BlendLandCover(P, L, Base, VegBase, bRefine);
				R.bLandCover = true;
			}
		}
		float T = bEntityRecord ? EntityPartTemp(P, Stencil, Pw) : Cd.X + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(PiF * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * Cd.Z * (SAbs - Cd.W);
		}
		const float Eps = Cd.Y;
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		R.TempK = T;
		R.Radiance = Tau * LSurf + (1.0f - Tau) * BAir;
		return R;
	}

	float EntityPartTemp(const FThermalFrameParams& P, uint32 Stencil, const FVector3f& Pw)
	{
		const FVector4f* R = &P.EntityRecords[(Stencil & 0xFFu) * FThermalFrameParams::EntityRecordFloat4s];
		const FVector3f Pb(
			R[0].X * Pw.X + R[0].Y * Pw.Y + R[0].Z * Pw.Z + R[0].W,
			R[1].X * Pw.X + R[1].Y * Pw.Y + R[1].Z * Pw.Z + R[1].W,
			R[2].X * Pw.X + R[2].Y * Pw.Y + R[2].Z * Pw.Z + R[2].W);
		float T = R[3].X;
		const int32 N = FMath::Min(static_cast<int32>(R[3].Z), FThermalFrameParams::MaxEntityParts);
		for (int32 K = 0; K < N; ++K)
		{
			const FVector4f A = R[4 + 3 * K];
			const FVector4f H = R[5 + 3 * K];
			const float Tk = R[6 + 3 * K].X;
			const FVector3f D(Pb.X - A.X, Pb.Y - A.Y, Pb.Z - A.Z);
			float Dist;
			if (A.W > 0.5f)
			{
				const FVector3f Q(D.X / H.X, D.Y / H.Y, D.Z / H.Z);
				Dist = (FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z) - 1.0f) * FMath::Min(H.X, FMath::Min(H.Y, H.Z));
			}
			else
			{
				const FVector3f E(FMath::Max(FMath::Abs(D.X) - H.X, 0.0f), FMath::Max(FMath::Abs(D.Y) - H.Y, 0.0f),
					FMath::Max(FMath::Abs(D.Z) - H.Z, 0.0f));
				Dist = FMath::Sqrt(E.X * E.X + E.Y * E.Y + E.Z * E.Z);
			}
			const float St = FMath::Clamp(Dist / H.W, 0.0f, 1.0f);
			const float W = 1.0f - St * St * (3.0f - 2.0f * St);
			T = T + (Tk - T) * W;
		}
		return T;
	}

	TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P)
	{
		check(In.SceneColor && In.SceneDepth && In.CustomDepth && In.Stencil);
		check(In.SceneColor->Num() == In.W * In.H);
		check(In.SceneDepth->Num() == In.DepthW * In.DepthH && In.CustomDepth->Num() == In.DepthW * In.DepthH && In.Stencil->Num() == In.DepthW * In.DepthH);
		check(!In.BaseColor || In.BaseColor->Num() == In.DepthW * In.DepthH);
		check(!In.LandCover || In.LandCover->Num() == static_cast<int32>(P.LandCoverTexels * P.LandCoverTexels));
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
					// Vegetation taps: the radius from the same range EvaluatePixel computes (ThermalCS: LoadVegBase).
					if (IsVegBlurOn(P) && FMath::IsFinite(S.DeviceZ) && S.DeviceZ > 0.0f)
					{
						const FVector3f Pw = ClipToWorld(P, S.U * 2.0f - 1.0f, 1.0f - S.V * 2.0f, S.DeviceZ);
						const int32 Rp = VegBlurRadiusPx(P, FMath::Sqrt(FVector3f::DotProduct(Pw, Pw)));
						const FIntPoint Off[4] = { FIntPoint(Rp, Rp), FIntPoint(-Rp, Rp), FIntPoint(Rp, -Rp), FIntPoint(-Rp, -Rp) };
						for (int32 K = 0; K < 4; ++K)
						{
							const int32 Qx = FMath::Clamp(Tx + Off[K].X, 0, In.DepthW - 1), Qy = FMath::Clamp(Ty + Off[K].Y, 0, In.DepthH - 1);
							const FLinearColor& Q = (*In.BaseColor)[Qy * In.DepthW + Qx];
							S.BaseTaps[K] = FVector3f(Q.R, Q.G, Q.B);
						}
						S.bBaseTaps = true;
					}
				}
				S.LandCover = In.LandCover ? In.LandCover->GetData() : nullptr;
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
