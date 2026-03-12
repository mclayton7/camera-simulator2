// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Overlay/FBitmapFont.h"

struct FCamSimTelemetry;

enum class ECrosshairStyle : uint8
{
    SimpleCross,
    MilDot,
    CircleCross,
};

/**
 * Per-element HUD config: enable, color override, and optional fixed position.
 * Color=(0,0,0,0) means "use sensor-mode default" (EO=white, IR=yellow, NVG=green).
 * X=-1 / Y=-1 means "use computed default anchor".
 */
struct FHudElementConfig
{
    bool   bEnabled = true;
    FColor Color    = FColor(0, 0, 0, 0);
    int32  X        = -1;
    int32  Y        = -1;
};

struct FHudOverlayConfig
{
    bool    bEnabled      = false;
    int32   TextScale     = 2;
    int32   EdgeMarginPx  = 10;

    ECrosshairStyle CrosshairStyle      = ECrosshairStyle::MilDot;
    FString         ClassificationText  = TEXT("UNCLASSIFIED");
    FColor          ClassificationColor = FColor(0, 200, 0, 255);
    FString         PlatformLabelText;  // 20G — empty string means don't render

    // Per-element configs (Sprint 1 elements default enabled; Sprint 2 elements default disabled)
    FHudElementConfig ElementCrosshair;
    FHudElementConfig ElementAzEl;
    FHudElementConfig ElementFov;
    FHudElementConfig ElementSlantRange;
    FHudElementConfig ElementTimestamp;
    FHudElementConfig ElementClassBanner;
    FHudElementConfig ElementCompassRose   = { false };  // 20F — off by default
    FHudElementConfig ElementPlatformLabel = { false };  // 20G — off by default
};

class FHudOverlay
{
public:
    void SetConfig(const FHudOverlayConfig& Cfg) { Config = Cfg; }
    const FHudOverlayConfig& GetConfig() const { return Config; }

    /** Load a named preset into OutCfg. Returns false and logs a warning if name unknown. */
    static bool LoadPreset(const FString& Name, FHudOverlayConfig& OutCfg);

    void Render(TArray<FColor>& Pixels, int32 W, int32 H,
                uint8 SensorMode, const FCamSimTelemetry& Telemetry,
                uint64 FrameIdx) const;

private:
    FHudOverlayConfig Config;

    FColor TextColor(uint8 SensorMode) const;
    FColor ResolveColor(const FHudElementConfig& E, uint8 SensorMode) const;

    void DrawCrosshair(TArray<FColor>& Pixels, int32 W, int32 H, uint8 SensorMode) const;
    void DrawAzElReadout(TArray<FColor>& Pixels, int32 W, int32 H,
                         uint8 SensorMode, const FCamSimTelemetry& T) const;
    void DrawFovIndicator(TArray<FColor>& Pixels, int32 W, int32 H,
                          uint8 SensorMode, const FCamSimTelemetry& T) const;
    void DrawSlantRange(TArray<FColor>& Pixels, int32 W, int32 H,
                        uint8 SensorMode, const FCamSimTelemetry& T) const;
    void DrawTimestamp(TArray<FColor>& Pixels, int32 W, int32 H,
                       uint8 SensorMode, const FCamSimTelemetry& T) const;
    void DrawClassificationBanner(TArray<FColor>& Pixels, int32 W, int32 H) const;
    void DrawCompassRose(TArray<FColor>& Pixels, int32 W, int32 H,
                         uint8 SensorMode, const FCamSimTelemetry& T) const;
    void DrawPlatformLabel(TArray<FColor>& Pixels, int32 W, int32 H,
                           uint8 SensorMode) const;
};
