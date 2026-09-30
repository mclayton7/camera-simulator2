// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/SurfaceClamp.h"
#include "Ocean/OceanSurface.h"

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

	CamSimFrames::FGeoPose ClampWater(const CamSimFrames::FGeoPose& Sender, TOptional<double> CentreHit,
		TOptional<double> SeaLevelM, double DtSec, FClampState& State, const FWaterInput& Water,
		double HalfLengthM, double HalfBeamM)
	{
		const TOptional<double> Sea = Water.Ocean ? Water.Ocean->SeaLevelM(Sender.Lat, Sender.Lon) : TOptional<double>();
		if (!Sea.IsSet())
		{
			return ClampWater(Sender, CentreHit, SeaLevelM, DtSec, State);   // no ocean / no geoid: as 2.5
		}
		// Base: the higher of the drawn tile surface and the sea (seabed loses, a lake above sea
		// level wins), with LakeMarginM of slack so decimetre noise between Cesium's surface and
		// the geoid doesn't misclassify an open-sea boat as inland. A miss holds the last base
		// height (tile eviction) instead of snapping to sea level — a lake boat shouldn't drop
		// to sea level just because its tile went away.
		// Lake only above max(Sea, geoid + LakeMarginM): Cesium's surface sits near the geoid and
		// doesn't move with the CIGI tide, so a low tide (-3 m) must not turn the open sea into a
		// "lake", and a high tide (+3 m) must not either (the sea then covers the geoid + 2 m band).
		// The boat itself floats at Sea (with tide).
		const TOptional<double> Hit = Finite(CentreHit);
		const double LakeAbove = FMath::Max(*Sea, *Sea - Water.Ocean->GetTideOffsetM() + LakeMarginM);
		bool bLake;
		double Target;
		if (Hit.IsSet())
		{
			bLake  = *Hit > LakeAbove;
			Target = bLake ? *Hit : *Sea;
		}
		else if (State.bHasSurface)
		{
			// A lake holds its height through a miss; at sea the boat follows Sea (e.g. a falling tide).
			const double Held = FMath::Max(State.Height, *Sea);
			bLake  = Held > LakeAbove;
			Target = bLake ? Held : *Sea;
		}
		else
		{
			Target = *Sea;
			bLake  = false;
		}
		State.Height = State.bHasSurface ? Ease(State.Height, Target, DtSec, /*bSnap=*/true) : Target;
		State.bHasSurface = true;
		State.PitchDeg = State.RollDeg = 0.0;   // unused on the wave path below (the sampled attitude is never eased through these)

		CamSimFrames::FGeoPose Out = Sender;
		Out.Alt = State.Height;
		if (bLake || !Water.bMotion || Water.Ocean->GetWaves().GetWaves().Num() == 0 || HalfLengthM <= 0.0 || HalfBeamM <= 0.0)
		{
			Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, 0.0, 0.0);
			return Out;
		}
		// Waves: four hull points (never eased — the motion is the signal).
		const FFootprint F = GetFootprint(Sender.Lat, Sender.Lon, Sender.Neu.Rotator().Yaw, HalfLengthM, HalfBeamM);
		double H[4];
		for (int32 i = 0; i < 4; ++i)
		{
			H[i] = Water.Ocean->SurfaceHeightM(F.Lat[i], F.Lon[i]).Get(*Sea) - *Sea;
		}
		const double S = Water.MotionScale;
		const double Pitch = FMath::RadiansToDegrees(FMath::Atan((H[0] - H[1]) / (2.0 * HalfLengthM))) * S;
		const double Roll  = FMath::RadiansToDegrees(FMath::Atan((H[2] - H[3]) / (2.0 * HalfBeamM))) * S;
		Out.Alt += 0.25 * (H[0] + H[1] + H[2] + H[3]) * S;
		Out.Neu = CamSimFrames::CigiToNeu(Sender.Neu.Rotator().Yaw, Pitch, Roll);
		return Out;
	}
}
