// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Control/ControlApiAdapter.h"
#include "Control/ControlApiJson.h"
#include "Sim/CommandSink.h"

// -------------------------------------------------------------------------
// FControlApiAdapter (ROADMAP 2.4): API entities in their own namespace,
// re-submitted every tick like a host at frame rate; removals and component
// changes once.
// -------------------------------------------------------------------------

namespace
{
	struct FRecordingSink : ISimCommandSink
	{
		TArray<FEntityCommand> Entities;
		TArray<FComponentCommand> Components;
		virtual void Submit(const FEntityCommand& C) override { Entities.Add(C); }
		virtual void Submit(const FEntityMotionCommand&) override {}
		virtual void Submit(const FArticulationCommand&) override {}
		virtual void Submit(const FComponentCommand& C) override { Components.Add(C); }
	};

	CamSimControl::FEntityRequest Parse(FAutomationTestBase& Test, uint32 Id, const TCHAR* Body)
	{
		const auto R = CamSimControl::ParseEntity(Id, Body);
		if (!R.HasValue())
		{
			Test.AddError(R.GetError().Message);
			return {};
		}
		return R.GetValue();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterNewEntityTest,
	"CamSim.ControlApi.Adapter.NewEntityNeedsTypeAndPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterNewEntityTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	FString Err;
	TestFalse(TEXT("no type"), A.PutEntity(Parse(*this, 1, TEXT(R"({"lat":1,"lon":2,"alt":3})")), Err));
	TestTrue(TEXT("names type_id"), Err.Contains(TEXT("type_id")));
	Err.Reset();
	TestFalse(TEXT("no position"), A.PutEntity(Parse(*this, 1, TEXT(R"({"type_id":2001})")), Err));
	TestTrue(TEXT("names lat"), Err.Contains(TEXT("lat")));
	TestEqual(TEXT("nothing stored"), A.GetIds().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterResubmitTest,
	"CamSim.ControlApi.Adapter.ResubmitsEveryPoll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterResubmitTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	FString Err;
	TestTrue(TEXT("put"), A.PutEntity(Parse(*this, 7, TEXT(R"({"type_id":2001,"lat":33,"lon":-117,"alt":40,"surface":"ground"})")), Err));
	FRecordingSink S;
	A.Poll(S);
	A.Poll(S);
	TestEqual(TEXT("two commands"), S.Entities.Num(), 2);
	if (S.Entities.Num() == 2)
	{
		TestEqual(TEXT("key api:7"), S.Entities[0].Key.ToString(), FString(TEXT("api:7")));
		TestEqual(TEXT("type"), S.Entities[1].TypeId, static_cast<uint16>(2001));
		TestEqual(TEXT("lat"), S.Entities[1].Pose.Lat, 33.0);
		TestEqual(TEXT("active"), S.Entities[1].Lifecycle, EEntityLifecycle::Active);
		TestEqual(TEXT("surface ground"), S.Entities[1].SurfaceMode, ESurfaceMode::Ground);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterPartialTest,
	"CamSim.ControlApi.Adapter.PartialUpdateKeepsState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterPartialTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	FString Err;
	A.PutEntity(Parse(*this, 2, TEXT(R"({"type_id":3001,"lat":33,"lon":-117,"alt":0,"surface":"water"})")), Err);
	TestTrue(TEXT("hide only"), A.PutEntity(Parse(*this, 2, TEXT(R"({"hidden":true})")), Err));
	FRecordingSink S;
	A.Poll(S);
	TestEqual(TEXT("one command"), S.Entities.Num(), 1);
	if (S.Entities.Num() == 1)
	{
		TestEqual(TEXT("hidden"), S.Entities[0].Lifecycle, EEntityLifecycle::Hidden);
		TestEqual(TEXT("type kept"), S.Entities[0].TypeId, static_cast<uint16>(3001));
		TestEqual(TEXT("lat kept"), S.Entities[0].Pose.Lat, 33.0);
		TestEqual(TEXT("surface kept"), S.Entities[0].SurfaceMode, ESurfaceMode::Water);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterRemoveTest,
	"CamSim.ControlApi.Adapter.RemoveSubmitsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterRemoveTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	FString Err;
	A.PutEntity(Parse(*this, 3, TEXT(R"({"type_id":2001,"lat":1,"lon":2,"alt":3})")), Err);
	A.PutEntity(Parse(*this, 4, TEXT(R"({"type_id":2001,"lat":1,"lon":2,"alt":3})")), Err);
	TestTrue(TEXT("remove known"), A.RemoveEntity(3));
	TestFalse(TEXT("remove unknown"), A.RemoveEntity(99));
	FRecordingSink S;
	A.Poll(S);
	int32 Removes = 0, Actives = 0;
	for (const FEntityCommand& C : S.Entities) (C.Lifecycle == EEntityLifecycle::Remove ? Removes : Actives)++;
	TestEqual(TEXT("one remove"), Removes, 1);
	TestEqual(TEXT("one active"), Actives, 1);
	S.Entities.Reset();
	A.RemoveAll();
	A.Poll(S);
	TestEqual(TEXT("remove all: one remove for 4"), S.Entities.Num(), 1);
	S.Entities.Reset();
	A.Poll(S);
	TestEqual(TEXT("then nothing"), S.Entities.Num(), 0);
	TestEqual(TEXT("no ids"), A.GetIds().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterComponentsTest,
	"CamSim.ControlApi.Adapter.ComponentsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterComponentsTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	FString Err;
	A.PutEntity(Parse(*this, 5, TEXT(R"({"type_id":2001,"lat":1,"lon":2,"alt":3,"components":[{"class":0,"id":11,"state":1}]})")), Err);
	FRecordingSink S;
	A.Poll(S);
	A.Poll(S);
	TestEqual(TEXT("component once"), S.Components.Num(), 1);
	if (S.Components.Num() == 1)
	{
		TestEqual(TEXT("component key"), S.Components[0].Key.ToString(), FString(TEXT("api:5")));
		TestEqual(TEXT("component id"), S.Components[0].ComponentId, static_cast<uint16>(11));
	}
	TestEqual(TEXT("entity submitted before its components each time"), S.Entities.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdapterCommandFrameTest,
	"CamSim.ControlApi.Adapter.CommandFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAdapterCommandFrameTest::RunTest(const FString& Parameters)
{
	FControlApiAdapter A;
	TestFalse(TEXT("nothing yet"), A.WasCommandedThisFrame(10));
	A.NoteCommand(10);
	TestTrue(TEXT("this frame"), A.WasCommandedThisFrame(10));
	TestFalse(TEXT("next frame"), A.WasCommandedThisFrame(11));
	FString Err;
	A.PutEntity(Parse(*this, 6, TEXT(R"({"type_id":2001,"lat":1,"lon":2,"alt":3})")), Err);
	TestTrue(TEXT("a put notes the current frame"), A.WasCommandedThisFrame(GFrameCounter));
	return true;
}
