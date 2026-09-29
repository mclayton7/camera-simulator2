// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceClamp.h"

namespace CamSimSurface
{
	namespace
	{
		TOptional<double> Finite(const TOptional<double>& V)
		{
			return (V.IsSet() && FMath::IsFinite(*V)) ? V : TOptional<double>();
		}

		/**
		 * Exponential ease toward Target. bSnap: a change of SnapThresholdM or more jumps
		 * straight there (heights; teleports). DtSec <= 0 (a second commit in the same
		 * frame) keeps Prev, so two commits per frame don't bypass the ease.
		 */
		double Ease(double Prev, double Target, double DtSec, bool bSnap)
		{
			if (bSnap && FMath::Abs(Target - Prev) >= SnapThresholdM) return Target;
			if (DtSec <= 0.0) return Prev;
			return Prev + (Target - Prev) * (1.0 - FMath::Exp(-DtSec / EaseTimeConstantSec));
		}

		CamSimFrames::FGeoPose WithSurface(const CamSimFrames::FGeoPose& Sender, double Height, double PitchDeg, double RollDeg)
		{
			CamSimFrames::FGeoPose Out = Sender;
			Out.Alt = Height;
			Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, PitchDeg, RollDeg);
			return Out;
		}
	}

	bool IsHorizontalJump(const CamSimFrames::FGeoPose& Last, const CamSimFrames::FGeoPose& Next)
	{
		const FVector D = CamSimFrames::GeodeticDeltaToNeu(Last.Lat, Last.Lon, 0.0, Next.Lat, Next.Lon, 0.0);
		return FMath::Square(D.X) + FMath::Square(D.Y) > FMath::Square(ResetJumpM);
	}

	FTraceSpan GetTraceSpan(const FClampState& State)
	{
		if (!State.bHasSurface) return { FirstTraceTopM, FirstTraceBottomM };
		return { State.Height + TraceAboveM, State.Height - TraceBelowM };
	}

	FFootprint GetFootprint(double LatDeg, double LonDeg, double HeadingDeg, double HalfLengthM, double HalfBeamM)
	{
		const double H = FMath::DegreesToRadians(HeadingDeg);
		const FVector Fwd(FMath::Cos(H), FMath::Sin(H), 0.0);     // North, East, Up
		const FVector Right(-FMath::Sin(H), FMath::Cos(H), 0.0);
		const FVector Offsets[4] = { Fwd * HalfLengthM, -Fwd * HalfLengthM, -Right * HalfBeamM, Right * HalfBeamM };
		FFootprint F;
		for (int32 i = 0; i < 4; ++i)
		{
			double Alt;
			CamSimFrames::OffsetGeodetic(LatDeg, LonDeg, 0.0, Offsets[i], F.Lat[i], F.Lon[i], Alt);
		}
		return F;
	}

	CamSimFrames::FGeoPose ClampGround(const CamSimFrames::FGeoPose& Sender, const FGroundHits& InHits,
		double HalfLengthM, double HalfBeamM, double DtSec, FClampState& State)
	{
		const TOptional<double> Bow = Finite(InHits.Bow), Stern = Finite(InHits.Stern);
		const TOptional<double> Port = Finite(InHits.Port), Stbd = Finite(InHits.Stbd);

		double Sum = 0.0;
		int32  Count = 0;
		for (const TOptional<double>* H : { &Bow, &Stern, &Port, &Stbd })
		{
			if (H->IsSet()) { Sum += **H; ++Count; }
		}
		if (Count == 0)
		{
			return State.bHasSurface ? WithSurface(Sender, State.Height, State.PitchDeg, State.RollDeg) : Sender;
		}

		const double Target = Sum / Count;
		const bool bFirst = !State.bHasSurface;
		State.Height = bFirst ? Target : Ease(State.Height, Target, DtSec, /*bSnap=*/true);

		if (Count == 4 && HalfLengthM > 0.0 && HalfBeamM > 0.0)
		{
			const double Pitch = FMath::RadiansToDegrees(FMath::Atan((*Bow - *Stern) / (2.0 * HalfLengthM)));
			const double Roll  = FMath::RadiansToDegrees(FMath::Atan((*Port - *Stbd) / (2.0 * HalfBeamM)));
			State.PitchDeg = bFirst ? Pitch : Ease(State.PitchDeg, Pitch, DtSec, /*bSnap=*/false);
			State.RollDeg  = bFirst ? Roll  : Ease(State.RollDeg,  Roll,  DtSec, /*bSnap=*/false);
		}
		State.bHasSurface = true;
		return WithSurface(Sender, State.Height, State.PitchDeg, State.RollDeg);
	}

	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State)
	{
		const TOptional<double> Hit = Finite(CentreHit), Sea = Finite(SeaLevelM);
		CamSimFrames::FGeoPose Out = Sender;
		if (Hit.IsSet())
		{
			State.Height = State.bHasSurface ? Ease(State.Height, *Hit, DtSec, /*bSnap=*/true) : *Hit;
			State.bHasSurface = true;
			Out.Alt = State.Height;
		}
		else if (State.bHasSurface)
		{
			Out.Alt = State.Height;   // hold the last water height (tile eviction)
		}
		else if (Sea.IsSet())
		{
			Out.Alt = *Sea;           // no water hit yet: EGM96 sea level
		}
		return Out;
	}
}
