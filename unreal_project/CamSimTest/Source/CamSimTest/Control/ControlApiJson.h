// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "Control/ControlApiTypes.h"
#include "Sim/Commands.h"

/**
 * Control API request parsing (ROADMAP 2.4, docs/control-api.md). Pure: JSON text in, canonical commands or a 400
 * message out, so a malformed request is never half applied. Every parser rejects unknown fields (a typo would
 * otherwise be silently ignored), non-numbers and non-finite numbers, and out-of-range values. Angles are degrees,
 * heights WGS-84 ellipsoid metres, heading/pitch/roll CIGI's (north-referenced).
 */
namespace CamSimControl
{
	struct FParseError
	{
		FString Message;
	};
	template<typename T> using TParsed = TValueOrError<T, FParseError>;

	struct FClockRequest
	{
		TOptional<bool>      bLockstep;
		TOptional<FDateTime> Utc;
		TOptional<double>    Rate;
	};

	struct FStepRequest
	{
		double Seconds = 0.0;   // (0, 3600]
	};

	struct FEntityRequest
	{
		uint16 Id = 0;
		TOptional<uint16> TypeId;
		TOptional<CamSimFrames::FGeoPose> Pose;   // position and orientation (heading/pitch/roll default 0)
		TOptional<ESurfaceMode> Surface;
		TOptional<bool> bHidden;
		TArray<FComponentCommand> Components;     // Key filled by the adapter
	};

	struct FEnvironmentRequest
	{
		TOptional<FAtmosphereCommand> Atmosphere;
		TOptional<FWeatherCommand>    Clouds;          // one global layer
		TOptional<float>              Beaufort;        // [0, 12]
		TOptional<float>              SeaDirectionDeg; // waves come from, true north
	};

	struct FCaptureRequest
	{
		TOptional<int32>  SettleFrames;   // [0, 600]; unset = control_api.default_settle_frames
		TOptional<double> TimeoutSec;     // [1, 3600]; unset = control_api.capture_timeout_s
		bool bDepth = true;
		bool bSemantic = true;   // ROADMAP 4D class image
	};

	CAMSIMTEST_API TParsed<FClockRequest>         ParseClock(const FString& Body);
	CAMSIMTEST_API TParsed<FStepRequest>          ParseStep(const FString& Body);
	/** An empty body is {} (nothing to change). */
	CAMSIMTEST_API TParsed<FControlCameraCommand> ParseCamera(const FString& Body);
	/** Id from the path (1..65535). */
	CAMSIMTEST_API TParsed<FEntityRequest>        ParseEntity(uint32 Id, const FString& Body);
	CAMSIMTEST_API TParsed<FEnvironmentRequest>   ParseEnvironment(const FString& Body);
	/** An empty body takes the defaults. */
	CAMSIMTEST_API TParsed<FCaptureRequest>       ParseCapture(const FString& Body);

	/** A sensor request merged with the sensor's current state (unset fields keep it). */
	CAMSIMTEST_API FSensorCommand MergeSensor(const FControlSensorRequest& Req, bool bCurrentOn, ESensorMode CurrentMode,
		uint8 CurrentPolarity);

	/** {"error":"<Message>"}, escaped. */
	CAMSIMTEST_API FString ErrorJson(const FString& Message);
	/** ISO 8601 UTC with milliseconds, e.g. 2026-10-15T18:00:00.250Z. */
	CAMSIMTEST_API FString UtcIso(const FDateTime& Utc);
}
