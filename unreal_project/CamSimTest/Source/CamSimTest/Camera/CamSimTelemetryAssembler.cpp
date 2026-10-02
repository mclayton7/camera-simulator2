// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimTelemetryAssembler.h"
#include "CIGI/CigiPacketTypes.h"
#include "Environment/CamSimEnvironment.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/EcefFrames.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Time/SimClock.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/App.h"

void FCamSimTelemetryAssembler::SetPlatformPose(double Lat, double Lon, double Alt, float Yaw, float Pitch, float Roll)
{
	Telemetry.Latitude  = Lat;
	Telemetry.Longitude = Lon;
	Telemetry.Altitude  = Alt;
	Telemetry.Yaw       = Yaw;
	Telemetry.Pitch     = Pitch;
	Telemetry.Roll      = Roll;
}

void FCamSimTelemetryAssembler::SetGimbal(float Yaw, float Pitch, float Roll)
{
	Telemetry.GimbalYaw   = Yaw;
	Telemetry.GimbalPitch = Pitch;
	Telemetry.GimbalRoll  = Roll;
}

void FCamSimTelemetryAssembler::SetSensor(uint8 Mode, uint8 Polarity)
{
	Telemetry.SensorMode     = Mode;
	Telemetry.SensorPolarity = Polarity;
}

void FCamSimTelemetryAssembler::SetFieldOfView(float HFovDeg, float VFovDeg)
{
	Telemetry.HFovDeg = HFovDeg;
	Telemetry.VFovDeg = VFovDeg;
}

void FCamSimTelemetryAssembler::ReadEnvironment(UWorld* World)
{
	if (!World) return;
	TActorIterator<ACamSimEnvironment> It(World);
	if (!It) return;

	Telemetry.SunElevationDeg = It->GetSunElevationDeg();

	const ACamSimEnvironment::FAtmosphericSnapshot Snap = It->GetAtmosphericSnapshot();
	Telemetry.AtmosphericVisibilityM = Snap.AtmosphericVisibilityM;
	Telemetry.RelativeHumidity       = Snap.RelativeHumidity;
	Telemetry.AirTempCelsius         = Snap.AirTempCelsius;
	Telemetry.WeatherSeverity        = Snap.WeatherSeverity;
	Telemetry.WeatherPrecipType      = Snap.WeatherPrecipType;
	Telemetry.bHasAtmosphere         = Snap.bHostAtmosphere;
	Telemetry.WindDirectionDeg       = Snap.WindDirectionDeg;
	Telemetry.WindSpeedMps           = Snap.WindSpeedMps;
	Telemetry.BaroPressureMb         = Snap.BaroPressureMb;
}

void FCamSimTelemetryAssembler::ApplyHostFrame(bool bHasPose, const FCigiPlatformKinematics* K, double AppTimeSec)
{
	if (bHasPose)
	{
		// A sample time describes only the pose of its own datagram.
		bHostSampleValid = K && K->HasSampleTime() && FMath::IsFinite(K->SampleUtcSec) && K->SampleUtcSec > 0.0;
		if (bHostSampleValid)
		{
			HostSampleUtcSec = K->SampleUtcSec;
			HostSampleAppSec = AppTimeSec;
		}
	}
	if (!K) return;

	if (K->HasAirspeeds() && FMath::IsFinite(K->TrueAirspeedMps) && FMath::IsFinite(K->IndicatedAirspeedMps))
	{
		Telemetry.TrueAirspeedMps      = K->TrueAirspeedMps;
		Telemetry.IndicatedAirspeedMps = K->IndicatedAirspeedMps;
		AirspeedTimeSec = AppTimeSec;
	}
	if (K->HasMagneticHeading() && FMath::IsFinite(K->MagneticHeadingDeg))
	{
		Telemetry.MagneticHeadingDeg = K->MagneticHeadingDeg;
		MagneticHeadingTimeSec = AppTimeSec;
	}
	if (K->HasNedVelocity() && FMath::IsFinite(K->VelNorthMps) && FMath::IsFinite(K->VelEastMps)
		&& FMath::IsFinite(K->VelDownMps))
	{
		Telemetry.VelNorthMps = K->VelNorthMps;
		Telemetry.VelEastMps  = K->VelEastMps;
		Telemetry.VelDownMps  = K->VelDownMps;
		// Tag 56: the host's velocity beats differencing positions.
		Telemetry.GroundSpeedMps = static_cast<float>(FMath::Sqrt(
			FMath::Square(static_cast<double>(K->VelNorthMps)) + FMath::Square(static_cast<double>(K->VelEastMps))));
		VelocityTimeSec = AppTimeSec;
	}
}

