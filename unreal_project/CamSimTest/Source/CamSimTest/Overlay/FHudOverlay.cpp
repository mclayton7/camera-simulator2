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

FColor FHudOverlay::ResolveColor(const FHudElementConfig& E, uint8 SensorMode) const
{
    // Alpha=0 is sentinel for "use sensor-mode default"
    return (E.Color.A > 0) ? E.Color : TextColor(SensorMode);
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
    (void)FrameIdx;

    if (Config.ElementClassBanner.bEnabled)  DrawClassificationBanner(Pixels, W, H);
    if (Config.ElementCrosshair.bEnabled)    DrawCrosshair(Pixels, W, H, SensorMode);
    if (Config.ElementAzEl.bEnabled)         DrawAzElReadout(Pixels, W, H, SensorMode, Telemetry);
    if (Config.ElementFov.bEnabled)          DrawFovIndicator(Pixels, W, H, SensorMode, Telemetry);
    if (Config.ElementSlantRange.bEnabled)   DrawSlantRange(Pixels, W, H, SensorMode, Telemetry);
    if (Config.ElementTimestamp.bEnabled)    DrawTimestamp(Pixels, W, H, SensorMode, Telemetry);
    if (Config.ElementCompassRose.bEnabled)  DrawCompassRose(Pixels, W, H, SensorMode, Telemetry);
    if (Config.ElementPlatformLabel.bEnabled) DrawPlatformLabel(Pixels, W, H, SensorMode);
}

// ---------------------------------------------------------------------------
// DrawCrosshair (20A)
// ---------------------------------------------------------------------------

void FHudOverlay::DrawCrosshair(TArray<FColor>& P, int32 W, int32 H, uint8 SensorMode) const
{
    const int32 CX = W / 2, CY = H / 2;
    const int32 ArmLen = 20, Gap = 4;
    const FColor C = ResolveColor(Config.ElementCrosshair, SensorMode);

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
// DrawAzElReadout (20B) — top-left corner
// ---------------------------------------------------------------------------

void FHudOverlay::DrawAzElReadout(TArray<FColor>& P, int32 W, int32 H,
                                  uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = ResolveColor(Config.ElementAzEl, SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;
    const int32 BannerH = Config.ElementClassBanner.bEnabled ? FBitmapFont::GlyphH * S + 4 : 0;
    const int32 LineH   = (FBitmapFont::GlyphH + 2) * S;
    const int32 DefaultX = M;
    const int32 DefaultY = M + BannerH;
    const int32 X = (Config.ElementAzEl.X >= 0) ? Config.ElementAzEl.X : DefaultX;
    const int32 Y = (Config.ElementAzEl.Y >= 0) ? Config.ElementAzEl.Y : DefaultY;

    char Buf[32];
    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "AZ: %+07.2f", (double)T.GimbalYaw);
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y, Buf, C, S);

    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "EL: %+07.2f", (double)T.GimbalPitch);
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y + LineH, Buf, C, S);
}

// ---------------------------------------------------------------------------
// DrawFovIndicator (20C) — top-right corner, right-aligned
// ---------------------------------------------------------------------------

void FHudOverlay::DrawFovIndicator(TArray<FColor>& P, int32 W, int32 H,
                                   uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = ResolveColor(Config.ElementFov, SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;
    const int32 BannerH = Config.ElementClassBanner.bEnabled ? FBitmapFont::GlyphH * S + 4 : 0;

    char Buf[32];
    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "FOV: %.1fx%.1f",
                           (double)T.HFovDeg, (double)T.VFovDeg);

    const int32 TxtW    = FBitmapFont::StringWidth(Buf, S);
    const int32 DefaultX = W - M - TxtW;
    const int32 DefaultY = M + BannerH;
    const int32 X = (Config.ElementFov.X >= 0) ? Config.ElementFov.X : DefaultX;
    const int32 Y = (Config.ElementFov.Y >= 0) ? Config.ElementFov.Y : DefaultY;
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y, Buf, C, S);
}

// ---------------------------------------------------------------------------
// DrawSlantRange (20D) — bottom-left corner
// ---------------------------------------------------------------------------

void FHudOverlay::DrawSlantRange(TArray<FColor>& P, int32 W, int32 H,
                                 uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = ResolveColor(Config.ElementSlantRange, SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;
    const int32 BannerH  = Config.ElementClassBanner.bEnabled ? FBitmapFont::GlyphH * S + 4 : 0;

    char Buf[32];
    if (T.SlantRangeM >= 1000.0)
        FCStringAnsi::Snprintf(Buf, sizeof(Buf), "R: %.2fkm", T.SlantRangeM / 1000.0);
    else
        FCStringAnsi::Snprintf(Buf, sizeof(Buf), "R: %.0fm", T.SlantRangeM);

    const int32 DefaultX = M;
    const int32 DefaultY = H - M - BannerH - FBitmapFont::GlyphH * S;
    const int32 X = (Config.ElementSlantRange.X >= 0) ? Config.ElementSlantRange.X : DefaultX;
    const int32 Y = (Config.ElementSlantRange.Y >= 0) ? Config.ElementSlantRange.Y : DefaultY;
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y, Buf, C, S);
}

