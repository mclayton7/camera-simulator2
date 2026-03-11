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

struct FHudOverlayConfig
{
    bool bEnabled            = false;
    bool bCrosshair          = true;
    bool bAzElReadout        = true;
    bool bFovIndicator       = true;
    bool bSlantRange         = true;
    bool bTimestamp          = true;
    bool bClassBanner        = true;

    ECrosshairStyle CrosshairStyle = ECrosshairStyle::MilDot;

    FString ClassificationText  = TEXT("UNCLASSIFIED");
    FColor  ClassificationColor = FColor(0, 200, 0, 255);

    int32 TextScale    = 2;
    int32 EdgeMarginPx = 10;
};

class FHudOverlay
{
public:
    void SetConfig(const FHudOverlayConfig& Cfg) { Config = Cfg; }
    const FHudOverlayConfig& GetConfig() const { return Config; }

    void Render(TArray<FColor>& Pixels, int32 W, int32 H,
                uint8 SensorMode, const FCamSimTelemetry& Telemetry,
                uint64 FrameIdx) const;

private:
    FHudOverlayConfig Config;

    FColor TextColor(uint8 SensorMode) const;

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
};