FQuat FCamSimTelemetryAssembler::SensorToNeu(const FCamSimTelemetry& T)
{
	return CamSimFrames::CigiToNeu(T.Yaw, T.Pitch, T.Roll)
		* FRotator(T.GimbalPitch, T.GimbalYaw, T.GimbalRoll).Quaternion();
}

FVector FCamSimTelemetryAssembler::CornerDirection(int32 Index, float HFovDeg, float VFovDeg)
{
	const double TanH = FMath::Tan(FMath::DegreesToRadians(0.5 * static_cast<double>(HFovDeg)));
	const double TanV = FMath::Tan(FMath::DegreesToRadians(0.5 * static_cast<double>(VFovDeg)));
	// Clockwise from the upper left (ST 0601 corner points 1-4): right = +Y, up = +Z.
	static constexpr double Right[4] = { -1.0, 1.0,  1.0, -1.0 };
	static constexpr double Up[4]    = {  1.0, 1.0, -1.0, -1.0 };
	const int32 I = FMath::Clamp(Index, 0, 3);
	return FVector(1.0, Right[I] * TanH, Up[I] * TanV).GetSafeNormal();
}

bool FCamSimTelemetryAssembler::IntersectEllipsoid(double Lat, double Lon, double AltM, const FVector& DirNeu,
	double SurfaceHeightM, double& OutRangeM, double& OutLat, double& OutLon)
{
	OutRangeM = 0.0;
	if (AltM <= SurfaceHeightM) return false;  // at or below the surface: no ground in view

	using namespace CamSimFrames;
	const FVector O = GeodeticToEcef(Lat, Lon, AltM);
	const FVector D = (NedToEcef(Lat, Lon) * FVector(DirNeu.X, DirNeu.Y, -DirNeu.Z)).GetSafeNormal();
	if (D.IsZero()) return false;

	// The surface at SurfaceHeightM, approximated by the ellipsoid with both
	// semi-axes raised by it: (x² + y²) / a'² + z² / b'² = 1.
	const double A2 = FMath::Square(Wgs84::A + SurfaceHeightM);
	const double B2 = FMath::Square(Wgs84::B + SurfaceHeightM);
	const double Qa = (D.X * D.X + D.Y * D.Y) / A2 + D.Z * D.Z / B2;
	const double Qb = 2.0 * ((O.X * D.X + O.Y * D.Y) / A2 + O.Z * D.Z / B2);
	const double Qc = (O.X * O.X + O.Y * O.Y) / A2 + O.Z * O.Z / B2 - 1.0;
	const double Disc = Qb * Qb - 4.0 * Qa * Qc;
	if (Qa <= 0.0 || Disc < 0.0) return false;           // passes above the horizon
	const double T = (-Qb - FMath::Sqrt(Disc)) / (2.0 * Qa);  // nearer root
	if (T <= 0.0) return false;                          // the surface is behind the sensor

	double HitAlt = 0.0;
	EcefToGeodetic(O + D * T, OutLat, OutLon, HitAlt);
	OutRangeM = T;
	return true;
}

bool FCamSimTelemetryAssembler::TraceGround(UWorld* World, const FVector& StartCm, const FVector& DirWorld,
	const AActor* IgnoreActor, const FCamSimGeospatialProvider* GeoProvider,
	double& OutRangeM, double& OutLat, double& OutLon, double& OutAlt)
{
	if (!World) return false;
	const FVector EndCm = StartCm + DirWorld * 500'000'000.0;  // 5000 km in cm

	FHitResult Hit;
	FCollisionQueryParams Params(NAME_None, /*bTraceComplex=*/false);
	Params.AddIgnoredActor(IgnoreActor);
	if (!World->LineTraceSingleByChannel(Hit, StartCm, EndCm, ECC_Visibility, Params)) return false;
	if (!GeoProvider || !GeoProvider->WorldToGeo(World, Hit.Location, OutLat, OutLon, OutAlt)) return false;
	OutRangeM = static_cast<double>(Hit.Distance) / 100.0;  // cm -> m
	return true;
}

