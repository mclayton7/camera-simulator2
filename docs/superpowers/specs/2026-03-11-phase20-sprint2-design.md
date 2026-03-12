# Phase 20 Sprint 2 — HUD/OSD Symbology Design

**Date:** 2026-03-11
**Items:** 20F Compass Rose, 20G Platform Presets, 20H Configurable Layout

---

## Overview

Sprint 2 extends the existing `FHudOverlay` / `FHudOverlayConfig` system (Sprint 1: 20A–20E, 20I) with three features:

- **20H** Per-element config foundation (position, color, enable per element) — designed first as the base for 20F and 20G
- **20F** Linear heading tape (MQ-9 style compass rose) rendered at bottom-center
- **20G** Three named platform presets (mq9, mq1c, rq7b) loaded by name from YAML

The long-term goal is full HUD customizability via YAML. Presets (20G) are named snapshots of the same config struct introduced in 20H.

---

## Section 1: Extended Config Foundation (20H)

### FHudElementConfig

New per-element config struct added to `FHudOverlay.h`:

```cpp
struct FHudElementConfig
{
    bool   bEnabled = true;
    FColor Color    = FColor(0, 0, 0, 0); // (0,0,0,0) = use sensor-mode default
    int32  X        = -1;                 // -1 = use default anchor
    int32  Y        = -1;                 // -1 = use default anchor
};
```

`Color=(0,0,0,0)` means "fall back to sensor-mode color" (EO=white, IR=yellow, NVG=green). Explicit color overrides are used as-is regardless of sensor mode.

### FHudOverlayConfig changes

The existing `bool bXxx` element flags are replaced by `FHudElementConfig` members:

| Old field | New field |
|-----------|-----------|
| `bool bCrosshair` | `FHudElementConfig ElementCrosshair` |
| `bool bAzElReadout` | `FHudElementConfig ElementAzEl` |
| `bool bFovIndicator` | `FHudElementConfig ElementFov` |
| `bool bSlantRange` | `FHudElementConfig ElementSlantRange` |
| `bool bTimestamp` | `FHudElementConfig ElementTimestamp` |
| `bool bClassBanner` | `FHudElementConfig ElementClassBanner` |

Two new elements:

```cpp
FHudElementConfig ElementCompassRose;   // 20F — disabled by default
FHudElementConfig ElementPlatformLabel; // 20G — disabled by default
```

Top-level fields retained: `bEnabled`, `TextScale`, `EdgeMarginPx`, `CrosshairStyle`, `ClassificationText`, `ClassificationColor`.

`FString PlatformLabelText` added at top level (empty by default; presets populate it).

### YAML schema additions

```yaml
overlay:
  preset: ""                  # optional — mq9 | mq1c | rq7b; seeds defaults before other keys
  compass_rose: false         # 20F enable/disable
  compass_rose_color: ""      # empty = sensor-mode default; hex RGB e.g. "FFFFFF"
  platform_label: ""          # 20G label text; empty = disabled
  platform_label_color: ""    # empty = sensor-mode default
  # per-element position overrides (all default -1 = use anchor defaults)
  az_el_x: -1
  az_el_y: -1
  fov_x: -1
  fov_y: -1
  slant_range_x: -1
  slant_range_y: -1
  timestamp_x: -1
  timestamp_y: -1
  compass_rose_x: -1
  compass_rose_y: -1
  platform_label_x: -1
  platform_label_y: -1
```

### Env var additions

```
CAMSIM_OVERLAY_COMPASS_ROSE      (0/1)
CAMSIM_OVERLAY_PLATFORM_LABEL    (text string)
CAMSIM_OVERLAY_PRESET            (mq9 | mq1c | rq7b)
```

---

## Section 2: Compass Rose / Heading Tape (20F)

### Visual layout

```
     N         045       NE        090
 ────┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬────
                 ▼
               [034]
```

A horizontal linear heading tape anchored at bottom-center of the frame, above the timestamp row.

### Mechanics

- **Tape width:** ~40% of frame width, centered horizontally
- **Scale:** 1° = 2px at TextScale=1; visible window ≈ ±(TapeWidth/4) degrees
- **Ticks:** every 5° (short, 3px tall) and 10° (tall, 6px tall)
- **Labels:** cardinal and intercardinal (N, NE, E, SE, S, SW, W, NW) at 45° intervals; drawn above tall ticks
- **Center marker:** small downward-pointing triangle above the tape center, indicating current heading
- **Heading readout:** current heading as `[034]` or `[N]` centered below center tick
- **Wrap:** handles 360°/0° boundary correctly (no out-of-bounds writes)
- **Position:** default anchor is bottom-center, one text row above the timestamp; overridable via `compass_rose_x` / `compass_rose_y`
- **Color:** follows `FHudElementConfig.Color`; defaults to sensor-mode color

