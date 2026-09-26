// Copyright CamSim Contributors. All Rights Reserved.

#include "Hosts/CigiCommands.h"
#include "CIGI/CigiPacketTypes.h"

namespace CamSim::Cigi
{
	FEntityCommand ToEntityCommand(const FCigiEntityState& In)
	{
		FEntityCommand Out;
		Out.Key = Key(In.EntityId);
		// CIGI 3.3 Entity State: 0 Inactive/Standby, 1 Active, 2 Destroyed
		Out.Lifecycle = In.EntityState == 1 ? EEntityLifecycle::Active
		              : In.EntityState == 2 ? EEntityLifecycle::Remove
		              :                       EEntityLifecycle::Hidden;
		Out.TypeId = In.EntityType;
		Out.Classification = { In.EntityKind, In.EntityDomain, In.EntityCategory };
		Out.SourceTimeSec = In.HostTimeSec;

		if (In.bAttached)
		{
			// Attached: Lat/Lon/Alt carry X/Y/Z offsets in the parent's body frame.
			FEntityAttachment Attach;
			Attach.Parent    = Key(In.ParentId);
			Attach.OffsetFrd = FVector(In.Latitude, In.Longitude, In.Altitude);
			Attach.Rotation  = FRotator(In.Pitch, In.Yaw, In.Roll);
			Out.Attachment = Attach;
		}
		else
		{
			Out.Pose.Lat = In.Latitude;
			Out.Pose.Lon = In.Longitude;
			Out.Pose.Alt = In.Altitude;
			Out.Pose.Neu = CamSimFrames::CigiToNeu(In.Yaw, In.Pitch, In.Roll);
		}
		return Out;
	}

	FEntityCommand ToEntityCommand(const FCigiConfClampEntityState& In)
	{
		FEntityCommand Out;
		Out.Key = Key(In.EntityId);
		Out.bClampToTerrain = true;
		Out.Pose.Lat = In.Latitude;
		Out.Pose.Lon = In.Longitude;
		Out.Pose.Alt = 0.0;
		Out.Pose.Neu = CamSimFrames::CigiToNeu(In.Yaw, 0.0, 0.0);
		return Out;
	}

	TOptional<FEntityMotionCommand> ToMotionCommand(const FCigiRateControl& In)
	{
		if (In.bApplyToArtPart) return {};
		FEntityMotionCommand Out;
		Out.Key = Key(In.EntityId);
		Out.Motion.LinearFrame  = In.bLocalFrame ? FMotionModel::EFrame::Body : FMotionModel::EFrame::World;
		Out.Motion.AngularFrame = In.bAngularLocalFrame ? FMotionModel::EFrame::Body : FMotionModel::EFrame::World;
		Out.Motion.Velocity     = FVector(In.XRate, In.YRate, In.ZRate);
		Out.Motion.AngularRate  = FVector(In.RollRate, In.PitchRate, In.YawRate);
		return Out;
	}

	FArticulationCommand ToArticulationCommand(const FCigiArtPartControl& In)
	{
		FArticulationCommand Out;
		Out.Key      = Key(In.EntityId);
		Out.PartId   = In.ArtPartId;
		Out.bEnabled = In.bArtPartEn;
		Out.bXEn = In.bXOffEn; Out.bYEn = In.bYOffEn; Out.bZEn = In.bZOffEn;
		Out.bRollEn = In.bRollEn; Out.bPitchEn = In.bPitchEn; Out.bYawEn = In.bYawEn;
		Out.Offset   = FVector(In.XOff, In.YOff, In.ZOff);
		Out.Rotation = FRotator(In.Pitch, In.Yaw, In.Roll);
		return Out;
	}

	FComponentCommand ToComponentCommand(const FCigiComponentControl& In)
	{
		FComponentCommand Out;
		Out.Key            = Key(In.EntityId);
		Out.ComponentClass = In.CompClass;
		Out.ComponentId    = In.CompId;
		Out.State          = In.CompState;
		return Out;
	}

	FViewCommand ToViewCommand(const FCigiViewDefinition& In)
	{
		FViewCommand Out;
		Out.HFovDeg = In.HFovDeg();
		Out.VFovDeg = In.VFovDeg();
		return Out;
	}

	FViewCommand ToViewCommand(const FCigiViewControl& In)
	{
		FViewCommand Out;
		Out.Gimbal  = FViewCommand::EGimbal::Snap;
		Out.bYawEn  = In.bYawEn;  Out.Yaw   = In.Yaw;
		Out.bPitchEn = In.bPitchEn; Out.Pitch = In.Pitch;
		Out.bRollEn = In.bRollEn; Out.Roll  = In.Roll;
		if (In.EntityId != 0)
		{
			Out.EyeEntity = Key(In.EntityId);
			Out.bXOffEn = In.bXOffEn; Out.bYOffEn = In.bYOffEn; Out.bZOffEn = In.bZOffEn;
			Out.EyeOffsetFrd = FVector(In.XOff, In.YOff, In.ZOff);
		}
		else
		{
			Out.bClearEyeEntity = true;
		}
		return Out;
	}