void FCamSimTelemetryAssembler::UpdateFootprint(UWorld* World, const USceneCaptureComponent2D* Sensor,
	const AActor* IgnoreActor, const FCamSimGeospatialProvider* GeoProvider)
{
	FCamSimTelemetry& T = Telemetry;
	const FQuat SensorNeu = SensorToNeu(T);
	const bool bTrace = World && Sensor;
	const FVector StartCm = bTrace ? Sensor->GetComponentLocation() : FVector::ZeroVector;
	const FQuat SensorWorld = bTrace ? Sensor->GetComponentQuat() : FQuat::Identity;

	// Frame centre: terrain along the boresight, else the ellipsoid at the last terrain height.
	double Range = 0.0, Lat = 0.0, Lon = 0.0, Alt = 0.0;
	if (bTrace && TraceGround(World, StartCm, SensorWorld.GetForwardVector(), IgnoreActor, GeoProvider, Range, Lat, Lon, Alt))
	{
		T.SlantRangeM     = Range;
		T.FrameCenterLat  = Lat;
		T.FrameCenterLon  = Lon;
		T.FrameCenterElev = Alt;
		GroundHeightM     = Alt;
	}
	else if (IntersectEllipsoid(T.Latitude, T.Longitude, T.Altitude, SensorNeu.GetForwardVector(), GroundHeightM,
		Range, Lat, Lon))
	{
		T.SlantRangeM     = Range;
		T.FrameCenterLat  = Lat;
		T.FrameCenterLon  = Lon;
		T.FrameCenterElev = GroundHeightM;
	}
	else
	{
		T.SlantRangeM = 0.0;  // above the horizon: Tags 21, 23-25 and 78 are omitted
	}

	// Corners: terrain along each corner ray, else the ellipsoid at the frame-centre height.
	const double CornerSurfaceM = (T.SlantRangeM > 0.0) ? T.FrameCenterElev : GroundHeightM;
	T.CornerValidMask = 0;
	for (int32 i = 0; i < 4; ++i)
	{
		const FVector DirCam = CornerDirection(i, T.HFovDeg, T.VFovDeg);
		bool bHit = bTrace && TraceGround(World, StartCm, SensorWorld.RotateVector(DirCam), IgnoreActor, GeoProvider,
			Range, Lat, Lon, Alt);
		if (!bHit)
		{
			bHit = IntersectEllipsoid(T.Latitude, T.Longitude, T.Altitude, SensorNeu.RotateVector(DirCam),
				CornerSurfaceM, Range, Lat, Lon);
		}
		if (bHit)
		{
			T.CornerLat[i] = Lat;
			T.CornerLon[i] = Lon;
			T.CornerValidMask |= static_cast<uint8>(1u << i);
		}
	}
}

FCamSimTelemetry FCamSimTelemetryAssembler::Snapshot()
{
	return Snapshot(FApp::GetCurrentTime());
}

FCamSimTelemetry FCamSimTelemetryAssembler::Snapshot(double AppTimeSec)
{
	FCamSimTelemetry& T = Telemetry;

	// Tag 2: the host's sample time of the pose on screen (packet 201), carried
	// forward by engine time on frames without a new pose; else the sim clock.
	uint64 TimestampUs = 0;
	T.bTimestampFromHost = bHostSampleValid;
	if (bHostSampleValid)
	{
		const double Utc = HostSampleUtcSec + FMath::Max(0.0, AppTimeSec - HostSampleAppSec);
		TimestampUs = static_cast<uint64>(FMath::RoundToDouble(Utc * 1e6));
	}
	else
	{
		TimestampUs = FSimClock::Get().NowMicros();
	}
	// Never step back a little (host jitter, switching clocks); a big step back
	// (a host restart or replay) is real and passes through.
	// A frozen clock (CIGI ephemeris off) still repeats its value.
	if (LastTimestampUs != 0 && TimestampUs < LastTimestampUs && LastTimestampUs - TimestampUs < 1'000'000)
	{
		TimestampUs = LastTimestampUs + 1;
	}
	LastTimestampUs = TimestampUs;
	T.TimestampUs   = TimestampUs;

	// Held host kinematics expire when the host stops sending them.
	auto Fresh = [AppTimeSec](double SetSec) { return SetSec >= 0.0 && AppTimeSec - SetSec <= KinematicsTimeoutSec; };
	T.bHasAirspeed        = Fresh(AirspeedTimeSec);
	T.bHasMagneticHeading = Fresh(MagneticHeadingTimeSec);
	T.bHasVelocity        = Fresh(VelocityTimeSec);
	return T;
}
