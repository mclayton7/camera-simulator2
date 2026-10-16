// Copyright CamSim Contributors. All Rights Reserved.

#include "Control/ControlApiJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CamSimControl
{
namespace
{
	/**
	 * Reads fields of one JSON object, keeping the first error and the keys it consumed, so CheckUnknown can reject
	 * whatever is left (a misspelt key). Prefix names nested objects in messages ("gimbal.").
	 */
	class FFieldReader
	{
	public:
		FFieldReader(const TSharedPtr<FJsonObject>& InObj, FString InPrefix, FString& InError)
			: Obj(InObj), Prefix(MoveTemp(InPrefix)), Error(InError) {}

		bool Has(const TCHAR* Key) const { return Obj->HasField(Key); }

		TOptional<double> Number(const TCHAR* Key, double Min, double Max, bool bMinOpen = false, bool bMaxOpen = false)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return {};
			double D = 0.0;
			if (V->Type != EJson::Number || !V->TryGetNumber(D) || !FMath::IsFinite(D))
			{
				Fail(FString::Printf(TEXT("%s must be a number"), *Name(Key)));
				return {};
			}
			const bool bLow  = bMinOpen ? D <= Min : D < Min;
			const bool bHigh = bMaxOpen ? D >= Max : D > Max;
			if (bLow || bHigh)
			{
				Fail(FString::Printf(TEXT("%s must be in %s%g, %g%s (got %g)"), *Name(Key),
					bMinOpen ? TEXT("(") : TEXT("["), Min, Max, bMaxOpen ? TEXT(")") : TEXT("]"), D));
				return {};
			}
			return D;
		}

		TOptional<int64> Integer(const TCHAR* Key, int64 Min, int64 Max)
		{
			const TOptional<double> D = Number(Key, static_cast<double>(Min), static_cast<double>(Max));
			if (!D) return {};
			if (FMath::Frac(*D) != 0.0)
			{
				Fail(FString::Printf(TEXT("%s must be an integer"), *Name(Key)));
				return {};
			}
			return static_cast<int64>(*D);
		}

		TOptional<bool> Bool(const TCHAR* Key)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return {};
			if (V->Type != EJson::Boolean)
			{
				Fail(FString::Printf(TEXT("%s must be true or false"), *Name(Key)));
				return {};
			}
			return V->AsBool();
		}

		/** One of Allowed (index returned). */
		TOptional<int32> Enum(const TCHAR* Key, std::initializer_list<const TCHAR*> Allowed)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return {};
			FString S;
			if (V->Type == EJson::String && V->TryGetString(S))
			{
				int32 I = 0;
				for (const TCHAR* A : Allowed)
				{
					if (S == A) return I;
					++I;
				}
			}
			FString List;
			for (const TCHAR* A : Allowed) List += (List.IsEmpty() ? TEXT("") : TEXT(", ")) + FString(A);
			Fail(FString::Printf(TEXT("%s must be one of %s"), *Name(Key), *List));
			return {};
		}

		TOptional<FString> String(const TCHAR* Key)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return {};
			FString S;
			if (V->Type != EJson::String || !V->TryGetString(S))
			{
				Fail(FString::Printf(TEXT("%s must be a string"), *Name(Key)));
				return {};
			}
			return S;
		}

		TSharedPtr<FJsonObject> Object(const TCHAR* Key)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return nullptr;
			if (V->Type != EJson::Object)
			{
				Fail(FString::Printf(TEXT("%s must be an object"), *Name(Key)));
				return nullptr;
			}
			return V->AsObject();
		}

		const TArray<TSharedPtr<FJsonValue>>* Array(const TCHAR* Key)
		{
			const TSharedPtr<FJsonValue> V = Take(Key);
			if (!V) return nullptr;
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			if (V->Type != EJson::Array || !V->TryGetArray(A))
			{
				Fail(FString::Printf(TEXT("%s must be an array"), *Name(Key)));
				return nullptr;
			}
			return A;
		}

		void CheckUnknown()
		{
			for (const auto& Pair : Obj->Values)
			{
				const FString Key(*Pair.Key);   // FJsonObject keys are shared strings in UE 5.8
				if (!Used.Contains(Key))
				{
					Fail(FString::Printf(TEXT("unknown field \"%s%s\""), *Prefix, *Key));
					return;
				}
			}
		}

		void Fail(const FString& Message)
		{
			if (Error.IsEmpty()) Error = Message;
		}

		FString Name(const TCHAR* Key) const { return Prefix + Key; }
		const FString& GetPrefix() const { return Prefix; }

	private:
		TSharedPtr<FJsonValue> Take(const TCHAR* Key)
		{
			Used.Add(Key);
			const TSharedPtr<FJsonValue>* V = Obj->Values.Find(Key);
			return (V && *V && (*V)->Type != EJson::Null) ? *V : nullptr;
		}

		TSharedPtr<FJsonObject> Obj;
		FString Prefix;
		FString& Error;
		TSet<FString> Used;
	};

	/** The body as a JSON object; an empty body is {} when bEmptyIsObject. */
	TSharedPtr<FJsonObject> ParseObject(const FString& Body, bool bEmptyIsObject, FString& Error)
	{
		if (Body.TrimStartAndEnd().IsEmpty())
		{
			if (bEmptyIsObject) return MakeShared<FJsonObject>();
			Error = TEXT("the body must be a JSON object");
			return nullptr;
		}
		TSharedPtr<FJsonObject> Obj;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Obj) || !Obj.IsValid())
		{
			Error = TEXT("the body must be a JSON object");
			return nullptr;
		}
		return Obj;
	}

	/** lat/lon/alt (all or none) + heading/pitch/roll. Returns false when there is no position. */
	TOptional<CamSimFrames::FGeoPose> ReadPose(FFieldReader& R)
	{
		const bool bAny = R.Has(TEXT("lat")) || R.Has(TEXT("lon")) || R.Has(TEXT("alt")) || R.Has(TEXT("heading"))
			|| R.Has(TEXT("pitch")) || R.Has(TEXT("roll"));
		if (!bAny) return {};
		const bool bAll = R.Has(TEXT("lat")) && R.Has(TEXT("lon")) && R.Has(TEXT("alt"));
		if (!bAll)
		{
			R.Fail(TEXT("lat, lon and alt go together (and heading, pitch, roll need them)"));
			return {};
		}
		const TOptional<double> Lat = R.Number(TEXT("lat"), -90.0, 90.0);
		const TOptional<double> Lon = R.Number(TEXT("lon"), -180.0, 180.0);
		const TOptional<double> Alt = R.Number(TEXT("alt"), -12000.0, 1.0e6);
		const double Heading = R.Number(TEXT("heading"), -360.0, 360.0).Get(0.0);
		const double Pitch   = R.Number(TEXT("pitch"), -90.0, 90.0).Get(0.0);
		const double Roll    = R.Number(TEXT("roll"), -180.0, 180.0).Get(0.0);
		if (!Lat || !Lon || !Alt) return {};
		CamSimFrames::FGeoPose Pose;
		Pose.Lat = *Lat;
		Pose.Lon = *Lon;
		Pose.Alt = *Alt;
		Pose.Neu = CamSimFrames::CigiToNeu(Heading, Pitch, Roll);
		return Pose;
	}

	template<typename T>
	TParsed<T> Finish(const FString& Error, T&& Value)
	{
		if (!Error.IsEmpty()) return MakeError(FParseError{ Error });
		return MakeValue(MoveTemp(Value));
	}
}