	FViewCommand ToGimbalSlewCommand(const FCigiArtPartControl& In)
	{
		FViewCommand Out;
		if (!In.bArtPartEn) return Out;
		Out.Gimbal = FViewCommand::EGimbal::Slew;
		Out.bYawEn  = In.bYawEn;   Out.Yaw   = In.Yaw;
		Out.bPitchEn = In.bPitchEn; Out.Pitch = In.Pitch;
		Out.bRollEn = In.bRollEn;  Out.Roll  = In.Roll;
		return Out;
	}

	FSensorCommand ToSensorCommand(const FCigiSensorControl& In)
	{
		FSensorCommand Out;
		Out.bOn      = In.bSensorOn;
		// Sensor ID selects the waveband: 0 EO, 1 IR, 2 NVG (others: EO).
		Out.Waveband = In.SensorId == 1 ? ESensorMode::IR : In.SensorId == 2 ? ESensorMode::NVG : ESensorMode::EO;
		Out.Polarity = In.Polarity;
		// Gain selects the FOV preset: 0 widest … 1 narrowest.
		Out.Zoom     = FMath::Clamp(In.Gain, 0.0f, 1.0f);
		return Out;
	}

	FTimeCommand ToTimeCommand(const FCigiCelestialState& In)
	{
		FTimeCommand Out;
		// ICD 4.1.9: the date and time override the IG's only when Date/Time
		// Valid is set; Ephemeris Model Enable = continuous time of day.
		if (In.bDateVld && FDateTime::Validate(In.Year, In.Month, In.Day, In.Hour, In.Minute, 0, 0))
		{
			Out.Utc = FDateTime(In.Year, In.Month, In.Day, In.Hour, In.Minute);
		}
		Out.bRunning = In.bEphemerisEn;
		return Out;
	}

	FAtmosphereCommand ToAtmosphereCommand(const FCigiAtmosphereState& In)
	{
		FAtmosphereCommand Out;
		Out.bEnabled       = In.bAtmosEn;
		Out.HumidityPct    = In.Humidity;
		Out.AirTempC       = In.AirTemp;
		Out.VisibilityM    = In.Visibility;
		Out.HorizWindMps   = In.HorizWindSp;
		Out.VertWindMps    = In.VertWindSp;
		Out.WindDirDeg     = In.WindDir;
		Out.BaroPressureMb = In.BaroPress;
		return Out;
	}

	static FWeatherCommand::EScope ToScope(uint8 Scope)
	{
		return Scope == 1 ? FWeatherCommand::EScope::Regional
		     : Scope == 2 ? FWeatherCommand::EScope::Entity
		     :              FWeatherCommand::EScope::Global;
	}

	FWeatherCommand ToWeatherCommand(const FCigiWeatherState& In)
	{
		FWeatherCommand Out;
		Out.Scope        = ToScope(In.Scope);
		Out.RegionId     = In.RegionId;
		Out.LayerId      = In.LayerId;
		Out.bEnabled     = In.bWeatherEn;
		Out.Severity     = In.Severity;
		Out.CloudType    = In.CloudType;
		Out.CoveragePct  = In.Coverage;
		Out.BaseElevM    = In.BaseElev;
		Out.ThicknessM   = In.Thickness;
		Out.TransitionM  = In.Transition;
		Out.VisibilityM  = In.VisibilityRng;
		Out.HorizWindMps = In.HorizWindSp;
		Out.VertWindMps  = In.VertWindSp;
		Out.WindDirDeg   = In.WindDir;
		return Out;
	}

	FOceanWaveCommand ToOceanWaveCommand(const FCigiWaveState& In)
	{
		FOceanWaveCommand Out;
		Out.WaveId   = In.WaveID;
		Out.bEnabled = In.bEnabled;
		Out.HeightM  = In.WaveHtM;
		Out.LengthM  = In.WaveLenM;
		Out.PeriodS  = In.PeriodS;
		return Out;
	}

	FMaritimeSurfaceCommand ToMaritimeSurfaceCommand(const FCigiMaritimeSurfaceState& In)
	{
		FMaritimeSurfaceCommand Out;
		Out.Scope          = ToScope(In.Scope);
		Out.RegionId       = In.EntityRgnId;
		Out.bEnabled       = In.bSurfaceCondEn;
		Out.bWhitecaps     = In.bWhitecapEn;
		Out.SurfaceHeightM = In.SurfaceHeight;
		Out.WaterTempC     = In.WaterTemp;
		Out.Clarity        = In.Clarity;
		return Out;
	}
}
