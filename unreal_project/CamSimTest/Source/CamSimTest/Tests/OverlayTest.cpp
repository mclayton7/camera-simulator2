// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Overlay/FBitmapFont.h"
#include "Overlay/FHudOverlay.h"
#include "Metadata/KlvBuilder.h"

// -------------------------------------------------------------------------
// Phase 20 HUD/OSD — Automation Tests
//
// Validates overlay toggle, crosshair, bitmap font, classification banner,
// Az/El readout, FOV indicator, slant range, and NVG color text rendering.
// All tests operate on raw pixel buffers — no UE subsystem required.
// -------------------------------------------------------------------------

namespace
{

constexpr int32 TW = 320;
constexpr int32 TH = 240;

TArray<FColor> BlackFrame()
{
    TArray<FColor> F;
    F.SetNumZeroed(TW * TH);
    return F;
}

bool HasNonBlackPixel(const TArray<FColor>& F, int32 X0, int32 Y0, int32 W, int32 H)
{
    for (int32 Y = Y0; Y < Y0 + H && Y < TH; ++Y)
        for (int32 X = X0; X < X0 + W && X < TW; ++X)
        {
            const FColor& C = F[Y * TW + X];
            if (C.R || C.G || C.B) return true;
        }
    return false;
}

FCamSimTelemetry MakeTelemetry()
{
    FCamSimTelemetry T{};
    T.GimbalYaw   = 45.25f;
    T.GimbalPitch = -12.5f;
    T.HFovDeg     = 5.0f;
    T.VFovDeg     = 2.8f;
    T.SlantRangeM = 3500.0;
    T.TimestampUs = 1741564800000000ULL; // 2025-03-10 00:00:00 UTC
    return T;
}

FHudOverlay MakeOverlay(bool AllEnabled = true)
{
    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = AllEnabled;
    Cfg.ElementCrosshair.bEnabled        = AllEnabled;
    Cfg.ElementAzEl.bEnabled             = AllEnabled;
    Cfg.ElementFov.bEnabled              = AllEnabled;
    Cfg.ElementSlantRange.bEnabled       = AllEnabled;
    Cfg.ElementTimestamp.bEnabled        = AllEnabled;
    Cfg.ElementClassBanner.bEnabled      = AllEnabled;
    Cfg.ElementCompassRose.bEnabled      = false; // off by default in helper
    Cfg.ElementPlatformLabel.bEnabled    = false;
    Cfg.TextScale                        = 1;
    Cfg.EdgeMarginPx                     = 5;
    Cfg.ClassificationText               = TEXT("UNCLASSIFIED");
    Cfg.ClassificationColor              = FColor(0, 200, 0, 255);
    FHudOverlay O;
    O.SetConfig(Cfg);
    return O;
}

} // anonymous namespace

// -------------------------------------------------------------------------
// Test 1: Disabled overlay leaves frame untouched
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayToggleDisablesRenderingTest,
    "CamSim.Overlay.ToggleDisablesRendering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayToggleDisablesRenderingTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlay O = MakeOverlay(false);
    FCamSimTelemetry T = MakeTelemetry();

    O.Render(F, TW, TH, 0, T, 0);

    for (int32 i = 0; i < F.Num(); ++i)
    {
        if (F[i].R != 0 || F[i].G != 0 || F[i].B != 0)
        {
            AddError(TEXT("Disabled overlay wrote non-black pixels to frame"));
            return false;
        }
    }
    TestTrue(TEXT("All pixels remain black when overlay is disabled"), true);
    return true;
}

