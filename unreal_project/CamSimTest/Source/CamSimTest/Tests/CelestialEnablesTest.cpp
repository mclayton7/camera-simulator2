// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/CelestialEnables.h"
#include "Environment/CamSimEnvironment.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCelestialEnablesTest, "CamSim.CIGI.Celestial.Enables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCelestialEnablesTest::RunTest(const FString&)
{
	FCamSimConfig::FCelestialConfig Cfg;
	FCelestialEnables E = FCelestialEnables::From(Cfg, nullptr);
	TestTrue(TEXT("default stars on before a packet"), E.bStars);
	TestEqual(TEXT("default scale"), E.StarScale, 1.0f);
	Cfg.bStarsDefault = false;
	TestFalse(TEXT("config stars off"), FCelestialEnables::From(Cfg, nullptr).bStars);
	Cfg.bMoonEnabled = false;
	TestFalse(TEXT("config moon off"), FCelestialEnables::From(Cfg, nullptr).bMoon);
	Cfg = FCamSimConfig::FCelestialConfig();
	FCigiCelestialState P;   // CIGI defaults: stars off
	E = FCelestialEnables::From(Cfg, &P);
	TestFalse(TEXT("host stars off wins"), E.bStars);
	P.bStarEn = true; P.StarInt = 0.25f; P.bSunEn = false; P.bMoonEn = false;
	E = FCelestialEnables::From(Cfg, &P);
	TestTrue(TEXT("host stars on"), E.bStars);
	TestEqual(TEXT("star intensity"), E.StarScale, 0.25f);
	TestFalse(TEXT("sun off"), E.bSun);
	TestFalse(TEXT("moon off"), E.bMoon);
	P.StarInt = NAN;
	TestEqual(TEXT("NaN intensity → 1"), FCelestialEnables::From(Cfg, &P).StarScale, 1.0f);
	Cfg.bMoonEnabled = false; P.bMoonEn = true;
	TestFalse(TEXT("config moon off overrides host"), FCelestialEnables::From(Cfg, &P).bMoon);
	return true;
}

// A Celestial Sphere Control that only changes an enable or the star intensity must re-apply the sky (controller ruling:
// ApplyCelestialControl's change test includes Sun/Moon/Star Field Enable and Star Field Intensity).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCelestialChangeTest, "CamSim.CIGI.Celestial.ChangeDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCelestialChangeTest::RunTest(const FString&)
{
	const FCigiCelestialState A;
	TestTrue(TEXT("first packet is a change"), ACamSimEnvironment::IsCelestialChange(nullptr, A));
	TestFalse(TEXT("identical packet is not"), ACamSimEnvironment::IsCelestialChange(&A, A));
	auto Changed = [&](TFunctionRef<void(FCigiCelestialState&)> Edit)
	{
		FCigiCelestialState B = A;
		Edit(B);
		return ACamSimEnvironment::IsCelestialChange(&A, B);
	};
	TestTrue(TEXT("hour"),        Changed([](FCigiCelestialState& S) { S.Hour = 3; }));
	TestTrue(TEXT("minute"),      Changed([](FCigiCelestialState& S) { S.Minute = 7; }));
	TestTrue(TEXT("day"),         Changed([](FCigiCelestialState& S) { S.Day = 9; }));
	TestTrue(TEXT("month"),       Changed([](FCigiCelestialState& S) { S.Month = 11; }));
	TestTrue(TEXT("year"),        Changed([](FCigiCelestialState& S) { S.Year = 2026; }));
	TestTrue(TEXT("date valid"),  Changed([](FCigiCelestialState& S) { S.bDateVld = !S.bDateVld; }));
	TestTrue(TEXT("ephemeris"),   Changed([](FCigiCelestialState& S) { S.bEphemerisEn = !S.bEphemerisEn; }));
	TestTrue(TEXT("sun enable"),  Changed([](FCigiCelestialState& S) { S.bSunEn = !S.bSunEn; }));
	TestTrue(TEXT("moon enable"), Changed([](FCigiCelestialState& S) { S.bMoonEn = !S.bMoonEn; }));
	TestTrue(TEXT("star enable"), Changed([](FCigiCelestialState& S) { S.bStarEn = !S.bStarEn; }));
	TestTrue(TEXT("star intensity"), Changed([](FCigiCelestialState& S) { S.StarInt = 0.5f; }));
	{
		FCigiCelestialState N1 = A, N2 = A;
		N1.StarInt = NAN; N2.StarInt = NAN;
		TestFalse(TEXT("NaN star intensity twice is not a change"), ACamSimEnvironment::IsCelestialChange(&N1, N2));
	}
	return true;
}
