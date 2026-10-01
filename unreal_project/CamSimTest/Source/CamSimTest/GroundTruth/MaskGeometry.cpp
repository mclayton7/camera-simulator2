// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/MaskGeometry.h"

namespace CamSimMask
{
	namespace
	{
		double Cross(const FVector2D& O, const FVector2D& A, const FVector2D& B)
		{
			return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
		}

		double FoldAngle(double Deg, bool bSquare)
		{
			const double Period = bSquare ? 90.0 : 180.0, Lo = bSquare ? -45.0 : -90.0;
			while (Deg >= Lo + Period) Deg -= Period;
			while (Deg < Lo) Deg += Period;
			return Deg;
		}
	}

	TArray<FVector2D> ConvexHull(TArray<FVector2D> P)
	{
		P.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });
		TArray<FVector2D> U;
		for (const FVector2D& Q : P) { if (U.Num() == 0 || U.Last() != Q) U.Add(Q); }
		if (U.Num() < 3) return U;
		TArray<FVector2D> H; H.SetNum(2 * U.Num());
		int32 K = 0;
		for (int32 I = 0; I < U.Num(); ++I)
		{
			while (K >= 2 && Cross(H[K - 2], H[K - 1], U[I]) <= 0) --K;
			H[K++] = U[I];
		}
		for (int32 I = U.Num() - 2, T = K + 1; I >= 0; --I)
		{
			while (K >= T && Cross(H[K - 2], H[K - 1], U[I]) <= 0) --K;
			H[K++] = U[I];
		}
		H.SetNum(K - 1);
		return H;
	}

	namespace
	{
		/** The rectangle enclosing Hull whose first axis is the unit vector U, normalised (w >= h, angle folded). */
		FOrientedBox RectForUnitAxis(const TArray<FVector2D>& Hull, const FVector2D& U, double& OutArea)
		{
			const FVector2D V(-U.Y, U.X);
			double MinU = TNumericLimits<double>::Max(), MaxU = -MinU, MinV = MinU, MaxV = -MinU;
			for (const FVector2D& P : Hull)
			{
				const double Pu = FVector2D::DotProduct(P, U), Pv = FVector2D::DotProduct(P, V);
				MinU = FMath::Min(MinU, Pu); MaxU = FMath::Max(MaxU, Pu);
				MinV = FMath::Min(MinV, Pv); MaxV = FMath::Max(MaxV, Pv);
			}
			FOrientedBox B;
			double Wu = MaxU - MinU, Hv = MaxV - MinV;
			const double Cu = 0.5 * (MinU + MaxU), Cv = 0.5 * (MinV + MaxV);
			B.Cx = Cu * U.X + Cv * V.X;
			B.Cy = Cu * U.Y + Cv * V.Y;
			double Angle = FMath::RadiansToDegrees(FMath::Atan2(U.Y, U.X));
			if (Wu < Hv) { Swap(Wu, Hv); Angle += 90.0; }
			B.W = Wu; B.H = Hv;
			B.AngleDeg = FoldAngle(Angle, FMath::IsNearlyEqual(Wu, Hv, 1e-9));
			OutArea = Wu * Hv;
			return B;
		}
	}

	FOrientedBox MinAreaRect(const TArray<FVector2D>& Hull)
	{
		FOrientedBox Best;
		if (Hull.Num() == 0) return Best;
		if (Hull.Num() == 1) { Best.Cx = Hull[0].X; Best.Cy = Hull[0].Y; return Best; }
		double BestArea = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Hull.Num(); ++I)
		{
			const FVector2D E = Hull[(I + 1) % Hull.Num()] - Hull[I];
			const double Len = E.Size();
			if (Len <= 0.0) continue;
			double Area = 0.0;
			const FOrientedBox B = RectForUnitAxis(Hull, E / Len, Area);
			if (Area < BestArea - 1e-9 || (FMath::Abs(Area - BestArea) <= 1e-9 && FMath::Abs(B.AngleDeg) < FMath::Abs(Best.AngleDeg)))
			{
				BestArea = Area;
				Best = B;
			}
		}
		return Best;
	}

	FOrientedBox RectAlongAxis(const TArray<FVector2D>& Hull, const FVector2D& Axis)
	{
		const double Len = Axis.Size();
		if (Hull.Num() == 0 || !(Len > 0.0) || !FMath::IsFinite(Len)) return MinAreaRect(Hull);
		if (Hull.Num() == 1) { FOrientedBox B; B.Cx = Hull[0].X; B.Cy = Hull[0].Y; return B; }
		double Area = 0.0;
		return RectForUnitAxis(Hull, Axis / Len, Area);
	}

	double PolygonArea(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		for (int32 I = 0; I < P.Num(); ++I)
		{
			const FVector2D& Q = P[I]; const FVector2D& R = P[(I + 1) % P.Num()];
			A += Q.X * R.Y - R.X * Q.Y;
		}
		return FMath::Abs(0.5 * A);
	}

	TArray<FVector2D> ClipToRect(const TArray<FVector2D>& Poly, const FBox2D& Rect)
	{
		// Edges: x >= Min.X, x <= Max.X, y >= Min.Y, y <= Max.Y
		auto Inside = [&](const FVector2D& P, int32 E)
		{
			switch (E) { case 0: return P.X >= Rect.Min.X; case 1: return P.X <= Rect.Max.X; case 2: return P.Y >= Rect.Min.Y; default: return P.Y <= Rect.Max.Y; }
		};
		auto Hit = [&](const FVector2D& A, const FVector2D& B, int32 E)
		{
			const double T = (E < 2)
				? ((E == 0 ? Rect.Min.X : Rect.Max.X) - A.X) / (B.X - A.X)
				: ((E == 2 ? Rect.Min.Y : Rect.Max.Y) - A.Y) / (B.Y - A.Y);
			return A + (B - A) * T;
		};
		TArray<FVector2D> Out = Poly;
		for (int32 E = 0; E < 4 && Out.Num() > 0; ++E)
		{
			TArray<FVector2D> In = MoveTemp(Out);
			Out.Reset();
			for (int32 I = 0; I < In.Num(); ++I)
			{
				const FVector2D& Cur = In[I]; const FVector2D& Prev = In[(I + In.Num() - 1) % In.Num()];
				const bool bCur = Inside(Cur, E), bPrev = Inside(Prev, E);
				if (bCur) { if (!bPrev) Out.Add(Hit(Prev, Cur, E)); Out.Add(Cur); }
				else if (bPrev) { Out.Add(Hit(Prev, Cur, E)); }
			}
		}
		return Out;
	}

	FString EncodeCocoRle(const TArray<uint32>& Runs)
	{
		FString S;
		for (int32 I = 0; I < Runs.Num(); ++I)
		{
			int64 X = Runs[I];
			if (I > 2) X -= static_cast<int64>(Runs[I - 2]);
			bool bMore = true;
			while (bMore)
			{
				int64 C = X & 0x1f;
				X >>= 5;  // arithmetic shift: int64 is signed
				bMore = (C & 0x10) ? X != -1 : X != 0;
				if (bMore) C |= 0x20;
				S.AppendChar(static_cast<TCHAR>(C + 48));
			}
		}
		return S;
	}
}
