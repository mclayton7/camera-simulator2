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

		double Ease(double Prev, double Target, double DtSec)
		{
			if (FMath::Abs(Target - Prev) >= SnapThresholdM || DtSec <= 0.0) return Target;
			return Prev + (Target - Prev) * (1.0 - FMath::Exp(-DtSec / EaseTimeConstantSec));
		}

		double EaseAngle(double Prev, double Target, double DtSec)
		{
			return DtSec <= 0.0 ? Target : Prev + (Target - Prev) * (1.0 - FMath::Exp(-DtSec / EaseTimeConstantSec));
		}

		CamSimFrames::FGeoPose WithSurface(const CamSimFrames::FGeoPose& Sender, double Height, double PitchDeg, double RollDeg)
		{
			CamSimFrames::FGeoPose Out = Sender;
			Out.Alt = Height;
			Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, PitchDeg, RollDeg);
			return Out;
		}
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
		State.Height = bFirst ? Target : Ease(State.Height, Target, DtSec);

		if (Count == 4 && HalfLengthM > 0.0 && HalfBeamM > 0.0)
		{
			const double Pitch = FMath::RadiansToDegrees(FMath::Atan((*Bow - *Stern) / (2.0 * HalfLengthM)));
			const double Roll  = FMath::RadiansToDegrees(FMath::Atan((*Port - *Stbd) / (2.0 * HalfBeamM)));
			State.PitchDeg = bFirst ? Pitch : EaseAngle(State.PitchDeg, Pitch, DtSec);
			State.RollDeg  = bFirst ? Roll  : EaseAngle(State.RollDeg,  Roll,  DtSec);
		}
		State.bHasSurface = true;
		return WithSurface(Sender, State.Height, State.PitchDeg, State.RollDeg);
	}

	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State)
	{
		const TOptional<double> Hit = Finite(CentreHit), Sea = Finite(SeaLevelM);
		if (!Hit.IsSet() && !Sea.IsSet()) return Sender;

		const double Target = Hit.IsSet() ? *Hit : *Sea;
		State.Height = State.bHasSurface ? Ease(State.Height, Target, DtSec) : Target;
		State.bHasSurface = true;
		CamSimFrames::FGeoPose Out = Sender;
		Out.Alt = State.Height;
		return Out;
	}
}