### New method

```cpp
void DrawCompassRose(TArray<FColor>& Pixels, int32 W, int32 H,
                     uint8 SensorMode, const FCamSimTelemetry& T) const;
```

Called from `Render()` when `ElementCompassRose.bEnabled`.

---

## Section 3: Platform Presets (20G)

### Preset table

| Name | Platform | Crosshair | Platform Label | Text Scale | Notes |
|------|----------|-----------|---------------|------------|-------|
| `mq9` | MQ-9 Reaper | MilDot | `"MQ-9"` | 2 | Full ISR layout; all elements on |
| `mq1c` | MQ-1C Gray Eagle | MilDot | `"MQ-1C"` | 2 | Similar to MQ-9; EdgeMarginPx=8 |
| `rq7b` | RQ-7B Shadow | SimpleCross | `"RQ-7B"` | 1 | Simpler layout; ClassBanner off by default |

Platform label anchor: top-right, one text row below the FOV indicator.

### API

```cpp
// Returns false if name is unrecognized (config left unchanged).
static bool LoadPreset(const FString& Name, FHudOverlayConfig& OutCfg);
```

Called in `CamSimConfig.cpp` after defaults are set, before individual YAML key overrides are applied:

```
1. Apply hardcoded defaults
2. If overlay.preset is set → LoadPreset() seeds config
3. Apply remaining YAML keys (can override any preset field)
4. Apply env var overrides
```

### New method in FHudOverlay

```cpp
void DrawPlatformLabel(TArray<FColor>& Pixels, int32 W, int32 H,
                       uint8 SensorMode) const;
```

Renders `Config.PlatformLabelText` using `DrawStringWithShadow` at the element's anchor (default: top-right, below FOV row).

---

## Section 4: Testing

6 new tests added to `Tests/OverlayTest.cpp` (existing: 8, total: 14):

| Test name | What it checks |
|-----------|---------------|
| `CompassRoseRendersAtBottom` | Pixels exist in bottom-center tape region when enabled |
| `CompassRoseNorthLabelVisible` | Heading=0 → "N" label pixels at center tick |
| `CompassRoseWrapThrough360` | Heading=355, no crash, pixels on both sides of center |
| `PlatformLabelRendersTopRight` | Pixels in top-right region when ElementPlatformLabel enabled |
| `PresetMq9SeedsConfig` | `LoadPreset("mq9")` → bEnabled=true, MilDot, PlatformLabelText="MQ-9" |
| `PerElementColorOverride` | Element with Color=(255,0,0,255) renders red pixels |

---

## Files Changed

| File | Change |
|------|--------|
| `Overlay/FHudOverlay.h` | Add `FHudElementConfig`; replace `bool bXxx` with element configs; add `PlatformLabelText`; add `LoadPreset()` |
| `Overlay/FHudOverlay.cpp` | `DrawCompassRose()`, `DrawPlatformLabel()`, `LoadPreset()`, update `Render()`, update element color helper |
| `Config/CamSimConfig.h` | No struct changes (FHudOverlayConfig is in FHudOverlay.h) |
| `Config/CamSimConfig.cpp` | Add preset loading; add new YAML keys; add new env vars |
| `deploy/camsim_config.yaml` | Add `preset:`, `compass_rose:`, `platform_label:` keys to `overlay:` block |
| `Tests/OverlayTest.cpp` | 6 new tests |
| `Plan.md` | Mark 20F, 20G, 20H as Sprint 2 Done |

---

## Constraints & Gotchas

- `FHudElementConfig.Color=(0,0,0,0)` is the sentinel for "use sensor-mode default" — alpha=0 is otherwise unused in the HUD
- `LoadPreset()` must be called before individual YAML field overrides, not after
- Compass tape wraps mod 360; heading values must be clamped to [0, 360) before arithmetic
- Per-element X/Y=-1 means "compute default anchor at render time from W/H/TextScale" — stored as -1 in config, resolved in each Draw method
- Existing tests must still pass: the `bool bXxx` removal requires updating `MakeOverlay()` helper in the test file