TParsed<FClockRequest> ParseClock(const FString& Body)
{
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, false, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FClockRequest Out;
	Out.bLockstep = R.Bool(TEXT("lockstep"));
	if (const TOptional<FString> S = R.String(TEXT("utc")))
	{
		FDateTime Utc;
		if (FDateTime::ParseIso8601(**S, Utc)) Out.Utc = Utc;
		else R.Fail(FString::Printf(TEXT("utc must be ISO 8601, e.g. 2026-07-01T19:30:00Z (got \"%s\")"), **S));
	}
	Out.Rate = R.Number(TEXT("rate"), 0.0, 1000.0);
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

TParsed<FStepRequest> ParseStep(const FString& Body)
{
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, false, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FStepRequest Out;
	if (!R.Has(TEXT("seconds"))) R.Fail(TEXT("seconds is required"));
	Out.Seconds = R.Number(TEXT("seconds"), 0.0, 3600.0, /*bMinOpen=*/true).Get(0.0);
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

TParsed<FControlCameraCommand> ParseCamera(const FString& Body)
{
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, true, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FControlCameraCommand Out;
	Out.Pose = ReadPose(R);

	const TOptional<double> HFov = R.Number(TEXT("hfov_deg"), 0.0, 180.0, true, true);
	const TSharedPtr<FJsonObject> Gimbal = R.Object(TEXT("gimbal"));
	if (HFov || Gimbal)
	{
		FViewCommand View;
		if (HFov) View.HFovDeg = static_cast<float>(*HFov);
		if (Gimbal)
		{
			FFieldReader G(Gimbal, TEXT("gimbal."), Error);
			View.Gimbal = FViewCommand::EGimbal::Snap;
			if (const TOptional<double> V = G.Number(TEXT("yaw"), -360.0, 360.0))   { View.bYawEn = true;   View.Yaw = *V; }
			if (const TOptional<double> V = G.Number(TEXT("pitch"), -180.0, 180.0)) { View.bPitchEn = true; View.Pitch = *V; }
			if (const TOptional<double> V = G.Number(TEXT("roll"), -360.0, 360.0))  { View.bRollEn = true;  View.Roll = *V; }
			G.CheckUnknown();
		}
		Out.View = View;
	}

	if (const TSharedPtr<FJsonObject> Sensor = R.Object(TEXT("sensor")))
	{
		FFieldReader S(Sensor, TEXT("sensor."), Error);
		FControlSensorRequest Cmd;
		Cmd.bOn = S.Bool(TEXT("on"));
		if (const TOptional<int32> M = S.Enum(TEXT("mode"), { TEXT("eo"), TEXT("ir") })) Cmd.Mode = *M == 1 ? ESensorMode::IR : ESensorMode::EO;
		if (const TOptional<int32> P = S.Enum(TEXT("polarity"), { TEXT("white_hot"), TEXT("black_hot") })) Cmd.Polarity = static_cast<uint8>(*P);
		S.CheckUnknown();
		Out.Sensor = Cmd;
	}
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

TParsed<FEntityRequest> ParseEntity(uint32 Id, const FString& Body)
{
	if (Id < 1 || Id > 65535)
	{
		return MakeError(FParseError{ FString::Printf(TEXT("entity id must be in 1..65535 (got %u)"), Id) });
	}
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, true, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FEntityRequest Out;
	Out.Id = static_cast<uint16>(Id);
	if (const TOptional<int64> T = R.Integer(TEXT("type_id"), 1, 65535)) Out.TypeId = static_cast<uint16>(*T);
	Out.Pose = ReadPose(R);
	if (const TOptional<int32> S = R.Enum(TEXT("surface"), { TEXT("none"), TEXT("ground"), TEXT("water") }))
	{
		Out.Surface = *S == 1 ? ESurfaceMode::Ground : (*S == 2 ? ESurfaceMode::Water : ESurfaceMode::None);
	}
	Out.bHidden = R.Bool(TEXT("hidden"));
	if (const TArray<TSharedPtr<FJsonValue>>* Comps = R.Array(TEXT("components")))
	{
		for (int32 I = 0; I < Comps->Num(); ++I)
		{
			const TSharedPtr<FJsonValue>& V = (*Comps)[I];
			if (!V || V->Type != EJson::Object)
			{
				R.Fail(FString::Printf(TEXT("components[%d] must be an object"), I));
				break;
			}
			FFieldReader C(V->AsObject(), FString::Printf(TEXT("components[%d]."), I), Error);
			FComponentCommand Cmd;
			Cmd.ComponentClass = static_cast<uint8>(C.Integer(TEXT("class"), 0, 255).Get(0));
			Cmd.ComponentId    = static_cast<uint16>(C.Integer(TEXT("id"), 0, 65535).Get(0));
			Cmd.State          = static_cast<uint8>(C.Integer(TEXT("state"), 0, 255).Get(0));
			C.CheckUnknown();
			Out.Components.Add(Cmd);
		}
	}
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

TParsed<FEnvironmentRequest> ParseEnvironment(const FString& Body)
{
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, false, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FEnvironmentRequest Out;

	if (const TSharedPtr<FJsonObject> A = R.Object(TEXT("atmosphere")))
	{
		FFieldReader F(A, TEXT("atmosphere."), Error);
		FAtmosphereCommand Cmd;
		Cmd.bEnabled       = F.Bool(TEXT("enabled")).Get(true);
		Cmd.VisibilityM    = static_cast<float>(F.Number(TEXT("visibility_m"), 0.0, 1.0e6, true).Get(Cmd.VisibilityM));
		Cmd.HumidityPct    = static_cast<float>(F.Number(TEXT("humidity_pct"), 0.0, 100.0).Get(Cmd.HumidityPct));
		Cmd.AirTempC       = static_cast<float>(F.Number(TEXT("air_temp_c"), -90.0, 60.0).Get(Cmd.AirTempC));
		Cmd.HorizWindMps   = static_cast<float>(F.Number(TEXT("wind_mps"), 0.0, 100.0).Get(Cmd.HorizWindMps));
		Cmd.WindDirDeg     = static_cast<float>(F.Number(TEXT("wind_dir_deg"), 0.0, 360.0).Get(Cmd.WindDirDeg));
		Cmd.BaroPressureMb = static_cast<float>(F.Number(TEXT("pressure_mb"), 300.0, 1100.0).Get(Cmd.BaroPressureMb));
		F.CheckUnknown();
		Out.Atmosphere = Cmd;
	}
	if (const TSharedPtr<FJsonObject> C = R.Object(TEXT("clouds")))
	{
		FFieldReader F(C, TEXT("clouds."), Error);
		FWeatherCommand Cmd;
		Cmd.Scope       = FWeatherCommand::EScope::Global;
		Cmd.LayerId     = 1;
		Cmd.CoveragePct = static_cast<float>(F.Number(TEXT("coverage_pct"), 0.0, 100.0).Get(0.0));
		Cmd.bEnabled    = Cmd.CoveragePct > 0.0f;
		Cmd.BaseElevM   = static_cast<float>(F.Number(TEXT("base_m"), -500.0, 30000.0).Get(Cmd.BaseElevM));
		Cmd.ThicknessM  = static_cast<float>(F.Number(TEXT("thickness_m"), 0.0, 20000.0).Get(Cmd.ThicknessM));
		Cmd.CloudType   = static_cast<uint8>(F.Integer(TEXT("type"), 0, 9).Get(1));
		F.CheckUnknown();
		Out.Clouds = Cmd;
	}
	if (const TSharedPtr<FJsonObject> S = R.Object(TEXT("sea")))
	{
		FFieldReader F(S, TEXT("sea."), Error);
		if (const TOptional<double> B = F.Number(TEXT("beaufort"), 0.0, 12.0)) Out.Beaufort = static_cast<float>(*B);
		if (const TOptional<double> D = F.Number(TEXT("direction_deg"), 0.0, 360.0)) Out.SeaDirectionDeg = static_cast<float>(*D);
		F.CheckUnknown();
	}
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

TParsed<FCaptureRequest> ParseCapture(const FString& Body)
{
	FString Error;
	const TSharedPtr<FJsonObject> Obj = ParseObject(Body, true, Error);
	if (!Obj) return MakeError(FParseError{ Error });
	FFieldReader R(Obj, TEXT(""), Error);
	FCaptureRequest Out;
	if (const TOptional<int64> F = R.Integer(TEXT("settle_frames"), 0, 600)) Out.SettleFrames = static_cast<int32>(*F);
	Out.TimeoutSec = R.Number(TEXT("timeout_s"), 1.0, 3600.0);
	Out.bDepth = R.Bool(TEXT("depth")).Get(true);
	R.CheckUnknown();
	return Finish(Error, MoveTemp(Out));
}

FSensorCommand MergeSensor(const FControlSensorRequest& Req, bool bCurrentOn, ESensorMode CurrentMode, uint8 CurrentPolarity)
{
	FSensorCommand Out;
	Out.bOn      = Req.bOn.Get(bCurrentOn);
	Out.Waveband = Req.Mode.Get(CurrentMode);
	Out.Polarity = Req.Polarity.Get(CurrentPolarity);
	return Out;
}

FString ErrorJson(const FString& Message)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	W->WriteObjectStart();
	W->WriteValue(TEXT("error"), Message);
	W->WriteObjectEnd();
	W->Close();
	return Out;
}

FString UtcIso(const FDateTime& Utc)
{
	return Utc.ToIso8601();
}
}