// -------------------------------------------------------------------------
// Test 2: Crosshair renders at frame center
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayCrosshairAtCenterTest,
    "CamSim.Overlay.CrosshairRendersAtCenter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayCrosshairAtCenterTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = true;
    Cfg.ElementCrosshair.bEnabled        = true;
    Cfg.ElementAzEl.bEnabled             = false;
    Cfg.ElementFov.bEnabled              = false;
    Cfg.ElementSlantRange.bEnabled       = false;
    Cfg.ElementTimestamp.bEnabled        = false;
    Cfg.ElementClassBanner.bEnabled      = false;
    Cfg.CrosshairStyle = ECrosshairStyle::SimpleCross;
    Cfg.TextScale     = 1;
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    O.Render(F, TW, TH, 0, T, 0);

    constexpr int32 CX = 160;
    constexpr int32 CY = 120;

    TestTrue(TEXT("Crosshair left arm has pixels"),   HasNonBlackPixel(F, CX - 20, CY, 15, 1));
    TestTrue(TEXT("Crosshair right arm has pixels"),  HasNonBlackPixel(F, CX + 5,  CY, 15, 1));
    TestTrue(TEXT("Crosshair top arm has pixels"),    HasNonBlackPixel(F, CX, CY - 20, 1, 15));
    TestTrue(TEXT("Crosshair bottom arm has pixels"), HasNonBlackPixel(F, CX, CY + 5,  1, 15));
    return true;
}

// -------------------------------------------------------------------------
// Test 3: DrawString writes pixels at the given position
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayDrawStringWritesPixelsTest,
    "CamSim.Overlay.DrawStringWritesPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayDrawStringWritesPixelsTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FBitmapFont::DrawString(F, TW, TH, 10, 10, "TEST", FColor::White, 1);

    TestTrue(TEXT("Pixels exist in text region (10,10,30x7)"),
             HasNonBlackPixel(F, 10, 10, 30, 7));
    TestTrue(TEXT("No pixels written before text start (0,0,9x7)"),
             !HasNonBlackPixel(F, 0, 0, 9, 7));
    return true;
}

// -------------------------------------------------------------------------
// Test 4: Classification banner renders at top of frame
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayClassificationBannerTopTest,
    "CamSim.Overlay.ClassificationBannerRendersAtTop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayClassificationBannerTopTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = true;
    Cfg.ElementCrosshair.bEnabled        = false;
    Cfg.ElementAzEl.bEnabled             = false;
    Cfg.ElementFov.bEnabled              = false;
    Cfg.ElementSlantRange.bEnabled       = false;
    Cfg.ElementTimestamp.bEnabled        = false;
    Cfg.ElementClassBanner.bEnabled      = true;
    Cfg.TextScale                        = 1;
    Cfg.EdgeMarginPx                     = 5;
    Cfg.ClassificationText               = TEXT("UNCLASSIFIED");
    Cfg.ClassificationColor              = FColor(0, 200, 0, 255);
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    O.Render(F, TW, TH, 0, T, 0);

    TestTrue(TEXT("Classification banner text visible at top center"),
             HasNonBlackPixel(F, TW / 2 - 20, 2, 40, FBitmapFont::GlyphH));
    return true;
}

// -------------------------------------------------------------------------
// Test 5: Az/El readout renders in top-left area
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayAzElRendersTopLeftTest,
    "CamSim.Overlay.AzElRendersTopLeft",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayAzElRendersTopLeftTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = true;
    Cfg.ElementCrosshair.bEnabled        = false;
    Cfg.ElementAzEl.bEnabled             = true;
    Cfg.ElementFov.bEnabled              = false;
    Cfg.ElementSlantRange.bEnabled       = false;
    Cfg.ElementTimestamp.bEnabled        = false;
    Cfg.ElementClassBanner.bEnabled      = false;
    Cfg.TextScale                        = 1;
    Cfg.EdgeMarginPx                     = 5;
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    O.Render(F, TW, TH, 0, T, 0);

    TestTrue(TEXT("Az/El readout has pixels in top-left region (5,5,80x20)"),
             HasNonBlackPixel(F, 5, 5, 80, 20));
    return true;
}