// ---------------------------------------------------------------------------
// DrawTimestamp (20E) — military DTG, centered at bottom
// ---------------------------------------------------------------------------

void FHudOverlay::DrawTimestamp(TArray<FColor>& P, int32 W, int32 H,
                                uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = ResolveColor(Config.ElementTimestamp, SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;
    const int32 BannerH = Config.ElementClassBanner.bEnabled ? FBitmapFont::GlyphH * S + 4 : 0;

    const int64 TotalSec = (int64)(T.TimestampUs / 1000000ULL);
    const int32 Sec  = (int32)(TotalSec % 60);
    const int32 Min  = (int32)((TotalSec / 60) % 60);
    const int32 Hour = (int32)((TotalSec / 3600) % 24);

    // Gregorian calendar decomposition from days-since-Unix-epoch
    int64 Days = TotalSec / 86400;
    int64 Z    = Days + 719468;
    int64 Era  = (Z >= 0 ? Z : Z - 146096) / 146097;
    int64 Doe  = Z - Era * 146097;
    int64 Yoe  = (Doe - Doe/1460 + Doe/36524 - Doe/146096) / 365;
    int64 Year = Yoe + Era * 400;
    int64 Doy  = Doe - (365*Yoe + Yoe/4 - Yoe/100);
    int64 Mp   = (5*Doy + 2) / 153;
    int32 Day  = (int32)(Doy - (153*Mp + 2)/5 + 1);
    int32 Month = (int32)(Mp + (Mp < 10 ? 3 : -9));
    if (Month <= 2) ++Year;
    int32 Yr2 = (int32)(Year % 100);

    static const char* MonStr[] = {
        "","JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };
    const char* Mon = (Month >= 1 && Month <= 12) ? MonStr[Month] : "---";

    char Buf[32];
    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "%02d%02d%02dZ %02d%s%02d",
                           Hour, Min, Sec, Day, Mon, Yr2);

    const int32 TxtW     = FBitmapFont::StringWidth(Buf, S);
    const int32 DefaultX = (W - TxtW) / 2;
    const int32 DefaultY = H - M - BannerH - FBitmapFont::GlyphH * S;
    const int32 X = (Config.ElementTimestamp.X >= 0) ? Config.ElementTimestamp.X : DefaultX;
    const int32 Y = (Config.ElementTimestamp.Y >= 0) ? Config.ElementTimestamp.Y : DefaultY;
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y, Buf, C, S);
}

// ---------------------------------------------------------------------------
// DrawClassificationBanner (20E) — black bar + centered text, top and bottom
// ---------------------------------------------------------------------------

void FHudOverlay::DrawClassificationBanner(TArray<FColor>& P, int32 W, int32 H) const
{
    const int32 S = Config.TextScale;
    const int32 BannerH = FBitmapFont::GlyphH * S + 4;

    // Black background bar at top and bottom
    const FColor Black(0, 0, 0, 255);
    FBitmapFont::FillRect(P, W, H, 0, 0,           W, BannerH, Black);
    FBitmapFont::FillRect(P, W, H, 0, H - BannerH, W, BannerH, Black);

    // Centered classification text
    auto TxtCast = StringCast<ANSICHAR>(*Config.ClassificationText);
    const ANSICHAR* Txt = TxtCast.Get();
    const int32 TxtW = FBitmapFont::StringWidth(Txt, S);
    const int32 X = (W - TxtW) / 2;

    FBitmapFont::DrawString(P, W, H, X, 2,               Txt, Config.ClassificationColor, S);
    FBitmapFont::DrawString(P, W, H, X, H - BannerH + 2, Txt, Config.ClassificationColor, S);
}

// ---------------------------------------------------------------------------
// LoadPreset (20G) — stub; full implementation in Task 8
// ---------------------------------------------------------------------------

bool FHudOverlay::LoadPreset(const FString& Name, FHudOverlayConfig& OutCfg)
{
    (void)Name; (void)OutCfg;
    return false;
}

// ---------------------------------------------------------------------------
// DrawCompassRose (20F) — stub; full implementation in Task 6
// ---------------------------------------------------------------------------

void FHudOverlay::DrawCompassRose(TArray<FColor>& P, int32 W, int32 H,
                                  uint8 SensorMode, const FCamSimTelemetry& T) const
{
    (void)P; (void)W; (void)H; (void)SensorMode; (void)T;
}

// ---------------------------------------------------------------------------
// DrawPlatformLabel (20G) — stub; full implementation in Task 8
// ---------------------------------------------------------------------------

void FHudOverlay::DrawPlatformLabel(TArray<FColor>& P, int32 W, int32 H,
                                    uint8 SensorMode) const
{
    (void)P; (void)W; (void)H; (void)SensorMode;
}
