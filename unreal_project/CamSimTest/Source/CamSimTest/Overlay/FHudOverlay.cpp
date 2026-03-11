// Copyright CamSim Contributors. All Rights Reserved.
#include "Overlay/FHudOverlay.h"
#include "Metadata/KlvBuilder.h"

// ---------------------------------------------------------------------------
// TextColor
// ---------------------------------------------------------------------------

FColor FHudOverlay::TextColor(uint8 SensorMode) const
{
    switch (SensorMode)
    {
    case 1:  return FColor(255, 255,   0, 255); // IR — yellow
    case 2:  return FColor(  0, 255,  80, 255); // NVG — bright green
    default: return FColor(255, 255, 255, 255); // EO — white
    }
}

// ---------------------------------------------------------------------------
// Render — master dispatcher
// ---------------------------------------------------------------------------

void FHudOverlay::Render(TArray<FColor>& Pixels, int32 W, int32 H,
                         uint8 SensorMode, const FCamSimTelemetry& Telemetry,
                         uint64 FrameIdx) const
{
    if (!Config.bEnabled) return;
    if (Pixels.Num() != W * H) return;

    if (Config.bClassBanner)  DrawClassificationBanner(Pixels, W, H);
    if (Config.bCrosshair)    DrawCrosshair(Pixels, W, H, SensorMode);
    if (Config.bAzElReadout)  DrawAzElReadout(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bFovIndicator) DrawFovIndicator(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bSlantRange)   DrawSlantRange(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bTimestamp)    DrawTimestamp(Pixels, W, H, SensorMode, Telemetry);
}

// ---------------------------------------------------------------------------
// DrawCrosshair (20A)
// ---------------------------------------------------------------------------

void FHudOverlay::DrawCrosshair(TArray<FColor>& P, int32 W, int32 H, uint8 SensorMode) const
{
    const int32 CX = W / 2, CY = H / 2;
    const int32 ArmLen = 20, Gap = 4;
    const FColor C = TextColor(SensorMode);

    FBitmapFont::DrawHLine(P, W, H, CX - ArmLen, CY, ArmLen - Gap, C);
    FBitmapFont::DrawHLine(P, W, H, CX + Gap + 1, CY, ArmLen - Gap, C);
    FBitmapFont::DrawVLine(P, W, H, CX, CY - ArmLen, ArmLen - Gap, C);
    FBitmapFont::DrawVLine(P, W, H, CX, CY + Gap + 1, ArmLen - Gap, C);

    if (Config.CrosshairStyle == ECrosshairStyle::MilDot ||
        Config.CrosshairStyle == ECrosshairStyle::CircleCross)
    {
        FBitmapFont::FillRect(P, W, H, CX - 1, CY - 1, 3, 3, C);
    }

    if (Config.CrosshairStyle == ECrosshairStyle::CircleCross)
    {
        const int32 R = 20;
        int32 x = 0, y = R, d = 3 - 2 * R;
        auto Plot8 = [&](int32 px, int32 py)
        {
            FBitmapFont::SetPixel(P, W, H, CX + px, CY + py, C);
            FBitmapFont::SetPixel(P, W, H, CX - px, CY + py, C);
            FBitmapFont::SetPixel(P, W, H, CX + px, CY - py, C);
            FBitmapFont::SetPixel(P, W, H, CX - px, CY - py, C);
            FBitmapFont::SetPixel(P, W, H, CX + py, CY + px, C);
            FBitmapFont::SetPixel(P, W, H, CX - py, CY + px, C);
            FBitmapFont::SetPixel(P, W, H, CX + py, CY - px, C);
            FBitmapFont::SetPixel(P, W, H, CX - py, CY - px, C);
        };
        while (y >= x)
        {
            Plot8(x, y);
            if (d < 0) d += 4 * x + 6;
            else { --y; d += 4 * (x - y) + 10; }
            ++x;
        }
    }
}

// ---------------------------------------------------------------------------
// Stub implementations — filled in Tasks 3–6
// ---------------------------------------------------------------------------

void FHudOverlay::DrawAzElReadout(TArray<FColor>& Pixels, int32 W, int32 H,
                                  uint8 SensorMode, const FCamSimTelemetry& T) const {}

void FHudOverlay::DrawFovIndicator(TArray<FColor>& Pixels, int32 W, int32 H,
                                   uint8 SensorMode, const FCamSimTelemetry& T) const {}

void FHudOverlay::DrawSlantRange(TArray<FColor>& Pixels, int32 W, int32 H,
                                 uint8 SensorMode, const FCamSimTelemetry& T) const {}

void FHudOverlay::DrawTimestamp(TArray<FColor>& Pixels, int32 W, int32 H,
                                uint8 SensorMode, const FCamSimTelemetry& T) const {}

void FHudOverlay::DrawClassificationBanner(TArray<FColor>& Pixels, int32 W, int32 H) const {}