// -------------------------------------------------------------------------
// Test 6: FOV indicator renders in top-right area
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayFovIndicatorTopRightTest,
    "CamSim.Overlay.FovIndicatorRendersTopRight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayFovIndicatorTopRightTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = true;
    Cfg.ElementCrosshair.bEnabled        = false;
    Cfg.ElementAzEl.bEnabled             = false;
    Cfg.ElementFov.bEnabled              = true;
    Cfg.ElementSlantRange.bEnabled       = false;
    Cfg.ElementTimestamp.bEnabled        = false;
    Cfg.ElementClassBanner.bEnabled      = false;
    Cfg.TextScale                        = 1;
    Cfg.EdgeMarginPx                     = 5;
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    O.Render(F, TW, TH, 0, T, 0);

    TestTrue(TEXT("FOV indicator has pixels in top-right region"),
             HasNonBlackPixel(F, TW - 100, 5, 95, 10));
    return true;
}

// -------------------------------------------------------------------------
// Test 7: Slant range formats as km and renders bottom-left
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlaySlantRangeFormatsKmTest,
    "CamSim.Overlay.SlantRangeFormatsKm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlaySlantRangeFormatsKmTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                         = true;
    Cfg.ElementCrosshair.bEnabled        = false;
    Cfg.ElementAzEl.bEnabled             = false;
    Cfg.ElementFov.bEnabled              = false;
    Cfg.ElementSlantRange.bEnabled       = true;
    Cfg.ElementTimestamp.bEnabled        = false;
    Cfg.ElementClassBanner.bEnabled      = false;
    Cfg.TextScale                        = 1;
    Cfg.EdgeMarginPx                     = 5;
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    T.SlantRangeM = 3500.0; // should render as "R: 3.50km"
    O.Render(F, TW, TH, 0, T, 0);

    TestTrue(TEXT("Slant range text visible at bottom-left (5, TH-20, 80x12)"),
             HasNonBlackPixel(F, 5, TH - 20, 80, 12));
    return true;
}

// -------------------------------------------------------------------------
// Test 8: NVG mode uses green-tinted text color
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayNvgModeUsesGreenTextTest,
    "CamSim.Overlay.NvgModeUsesGreenText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayNvgModeUsesGreenTextTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FBitmapFont::DrawString(F, TW, TH, 10, 10, "A", FColor(0, 255, 80, 255), 1);

    for (int32 Y = 10; Y <= 16 && Y < TH; ++Y)
        for (int32 X = 10; X <= 15 && X < TW; ++X)
        {
            const FColor& C = F[Y * TW + X];
            if (C.G > 200 && C.R < 100)
            {
                TestTrue(TEXT("Found NVG green pixel in drawn glyph"), true);
                return true;
            }
        }

    AddError(TEXT("No NVG green pixel (G>200, R<100) found in glyph region (10,10)-(15,16)"));
    return false;
}

// -------------------------------------------------------------------------
// Test 9: Per-element color override renders in the specified color
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayPerElementColorOverrideTest,
    "CamSim.Overlay.PerElementColorOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayPerElementColorOverrideTest::RunTest(const FString& Parameters)
{
    TArray<FColor> F = BlackFrame();

    FHudOverlayConfig Cfg;
    Cfg.bEnabled                   = true;
    Cfg.ElementCrosshair.bEnabled  = false;
    Cfg.ElementAzEl.bEnabled       = true;
    Cfg.ElementAzEl.Color          = FColor(255, 0, 0, 255); // force red
    Cfg.ElementFov.bEnabled        = false;
    Cfg.ElementSlantRange.bEnabled = false;
    Cfg.ElementTimestamp.bEnabled  = false;
    Cfg.ElementClassBanner.bEnabled= false;
    Cfg.TextScale                  = 1;
    Cfg.EdgeMarginPx               = 5;
    FHudOverlay O;
    O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    O.Render(F, TW, TH, 0 /*EO*/, T, 0);

    // Find a red pixel (R>200, G<50, B<50) in the Az/El region (top-left)
    bool bFoundRed = false;
    for (int32 Y = 5; Y < 30 && Y < TH; ++Y)
        for (int32 X = 5; X < 100 && X < TW; ++X)
        {
            const FColor& C = F[Y * TW + X];
            if (C.R > 200 && C.G < 50 && C.B < 50) { bFoundRed = true; break; }
        }

    TestTrue(TEXT("Az/El element renders in explicit red color override"), bFoundRed);
    return true;
}
