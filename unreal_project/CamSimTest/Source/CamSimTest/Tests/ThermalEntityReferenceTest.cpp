// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalReference.h"

// CamSim.Thermal.Entity.Reference.*: CamSimThermalRef::EntityPartTemp and its use in EvaluatePixel (spec §3).

namespace
{
	/** A valid record with an unrotated body frame at translated-world origin O (cm): Pb = ((Pw - O) / 100 with Z flipped). */
	void SetRecord(FThermalFrameParams& P, int32 S, float SkinK, const FVector3d& O)
	{
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		R[0] = FVector4f(0.01f, 0.0f, 0.0f, 0.0f);
		R[1] = FVector4f(0.0f, 0.01f, 0.0f, 0.0f);
		R[2] = FVector4f(0.0f, 0.0f, -0.01f, 0.0f);
		R[3] = FVector4f(SkinK, 2.0f, 0.0f, 1.0f);
		P.EntityOriginWorld[S] = O;
		FinalizeEntityRecords(P, FVector3d::ZeroVector);
	}

	void AddPart(FThermalFrameParams& P, int32 S, float Shape, const FVector3f& C, const FVector3f& H, float F, float T)
	{
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		const int32 K = static_cast<int32>(R[3].Z);
		R[4 + 3 * K] = FVector4f(C, Shape);
		R[5 + 3 * K] = FVector4f(H, F);
		R[6 + 3 * K] = FVector4f(T, 0.0f, 0.0f, 0.0f);
		R[3].Z = static_cast<float>(K + 1);
	}

	FVector3f BodyToWorld(const FVector3d& O, const FVector3f& Pb) { return FVector3f(O + FVector3d(Pb.X, Pb.Y, -Pb.Z) * 100.0); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityPartTempTest, "CamSim.Thermal.Entity.Reference.PartTemp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityPartTempTest::RunTest(const FString& Parameters)
{
	const FVector3d O(500.0, -200.0, -300.0);
	FThermalFrameParams P;
	SetRecord(P, 4, 290.0f, O);
	TestEqual(TEXT("no parts = skin"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(1.0f, 1.0f, 1.0f))), 290.0f);
	AddPart(P, 4, 0.0f, FVector3f(2.0f, 0.0f, -1.0f), FVector3f(0.5f, 0.5f, 0.5f), 0.2f, 350.0f);   // box
	TestNearlyEqual(TEXT("box inside"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.2f, 0.1f, -1.1f))), 350.0f, 1e-3f);
	// 0.1 m outside the +X face: w = 1 - smoothstep(0, 0.2, 0.1) = 0.5
	TestNearlyEqual(TEXT("box shell"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.6f, 0.0f, -1.0f))), 320.0f, 1e-2f);
	TestNearlyEqual(TEXT("box outside"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(3.0f, 0.0f, -1.0f))), 290.0f, 1e-3f);
	// Body Z is down: a point 1 m below the box centre's height is outside it
	TestNearlyEqual(TEXT("z is down"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.0f, 0.0f, 0.0f))), 290.0f, 1e-3f);
	AddPart(P, 4, 1.0f, FVector3f(2.0f, 0.0f, -1.0f), FVector3f(0.2f, 0.2f, 0.2f), 0.1f, 280.0f);   // cooler, later: wins in overlap
	TestNearlyEqual(TEXT("overlap: later part wins"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.0f, 0.0f, -1.0f))), 280.0f, 1e-3f);
	TestNearlyEqual(TEXT("outside the cooler part: box"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.4f, 0.0f, -1.0f))), 350.0f, 1e-3f);
	// Sphere shell 0.05 m outside (falloff 0.1): halfway from the box's 350 to the part's 280
	TestNearlyEqual(TEXT("ellipsoid shell"), CamSimThermalRef::EntityPartTemp(P, 4, BodyToWorld(O, FVector3f(2.25f, 0.0f, -1.0f))), 315.0f, 1e-2f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityEvaluateTest, "CamSim.Thermal.Entity.Reference.EvaluatePixelUsesRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityEvaluateTest::RunTest(const FString& Parameters)
{
	// An entity pixel with a valid record takes its temperature from the record; with an invalid one, 4A's stencil table.
	FThermalFrameParams P;
	P.NumClasses = 3;
	P.ClassTempK[2] = 295.0f; P.ClassEmissivity[2] = 0.9f;
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) { P.StencilClass[S] = 2; P.StencilOffsetK[S] = 8.0f; }
	P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(FRotator(-30.0, 0.0, 0.0), 60.0f, 64, 36, 10.0f);
	P.TairK = 288.15f;
	CamSimThermalRef::FPixelSample S;
	S.U = 0.5f; S.V = 0.5f; S.DeviceZ = 0.01f; S.CustomZ = 0.01f; S.Stencil = 6;
	const CamSimThermalRef::FPixelResult A = CamSimThermalRef::EvaluatePixel(P, S);
	TestEqual(TEXT("entity"), A.Class, CamSimThermalRef::EPixelClass::Entity);
	TestNearlyEqual(TEXT("invalid record: 4A class + offset"), A.TempK, 303.0f, 1e-3f);
	SetRecord(P, 6, 310.0f, FVector3d::ZeroVector);
	TestNearlyEqual(TEXT("valid record: skin"), CamSimThermalRef::EvaluatePixel(P, S).TempK, 310.0f, 1e-3f);
	// A part covering the pixel's world point
	const FVector3f Pw = CamSimThermalRef::ClipToWorld(P, 0.0f, 0.0f, 0.01f);
	AddPart(P, 6, 0.0f, FVector3f(Pw.X, Pw.Y, -Pw.Z) / 100.0f, FVector3f(0.5f, 0.5f, 0.5f), 0.2f, 400.0f);
	TestNearlyEqual(TEXT("valid record: part"), CamSimThermalRef::EvaluatePixel(P, S).TempK, 400.0f, 1e-2f);
	return true;
}
