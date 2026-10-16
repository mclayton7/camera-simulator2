// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Control/ControlApiJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// -------------------------------------------------------------------------
// Control API request parsing (ROADMAP 2.4): valid bodies become canonical
// commands; anything malformed is a 400 message, never half applied.
// -------------------------------------------------------------------------

using namespace CamSimControl;

namespace
{
	template<typename T>
	bool ExpectError(FAutomationTestBase& Test, const TParsed<T>& R, const TCHAR* Substring)
	{
		if (R.HasValue())
		{
			Test.AddError(FString::Printf(TEXT("expected an error containing '%s', got a value"), Substring));
			return false;
		}
		const FString& Msg = R.GetError().Message;
		Test.TestTrue(FString::Printf(TEXT("error '%s' contains '%s'"), *Msg, Substring), Msg.Contains(Substring));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonCameraFullTest,
	"CamSim.ControlApi.Json.CameraFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonCameraFullTest::RunTest(const FString& Parameters)
{
	const auto R = ParseCamera(TEXT(R"({"lat":33.3,"lon":-117.4,"alt":900,"heading":30,"pitch":0,"roll":0,
		"gimbal":{"yaw":10,"pitch":-45},"hfov_deg":20,"sensor":{"mode":"ir","polarity":"black_hot"}})"));
	if (!TestTrue(TEXT("parsed"), R.HasValue())) { AddError(R.GetError().Message); return false; }
	const FControlCameraCommand& C = R.GetValue();
	TestTrue(TEXT("pose"), C.Pose.IsSet());
	TestEqual(TEXT("lat"), C.Pose->Lat, 33.3);
	TestEqual(TEXT("lon"), C.Pose->Lon, -117.4);
	TestEqual(TEXT("alt"), C.Pose->Alt, 900.0);
	const FQuat Expected = CamSimFrames::CigiToNeu(30.0, 0.0, 0.0);
	TestTrue(TEXT("orientation via CigiToNeu"), C.Pose->Neu.Equals(Expected, 1e-9));
	TestTrue(TEXT("view"), C.View.IsSet());
	TestEqual(TEXT("gimbal snap"), C.View->Gimbal, FViewCommand::EGimbal::Snap);
	TestTrue(TEXT("yaw enabled"), C.View->bYawEn);
	TestTrue(TEXT("pitch enabled"), C.View->bPitchEn);
	TestFalse(TEXT("roll not given"), C.View->bRollEn);
	TestEqual(TEXT("gimbal pitch"), C.View->Pitch, -45.0f);
	TestEqual(TEXT("hfov"), C.View->HFovDeg.Get(0.f), 20.0f);
	TestTrue(TEXT("sensor"), C.Sensor.IsSet());
	TestEqual(TEXT("ir"), C.Sensor->Mode.Get(ESensorMode::EO), ESensorMode::IR);
	TestEqual(TEXT("black hot"), C.Sensor->Polarity.Get(0), static_cast<uint8>(1));
	TestFalse(TEXT("on not given: unset (keeps the current state)"), C.Sensor->bOn.IsSet());

	const auto P = ParseCamera(TEXT(R"({"sensor":{"polarity":"black_hot"}})"));
	TestTrue(TEXT("polarity-only parsed"), P.HasValue());
	if (P.HasValue()) TestFalse(TEXT("polarity only: mode unset"), P.GetValue().Sensor->Mode.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonCameraEmptyTest,
	"CamSim.ControlApi.Json.CameraEmptyBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonCameraEmptyTest::RunTest(const FString& Parameters)
{
	const auto R = ParseCamera(TEXT(""));
	TestTrue(TEXT("empty body is {}"), R.HasValue());
	if (R.HasValue())
	{
		TestFalse(TEXT("no pose"), R.GetValue().Pose.IsSet());
		TestFalse(TEXT("no view"), R.GetValue().View.IsSet());
		TestFalse(TEXT("no sensor"), R.GetValue().Sensor.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonCameraPartialTest,
	"CamSim.ControlApi.Json.CameraPartialPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonCameraPartialTest::RunTest(const FString& Parameters)
{
	ExpectError(*this, ParseCamera(TEXT(R"({"lat":1,"lon":2})")), TEXT("lat, lon and alt go together"));
	ExpectError(*this, ParseCamera(TEXT(R"({"heading":5})")), TEXT("lat, lon and alt go together"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonCameraBadValuesTest,
	"CamSim.ControlApi.Json.CameraBadValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonCameraBadValuesTest::RunTest(const FString& Parameters)
{
	ExpectError(*this, ParseCamera(TEXT(R"({"hfov_deg":"x"})")), TEXT("number"));
	ExpectError(*this, ParseCamera(TEXT(R"({"hfov_deg":0})")), TEXT("hfov_deg"));
	ExpectError(*this, ParseCamera(TEXT(R"({"hfov_deg":180})")), TEXT("hfov_deg"));
	ExpectError(*this, ParseCamera(TEXT(R"({"lat":91,"lon":0,"alt":0})")), TEXT("lat"));
	ExpectError(*this, ParseCamera(TEXT(R"({"lat":0,"lon":181,"alt":0})")), TEXT("lon"));
	ExpectError(*this, ParseCamera(TEXT(R"({"sensor":{"mode":"nvg"}})")), TEXT("sensor.mode"));
	ExpectError(*this, ParseCamera(TEXT(R"({"sensor":{"polarity":"grey"}})")), TEXT("sensor.polarity"));
	ExpectError(*this, ParseCamera(TEXT(R"({"sensor":{"on":1}})")), TEXT("sensor.on"));
	ExpectError(*this, ParseCamera(TEXT(R"({"gimbal":{"tilt":3}})")), TEXT("unknown field \"gimbal.tilt\""));
	ExpectError(*this, ParseCamera(TEXT(R"([1,2])")), TEXT("JSON object"));
	ExpectError(*this, ParseCamera(TEXT(R"({"lat":)")), TEXT("JSON object"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonUnknownFieldTest,
	"CamSim.ControlApi.Json.UnknownField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonUnknownFieldTest::RunTest(const FString& Parameters)
{
	ExpectError(*this, ParseCamera(TEXT(R"({"hdg":3})")), TEXT("unknown field \"hdg\""));
	ExpectError(*this, ParseEntity(5, TEXT(R"({"type":2001})")), TEXT("unknown field \"type\""));
	ExpectError(*this, ParseClock(TEXT(R"({"lock":true})")), TEXT("unknown field \"lock\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonEntityTest,
	"CamSim.ControlApi.Json.EntityFull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonEntityTest::RunTest(const FString& Parameters)
{
	const auto R = ParseEntity(7, TEXT(R"({"type_id":2001,"lat":33.3,"lon":-117.3,"alt":40,"heading":90,
		"surface":"ground","hidden":false,"components":[{"class":0,"id":11,"state":1}]})"));
	if (!TestTrue(TEXT("parsed"), R.HasValue())) { AddError(R.GetError().Message); return false; }
	const FEntityRequest& E = R.GetValue();
	TestEqual(TEXT("id"), E.Id, static_cast<uint16>(7));
	TestEqual(TEXT("type"), E.TypeId.Get(0), static_cast<uint16>(2001));
	TestTrue(TEXT("pose"), E.Pose.IsSet());
	TestTrue(TEXT("heading via CigiToNeu"), E.Pose->Neu.Equals(CamSimFrames::CigiToNeu(90.0, 0.0, 0.0), 1e-9));
	TestEqual(TEXT("surface"), E.Surface.Get(ESurfaceMode::None), ESurfaceMode::Ground);
	TestFalse(TEXT("not hidden"), E.bHidden.Get(true));
	TestEqual(TEXT("one component"), E.Components.Num(), 1);
	if (E.Components.Num() == 1)
	{
		TestEqual(TEXT("component id"), E.Components[0].ComponentId, static_cast<uint16>(11));
		TestEqual(TEXT("component state"), E.Components[0].State, static_cast<uint8>(1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonEntityDefaultsTest,
	"CamSim.ControlApi.Json.EntityDefaultsHpr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonEntityDefaultsTest::RunTest(const FString& Parameters)
{
	const auto R = ParseEntity(1, TEXT(R"({"lat":1,"lon":2,"alt":3})"));
	TestTrue(TEXT("parsed"), R.HasValue());
	if (R.HasValue())
	{
		TestTrue(TEXT("level, north"), R.GetValue().Pose->Neu.Equals(CamSimFrames::CigiToNeu(0.0, 0.0, 0.0), 1e-9));
		TestFalse(TEXT("no type"), R.GetValue().TypeId.IsSet());
		TestFalse(TEXT("surface unset"), R.GetValue().Surface.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonEntityBadTest,
	"CamSim.ControlApi.Json.EntityBadValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonEntityBadTest::RunTest(const FString& Parameters)
{
	ExpectError(*this, ParseEntity(0, TEXT("{}")), TEXT("entity id"));
	ExpectError(*this, ParseEntity(70000, TEXT("{}")), TEXT("entity id"));
	ExpectError(*this, ParseEntity(1, TEXT(R"({"type_id":70000})")), TEXT("type_id"));
	ExpectError(*this, ParseEntity(1, TEXT(R"({"surface":"air"})")), TEXT("surface"));
	ExpectError(*this, ParseEntity(1, TEXT(R"({"components":[{"class":0,"id":11,"state":300}]})")), TEXT("state"));
	ExpectError(*this, ParseEntity(1, TEXT(R"({"components":{}})")), TEXT("components"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonClockTest,
	"CamSim.ControlApi.Json.ClockIso",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonClockTest::RunTest(const FString& Parameters)
{
	const auto R = ParseClock(TEXT(R"({"lockstep":true,"utc":"2026-07-01T19:30:00Z","rate":2})"));
	if (!TestTrue(TEXT("parsed"), R.HasValue())) { AddError(R.GetError().Message); return false; }
	TestTrue(TEXT("lockstep"), R.GetValue().bLockstep.Get(false));
	TestEqual(TEXT("utc"), R.GetValue().Utc.Get(FDateTime()), FDateTime(2026, 7, 1, 19, 30, 0));
	TestEqual(TEXT("rate"), R.GetValue().Rate.Get(0.0), 2.0);
	ExpectError(*this, ParseClock(TEXT(R"({"utc":"yesterday"})")), TEXT("utc"));
	ExpectError(*this, ParseClock(TEXT(R"({"rate":-1})")), TEXT("rate"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonStepTest,
	"CamSim.ControlApi.Json.StepRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonStepTest::RunTest(const FString& Parameters)
{
	const auto R = ParseStep(TEXT(R"({"seconds":0.5})"));
	TestTrue(TEXT("parsed"), R.HasValue());
	if (R.HasValue()) TestEqual(TEXT("seconds"), R.GetValue().Seconds, 0.5);
	ExpectError(*this, ParseStep(TEXT(R"({"seconds":0})")), TEXT("seconds"));
	ExpectError(*this, ParseStep(TEXT(R"({"seconds":3601})")), TEXT("seconds"));
	ExpectError(*this, ParseStep(TEXT("{}")), TEXT("seconds"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonEnvironmentTest,
	"CamSim.ControlApi.Json.Environment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonEnvironmentTest::RunTest(const FString& Parameters)
{
	const auto R = ParseEnvironment(TEXT(R"({"atmosphere":{"visibility_m":20000,"humidity_pct":40,"air_temp_c":22,
		"wind_mps":4,"wind_dir_deg":270},"clouds":{"coverage_pct":40,"base_m":2000,"thickness_m":500,"type":2},
		"sea":{"beaufort":3,"direction_deg":225}})"));
	if (!TestTrue(TEXT("parsed"), R.HasValue())) { AddError(R.GetError().Message); return false; }
	const FEnvironmentRequest& E = R.GetValue();
	TestEqual(TEXT("visibility"), E.Atmosphere->VisibilityM, 20000.0f);
	TestEqual(TEXT("humidity"), E.Atmosphere->HumidityPct, 40.0f);
	TestEqual(TEXT("air temp"), E.Atmosphere->AirTempC, 22.0f);
	TestEqual(TEXT("wind"), E.Atmosphere->HorizWindMps, 4.0f);
	TestEqual(TEXT("wind dir"), E.Atmosphere->WindDirDeg, 270.0f);
	TestTrue(TEXT("clouds on"), E.Clouds->bEnabled);
	TestEqual(TEXT("clouds global"), E.Clouds->Scope, FWeatherCommand::EScope::Global);
	TestEqual(TEXT("cloud type"), E.Clouds->CloudType, static_cast<uint8>(2));
	TestEqual(TEXT("coverage"), E.Clouds->CoveragePct, 40.0f);
	TestEqual(TEXT("beaufort"), E.Beaufort.Get(-1.f), 3.0f);
	TestEqual(TEXT("sea dir"), E.SeaDirectionDeg.Get(-1.f), 225.0f);

	const auto Clear = ParseEnvironment(TEXT(R"({"clouds":{"coverage_pct":0}})"));
	TestTrue(TEXT("clear parsed"), Clear.HasValue());
	if (Clear.HasValue()) TestFalse(TEXT("0 % coverage: layer off"), Clear.GetValue().Clouds->bEnabled);

	ExpectError(*this, ParseEnvironment(TEXT(R"({"atmosphere":{"visibility_m":0}})")), TEXT("visibility_m"));
	ExpectError(*this, ParseEnvironment(TEXT(R"({"clouds":{"coverage_pct":101}})")), TEXT("coverage_pct"));
	ExpectError(*this, ParseEnvironment(TEXT(R"({"sea":{"beaufort":13}})")), TEXT("beaufort"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonCaptureTest,
	"CamSim.ControlApi.Json.Capture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonCaptureTest::RunTest(const FString& Parameters)
{
	const auto Empty = ParseCapture(TEXT(""));
	TestTrue(TEXT("empty body"), Empty.HasValue());
	if (Empty.HasValue())
	{
		TestFalse(TEXT("default settle"), Empty.GetValue().SettleFrames.IsSet());
		TestTrue(TEXT("depth by default"), Empty.GetValue().bDepth);
		TestTrue(TEXT("semantic by default"), Empty.GetValue().bSemantic);
	}
	const auto R = ParseCapture(TEXT(R"({"settle_frames":5,"timeout_s":30,"depth":false,"semantic":false})"));
	TestTrue(TEXT("parsed"), R.HasValue());
	if (R.HasValue())
	{
		TestEqual(TEXT("settle"), R.GetValue().SettleFrames.Get(-1), 5);
		TestEqual(TEXT("timeout"), R.GetValue().TimeoutSec.Get(-1.0), 30.0);
		TestFalse(TEXT("no depth"), R.GetValue().bDepth);
		TestFalse(TEXT("no semantic"), R.GetValue().bSemantic);
	}
	ExpectError(*this, ParseCapture(TEXT(R"({"settle_frames":601})")), TEXT("settle_frames"));
	ExpectError(*this, ParseCapture(TEXT(R"({"settle_frames":2.5})")), TEXT("settle_frames"));
	ExpectError(*this, ParseCapture(TEXT(R"({"timeout_s":0})")), TEXT("timeout_s"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlJsonErrorEscapeTest,
	"CamSim.ControlApi.Json.ErrorJsonEscapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FControlJsonErrorEscapeTest::RunTest(const FString& Parameters)
{
	const FString Msg = TEXT("bad \"quote\" and \\ backslash\nline");
	TSharedPtr<FJsonObject> Obj;
	TestTrue(TEXT("valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ErrorJson(Msg)), Obj) && Obj.IsValid());
	if (Obj.IsValid()) TestEqual(TEXT("round trip"), Obj->GetStringField(TEXT("error")), Msg);
	TestEqual(TEXT("utc iso"), UtcIso(FDateTime(2026, 10, 15, 18, 0, 0, 250)), FString(TEXT("2026-10-15T18:00:00.250Z")));
	return true;
}
