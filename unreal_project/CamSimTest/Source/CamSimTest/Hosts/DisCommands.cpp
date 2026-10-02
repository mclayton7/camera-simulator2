// Copyright CamSim Contributors. All Rights Reserved.

#include "Hosts/DisCommands.h"
#include "DIS/DisPduTypes.h"
#include "Geospatial/EcefFrames.h"

namespace CamSim::Dis
{
	FEntityKey Key(const FDisEntityId& Id)
	{
		return FEntityKey(EHostSource::Dis,
			(uint64(Id.Site) << 32) | (uint64(Id.Application) << 16) | uint64(Id.Entity));
	}

	TOptional<FMotionModel> ToMotionModel(const FDisEntityStatePdu& Pdu, double LatDeg, double LonDeg)
	{
		// IEEE 1278.1 dead reckoning: 2-5 world (ECEF) velocity/acceleration,
		// 6-9 body; rotating algorithms (3, 4, 7, 8) use body angular velocity;
		// acceleration only for 4, 5, 8, 9.
		const uint8 Algorithm = Pdu.DeadReckoning.Algorithm;
		if (Algorithm < 2 || Algorithm > 9) return {};

		const bool bWorld    = Algorithm <= 5;
		const bool bRotating = Algorithm == 3 || Algorithm == 4 || Algorithm == 7 || Algorithm == 8;
		const bool bAccel    = Algorithm == 4 || Algorithm == 5 || Algorithm == 8 || Algorithm == 9;
		const auto& Dr = Pdu.DeadReckoning;

		FMotionModel Model;
		Model.LinearFrame = bWorld ? FMotionModel::EFrame::World : FMotionModel::EFrame::Body;
		const FVector Velocity(Dr.VelX, Dr.VelY, Dr.VelZ);
		const FVector Accel(Dr.AccelX, Dr.AccelY, Dr.AccelZ);
		Model.Velocity = bWorld ? CamSimFrames::EcefVectorToNed(Velocity, LatDeg, LonDeg) : Velocity;
		if (bAccel)
		{
			Model.Acceleration = bWorld ? CamSimFrames::EcefVectorToNed(Accel, LatDeg, LonDeg) : Accel;
		}
		Model.AngularFrame = FMotionModel::EFrame::Body;
		if (bRotating)
		{
			Model.AngularRate = FVector(FMath::RadiansToDegrees(Dr.AngVelX),
			                            FMath::RadiansToDegrees(Dr.AngVelY),
			                            FMath::RadiansToDegrees(Dr.AngVelZ));
		}
		return Model;
	}

	ESurfaceMode SurfaceModeFor(uint8 Kind, uint8 Domain, bool bClampToSurface)
	{
		if (!bClampToSurface || Kind != 1) return ESurfaceMode::None;   // platforms only
		switch (Domain)
		{
		case 1:  return ESurfaceMode::Ground;
		case 3:  return ESurfaceMode::Water;
		default: return ESurfaceMode::None;
		}
	}

	FEntityCommand ToEntityCommand(const FDisEntityStatePdu& Pdu, uint16 TypeId, bool bClampToSurface)
	{
		FEntityCommand Out;
		Out.Key       = Key(Pdu.EntityId);
		Out.Lifecycle = EEntityLifecycle::Active;
		Out.TypeId    = TypeId;
		Out.Classification = { Pdu.EntityType.EntityKind, Pdu.EntityType.Domain, Pdu.EntityType.Category };
		Out.SurfaceMode = SurfaceModeFor(Pdu.EntityType.EntityKind, Pdu.EntityType.Domain, bClampToSurface);

		CamSimFrames::EcefToGeodetic(FVector(Pdu.LocationX, Pdu.LocationY, Pdu.LocationZ),
			Out.Pose.Lat, Out.Pose.Lon, Out.Pose.Alt);
		const FRotator Hpr = CamSimFrames::DisEulerToCigi(Pdu.Psi, Pdu.Theta, Pdu.Phi, Out.Pose.Lat, Out.Pose.Lon);
		Out.Pose.Neu = CamSimFrames::CigiToNeu(Hpr.Yaw, Hpr.Pitch, Hpr.Roll);
		Out.Motion = ToMotionModel(Pdu, Out.Pose.Lat, Out.Pose.Lon);
		return Out;
	}

	TOptional<FPlatformAppearance> DecodePlatformAppearance(uint8 Kind, uint8 Domain, uint32 Appearance)
	{
		if (Kind != 1 || Domain < 1 || Domain > 3) return {};
		FPlatformAppearance A;
		A.bPowerPlant = ((Appearance >> 22) & 1u) != 0u;
		A.bFlaming    = ((Appearance >> 15) & 1u) != 0u;
		const uint32 D = (Appearance >> 3) & 3u;
		A.Damage = D == 3u ? 2 : (D == 0u ? 0 : 1);
		return A;
	}

	void AppearanceCommands(const FEntityKey& Key, const TOptional<FPlatformAppearance>& Previous, const FPlatformAppearance& Now,
		TArray<FComponentCommand>& Out)
	{
		const FPlatformAppearance Prev = Previous.Get(FPlatformAppearance());
		auto Emit = [&Key, &Out](uint16 Id, uint8 State)
		{
			FComponentCommand C;
			C.Key = Key;
			C.ComponentClass = 0;
			C.ComponentId = Id;
			C.State = State;
			Out.Add(C);
		};
		if (Now.Damage != Prev.Damage)           Emit(10, Now.Damage);
		if (Now.bPowerPlant != Prev.bPowerPlant) Emit(11, Now.bPowerPlant ? 1 : 0);
		if (Now.bFlaming != Prev.bFlaming)       Emit(12, Now.bFlaming ? 1 : 0);
	}
}
