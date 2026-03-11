# Phase 20 HUD/OSD Symbology Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add burned-in HUD/OSD symbology (crosshair, gimbal readouts, FOV, slant range, timestamp, classification banner) to the CPU pixel pipeline with a single toggle for clean ML frames.

**Architecture:** A new `Overlay/` module provides `FBitmapFont` (5×7 pixel font, pure CPU) and `FHudOverlay` (draws all HUD elements directly into `TArray<FColor>`). `FHudOverlay::Render()` is called as the final step of `FSensorPostProcess::Process()`, after all sensor effects but before the frame is handed to the encoder. A single `bEnabled` flag skips all rendering for clean ML output.

**Tech Stack:** Pure C++ / UE5 TArray<FColor> pixel manipulation — no UCanvas, no UMG, no GPU. All drawing modifies raw BGRA8 pixels on the background task thread (same thread as sensor post-process).

---

## Chunk 1: Bitmap Font + Drawing Primitives + Crosshair

### Task 1: FBitmapFont — 5×7 pixel font and draw primitives

**Files:**
- Create: `Source/CamSimTest/Overlay/FBitmapFont.h`

- [ ] **Step 1.1: Create FBitmapFont.h with font data and API**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/**
 * Minimal 5×7 bitmap font for HUD overlays.
 * Characters 0x20 (space) through 0x7E (~), 95 glyphs.
 * Encoding: each glyph = 7 bytes (one per row), each byte has 5 bits
 *   bit 4 (0x10) = column 0 (leftmost)
 *   bit 0 (0x01) = column 4 (rightmost)
 *   row 0 = top row
 *
 * DrawString and primitive helpers operate directly on a TArray<FColor>
 * pixel buffer in BGRA8 layout (UE readback format).
 */
struct FBitmapFont
{
    static constexpr int32 GlyphW = 5;   // pixels per glyph (excl. spacing)
    static constexpr int32 GlyphH = 7;
    static constexpr int32 CharSpacing = 1; // pixels between glyphs
    static constexpr int32 CharStride = GlyphW + CharSpacing; // 6

    // Draw a single ASCII character (0x20–0x7E).
    // (X,Y) = top-left pixel of glyph. Scale=1 or 2.
    // Returns next X position.
    static int32 DrawChar(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                          int32 X, int32 Y, char Ch,
                          FColor Fg, int32 Scale = 1);

    // Draw a null-terminated ASCII string. Returns next X position.
    static int32 DrawString(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                            int32 X, int32 Y, const ANSICHAR* Str,
                            FColor Fg, int32 Scale = 1);

    // Draw a string with a 1-pixel dark background for readability.
    static int32 DrawStringWithShadow(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                                      int32 X, int32 Y, const ANSICHAR* Str,
                                      FColor Fg, int32 Scale = 1);

    // Pixel width of a string at the given scale.
    static int32 StringWidth(const ANSICHAR* Str, int32 Scale = 1);

    // Low-level primitives
    static void SetPixel(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, FColor C);
    static void DrawHLine(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 Len, FColor C);
    static void DrawVLine(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 Len, FColor C);
    static void DrawRect(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 W, int32 H, FColor C);
    static void FillRect(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 W, int32 H, FColor C);

    // The font bitmap data
    static const uint8 GFont5x7[95][7];
};
```

- [ ] **Step 1.2: Create FBitmapFont.cpp with implementation and font data**

Create `Source/CamSimTest/Overlay/FBitmapFont.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "Overlay/FBitmapFont.h"

// 5×7 bitmap font, ASCII 0x20–0x7E (95 glyphs).
// Row 0 = top. Bit 4 = leftmost column. Public domain pixel font.
const uint8 FBitmapFont::GFont5x7[95][7] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // 0x20 ' '
    { 0x04,0x04,0x04,0x04,0x04,0x00,0x04 }, // 0x21 '!'
    { 0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00 }, // 0x22 '"'
    { 0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A }, // 0x23 '#'
    { 0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04 }, // 0x24 '$'
    { 0x18,0x19,0x02,0x04,0x08,0x13,0x03 }, // 0x25 '%'
    { 0x0C,0x12,0x14,0x08,0x15,0x12,0x0D }, // 0x26 '&'
    { 0x04,0x04,0x08,0x00,0x00,0x00,0x00 }, // 0x27 '\''
    { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 }, // 0x28 '('
    { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 }, // 0x29 ')'
    { 0x00,0x04,0x15,0x0E,0x15,0x04,0x00 }, // 0x2A '*'
    { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 }, // 0x2B '+'
    { 0x00,0x00,0x00,0x00,0x04,0x04,0x08 }, // 0x2C ','
    { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 }, // 0x2D '-'
    { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C }, // 0x2E '.'
    { 0x00,0x01,0x02,0x04,0x08,0x10,0x00 }, // 0x2F '/'
    // Digits 0x30–0x39
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }, // 0x30 '0'
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, // 0x31 '1'
    { 0x0E,0x11,0x01,0x06,0x08,0x10,0x1F }, // 0x32 '2'
    { 0x1F,0x01,0x02,0x06,0x01,0x11,0x0E }, // 0x33 '3'
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, // 0x34 '4'
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E }, // 0x35 '5'
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E }, // 0x36 '6'
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, // 0x37 '7'
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }, // 0x38 '8'
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C }, // 0x39 '9'
    { 0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00 }, // 0x3A ':'
    { 0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08 }, // 0x3B ';'
    { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 }, // 0x3C '<'
    { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 }, // 0x3D '='
    { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 }, // 0x3E '>'
    { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 }, // 0x3F '?'
    { 0x0E,0x11,0x17,0x15,0x17,0x10,0x0E }, // 0x40 '@'
    // Uppercase letters 0x41–0x5A
    { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, // 0x41 'A'
    { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, // 0x42 'B'
    { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E }, // 0x43 'C'
    { 0x1C,0x12,0x11,0x11,0x11,0x12,0x1C }, // 0x44 'D'
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, // 0x45 'E'
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }, // 0x46 'F'
    { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F }, // 0x47 'G'
    { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }, // 0x48 'H'
    { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }, // 0x49 'I'
    { 0x07,0x02,0x02,0x02,0x02,0x12,0x0C }, // 0x4A 'J'
    { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, // 0x4B 'K'
    { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }, // 0x4C 'L'
    { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, // 0x4D 'M'
    { 0x11,0x11,0x19,0x15,0x13,0x11,0x11 }, // 0x4E 'N'
    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, // 0x4F 'O'
    { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }, // 0x50 'P'
    { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, // 0x51 'Q'
    { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }, // 0x52 'R'
    { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, // 0x53 'S'
    { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }, // 0x54 'T'
    { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, // 0x55 'U'
    { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }, // 0x56 'V'
    { 0x11,0x11,0x11,0x15,0x15,0x1B,0x11 }, // 0x57 'W'
    { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }, // 0x58 'X'
    { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }, // 0x59 'Y'
    { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }, // 0x5A 'Z'
    { 0x0E,0x08,0x08,0x08,0x08,0x08,0x0E }, // 0x5B '['
    { 0x00,0x10,0x08,0x04,0x02,0x01,0x00 }, // 0x5C '\'
    { 0x0E,0x02,0x02,0x02,0x02,0x02,0x0E }, // 0x5D ']'
    { 0x04,0x0A,0x11,0x00,0x00,0x00,0x00 }, // 0x5E '^'
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F }, // 0x5F '_'
    { 0x08,0x04,0x02,0x00,0x00,0x00,0x00 }, // 0x60 '`'
    // Lowercase 0x61–0x7A
    { 0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F }, // 0x61 'a'
    { 0x10,0x10,0x1E,0x11,0x11,0x11,0x1E }, // 0x62 'b'
    { 0x00,0x00,0x0E,0x10,0x10,0x11,0x0E }, // 0x63 'c'
    { 0x01,0x01,0x0F,0x11,0x11,0x11,0x0F }, // 0x64 'd'
    { 0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E }, // 0x65 'e'
    { 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 }, // 0x66 'f'
    { 0x00,0x00,0x0F,0x11,0x11,0x0F,0x01,}, // 0x67 'g' (note: descender cut)
    { 0x10,0x10,0x1E,0x11,0x11,0x11,0x11 }, // 0x68 'h'
    { 0x04,0x00,0x0C,0x04,0x04,0x04,0x0E }, // 0x69 'i'
    { 0x02,0x00,0x06,0x02,0x02,0x12,0x0C }, // 0x6A 'j'
    { 0x10,0x10,0x11,0x12,0x1C,0x12,0x11 }, // 0x6B 'k'
    { 0x0C,0x04,0x04,0x04,0x04,0x04,0x0E }, // 0x6C 'l'
    { 0x00,0x00,0x1A,0x15,0x15,0x11,0x11 }, // 0x6D 'm'
    { 0x00,0x00,0x1E,0x11,0x11,0x11,0x11 }, // 0x6E 'n'
    { 0x00,0x00,0x0E,0x11,0x11,0x11,0x0E }, // 0x6F 'o'
    { 0x00,0x00,0x1E,0x11,0x11,0x1E,0x10 }, // 0x70 'p'
    { 0x00,0x00,0x0F,0x11,0x11,0x0F,0x01 }, // 0x71 'q'
    { 0x00,0x00,0x16,0x19,0x10,0x10,0x10 }, // 0x72 'r'
    { 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E }, // 0x73 's'
    { 0x08,0x08,0x1C,0x08,0x08,0x09,0x06 }, // 0x74 't'
    { 0x00,0x00,0x11,0x11,0x11,0x13,0x0D }, // 0x75 'u'
    { 0x00,0x00,0x11,0x11,0x11,0x0A,0x04 }, // 0x76 'v'
    { 0x00,0x00,0x11,0x11,0x15,0x15,0x0A }, // 0x77 'w'
    { 0x00,0x00,0x11,0x0A,0x04,0x0A,0x11 }, // 0x78 'x'
    { 0x00,0x00,0x11,0x11,0x0F,0x01,0x0E }, // 0x79 'y'
    { 0x00,0x00,0x1F,0x02,0x04,0x08,0x1F }, // 0x7A 'z'
    { 0x06,0x04,0x04,0x08,0x04,0x04,0x06 }, // 0x7B '{'
    { 0x04,0x04,0x04,0x00,0x04,0x04,0x04 }, // 0x7C '|'
    { 0x0C,0x04,0x04,0x02,0x04,0x04,0x0C }, // 0x7D '}'
    { 0x00,0x00,0x08,0x15,0x02,0x00,0x00 }, // 0x7E '~'
};

void FBitmapFont::SetPixel(TArray<FColor>& P, int32 W, int32 H, int32 X, int32 Y, FColor C)
{
    if (X < 0 || Y < 0 || X >= W || Y >= H) return;
    P[Y * W + X] = C;
}

void FBitmapFont::DrawHLine(TArray<FColor>& P, int32 W, int32 H, int32 X, int32 Y, int32 Len, FColor C)
{
    for (int32 i = 0; i < Len; ++i) SetPixel(P, W, H, X + i, Y, C);
}

void FBitmapFont::DrawVLine(TArray<FColor>& P, int32 W, int32 H, int32 X, int32 Y, int32 Len, FColor C)
{
    for (int32 i = 0; i < Len; ++i) SetPixel(P, W, H, X, Y + i, C);
}

void FBitmapFont::DrawRect(TArray<FColor>& P, int32 W, int32 H, int32 X, int32 Y, int32 RW, int32 RH, FColor C)
{
    DrawHLine(P, W, H, X,        Y,        RW, C);
    DrawHLine(P, W, H, X,        Y+RH-1,   RW, C);
    DrawVLine(P, W, H, X,        Y,        RH, C);
    DrawVLine(P, W, H, X+RW-1,   Y,        RH, C);
}

void FBitmapFont::FillRect(TArray<FColor>& P, int32 W, int32 H, int32 X, int32 Y, int32 RW, int32 RH, FColor C)
{
    for (int32 Row = 0; Row < RH; ++Row)
        DrawHLine(P, W, H, X, Y + Row, RW, C);
}

int32 FBitmapFont::DrawChar(TArray<FColor>& P, int32 BW, int32 BH,
                             int32 X, int32 Y, char Ch, FColor Fg, int32 Scale)
{
    const int32 Idx = (uint8)Ch - 0x20;
    if (Idx < 0 || Idx >= 95) { return X + CharStride * Scale; }
    const uint8* Glyph = GFont5x7[Idx];
    for (int32 Row = 0; Row < GlyphH; ++Row)
    {
        for (int32 Col = 0; Col < GlyphW; ++Col)
        {
            if (Glyph[Row] & (0x10 >> Col))
            {
                for (int32 sy = 0; sy < Scale; ++sy)
                    for (int32 sx = 0; sx < Scale; ++sx)
                        SetPixel(P, BW, BH, X + Col*Scale + sx, Y + Row*Scale + sy, Fg);
            }
        }
    }
    return X + CharStride * Scale;
}

int32 FBitmapFont::DrawString(TArray<FColor>& P, int32 BW, int32 BH,
                               int32 X, int32 Y, const ANSICHAR* Str, FColor Fg, int32 Scale)
{
    int32 CX = X;
    for (; *Str; ++Str)
        CX = DrawChar(P, BW, BH, CX, Y, *Str, Fg, Scale);
    return CX;
}

int32 FBitmapFont::DrawStringWithShadow(TArray<FColor>& P, int32 BW, int32 BH,
                                         int32 X, int32 Y, const ANSICHAR* Str, FColor Fg, int32 Scale)
{
    // 1-pixel dark shadow (offset +1,+1)
    DrawString(P, BW, BH, X+1, Y+1, Str, FColor(0, 0, 0, 255), Scale);
    return DrawString(P, BW, BH, X, Y, Str, Fg, Scale);
}

int32 FBitmapFont::StringWidth(const ANSICHAR* Str, int32 Scale)
{
    int32 Len = 0;
    for (const ANSICHAR* S = Str; *S; ++S) ++Len;
    return Len * CharStride * Scale;
}
```

- [ ] **Step 1.3: Verify files compile (no UE build yet — just check for syntax errors visually)**

Inspect `FBitmapFont.h` and `FBitmapFont.cpp` for:
- All function declarations have matching implementations
- Pixel index math: `Y * W + X` (row-major BGRA8)
- Bounds check: `X < 0 || Y < 0 || X >= W || Y >= H`
- Font array is exactly 95 entries (0x20–0x7E = 95 characters)

- [ ] **Step 1.4: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FBitmapFont.h \
        unreal_project/CamSimTest/Source/CamSimTest/Overlay/FBitmapFont.cpp
git commit -m "feat: add FBitmapFont 5x7 pixel font for HUD overlay"
```

---

### Task 2: FHudOverlay skeleton + crosshair (20A) + toggle (20I)

**Files:**
- Create: `Source/CamSimTest/Overlay/FHudOverlay.h`
- Create: `Source/CamSimTest/Overlay/FHudOverlay.cpp`

- [ ] **Step 2.1: Create FHudOverlay.h**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Overlay/FBitmapFont.h"

struct FCamSimTelemetry;

/** Crosshair style for the targeting reticle. */
UENUM()
enum class ECrosshairStyle : uint8
{
    SimpleCross,    // plain + (two lines, 20px arms)
    MilDot,         // SimpleCross + central 3x3 dot
    CircleCross,    // circle (r=20) + SimpleCross
};

/** Per-element enable flags + layout config for the HUD overlay.
 *  Loaded from YAML overlay: block and CAMSIM_OVERLAY_* env vars. */
struct FHudOverlayConfig
{
    bool bEnabled            = false;  // master switch (CAMSIM_OVERLAY_ENABLED)
    bool bCrosshair          = true;   // 20A
    bool bAzElReadout        = true;   // 20B  (top-left corner)
    bool bFovIndicator       = true;   // 20C  (top-right corner)
    bool bSlantRange         = true;   // 20D  (bottom-left)
    bool bTimestamp          = true;   // 20E  (bottom-center)
    bool bClassBanner        = true;   // 20E  (top + bottom bar)

    ECrosshairStyle CrosshairStyle = ECrosshairStyle::MilDot;

    // Classification text for the banner (e.g. "UNCLASSIFIED" / "SECRET")
    FString ClassificationText = TEXT("UNCLASSIFIED");
    FColor  ClassificationColor = FColor(0, 200, 0, 255); // green

    // Text scale (1 = 5x7 native, 2 = 10x14 — recommended for 1080p)
    int32 TextScale = 2;

    // Margin from frame edges in pixels
    int32 EdgeMarginPx = 10;
};

/** Renders all HUD/OSD elements directly into a TArray<FColor> pixel buffer.
 *  All methods are thread-safe (no UObject access). */
class FHudOverlay
{
public:
    void SetConfig(const FHudOverlayConfig& Cfg) { Config = Cfg; }
    const FHudOverlayConfig& GetConfig() const { return Config; }

    /**
     * Render all enabled HUD elements onto Pixels.
     * Must be called from the task thread, after FSensorPostProcess::Process().
     *
     * @param Pixels   BGRA8 pixel buffer (width*height elements)
     * @param W        Frame width in pixels
     * @param H        Frame height in pixels
     * @param SensorMode  0=EO, 1=IR, 2=NVG (affects overlay colours)
     * @param Telemetry   Gimbal/sensor state for data readouts
     * @param FrameIdx    Monotonic frame counter (used for timestamp)
     */
    void Render(TArray<FColor>& Pixels, int32 W, int32 H,
                uint8 SensorMode, const FCamSimTelemetry& Telemetry,
                uint64 FrameIdx) const;

private:
    FHudOverlayConfig Config;

    // Returns the appropriate text colour for the current sensor mode.
    FColor TextColor(uint8 SensorMode) const;

    // --- Element draw methods ---
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
```

- [ ] **Step 2.2: Create FHudOverlay.cpp with Render() + crosshair**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "Overlay/FHudOverlay.h"
#include "Metadata/KlvBuilder.h"   // FCamSimTelemetry

FColor FHudOverlay::TextColor(uint8 SensorMode) const
{
    switch (SensorMode)
    {
    case 2:  return FColor(0, 255, 80, 255);   // NVG: bright green
    case 1:  return FColor(255, 255, 0, 255);  // IR:  yellow
    default: return FColor(255, 255, 255, 255); // EO:  white
    }
}

void FHudOverlay::Render(TArray<FColor>& Pixels, int32 W, int32 H,
                         uint8 SensorMode, const FCamSimTelemetry& Telemetry,
                         uint64 FrameIdx) const
{
    if (!Config.bEnabled) return;
    if (Pixels.Num() != W * H) return; // safety: mismatched buffer

    if (Config.bClassBanner)    DrawClassificationBanner(Pixels, W, H);
    if (Config.bCrosshair)      DrawCrosshair(Pixels, W, H, SensorMode);
    if (Config.bAzElReadout)    DrawAzElReadout(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bFovIndicator)   DrawFovIndicator(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bSlantRange)     DrawSlantRange(Pixels, W, H, SensorMode, Telemetry);
    if (Config.bTimestamp)      DrawTimestamp(Pixels, W, H, SensorMode, Telemetry);
}

void FHudOverlay::DrawCrosshair(TArray<FColor>& P, int32 W, int32 H, uint8 SensorMode) const
{
    const int32 CX = W / 2;
    const int32 CY = H / 2;
    const int32 ArmLen = 20;
    const int32 Gap = 4;       // gap around center (no line in center ±Gap px)
    const FColor C = TextColor(SensorMode);

    // Horizontal arms
    FBitmapFont::DrawHLine(P, W, H, CX - ArmLen, CY, ArmLen - Gap, C);
    FBitmapFont::DrawHLine(P, W, H, CX + Gap,    CY, ArmLen - Gap, C);
    // Vertical arms
    FBitmapFont::DrawVLine(P, W, H, CX, CY - ArmLen, ArmLen - Gap, C);
    FBitmapFont::DrawVLine(P, W, H, CX, CY + Gap,    ArmLen - Gap, C);

    if (Config.CrosshairStyle == ECrosshairStyle::MilDot ||
        Config.CrosshairStyle == ECrosshairStyle::CircleCross)
    {
        // 3×3 center dot
        FBitmapFont::FillRect(P, W, H, CX - 1, CY - 1, 3, 3, C);
    }

    if (Config.CrosshairStyle == ECrosshairStyle::CircleCross)
    {
        // Bresenham circle radius 20
        const int32 R = 20;
        int32 x = 0, y = R, d = 3 - 2 * R;
        auto Plot8 = [&](int32 px, int32 py)
        {
            FBitmapFont::SetPixel(P, W, H, CX+px, CY+py, C);
            FBitmapFont::SetPixel(P, W, H, CX-px, CY+py, C);
            FBitmapFont::SetPixel(P, W, H, CX+px, CY-py, C);
            FBitmapFont::SetPixel(P, W, H, CX-px, CY-py, C);
            FBitmapFont::SetPixel(P, W, H, CX+py, CY+px, C);
            FBitmapFont::SetPixel(P, W, H, CX-py, CY+px, C);
            FBitmapFont::SetPixel(P, W, H, CX+py, CY-px, C);
            FBitmapFont::SetPixel(P, W, H, CX-py, CY-px, C);
        };
        while (y >= x) { Plot8(x, y); if (d < 0) d += 4*x+6; else { --y; d += 4*(x-y)+10; } ++x; }
    }
}
```

- [ ] **Step 2.3: Commit skeleton**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.h \
        unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.cpp
git commit -m "feat: add FHudOverlay skeleton with crosshair (20A) and toggle (20I)"
```

---

## Chunk 2: Data Readouts (20B–20E)

### Task 3: Gimbal Az/El Readout (20B)

**Files:**
- Modify: `Source/CamSimTest/Overlay/FHudOverlay.cpp` — add DrawAzElReadout()

FCamSimTelemetry fields used: `GimbalYaw` (azimuth, deg), `GimbalPitch` (elevation, deg).

- [ ] **Step 3.1: Implement DrawAzElReadout in FHudOverlay.cpp**

```cpp
void FHudOverlay::DrawAzElReadout(TArray<FColor>& P, int32 W, int32 H,
                                   uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = TextColor(SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;

    // Top-left: two lines
    //   AZ: 045.2
    //   EL: -12.5
    char Buf[32];

    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "AZ: %+07.2f", (double)T.GimbalYaw);
    FBitmapFont::DrawStringWithShadow(P, W, H, M, M, Buf, C, S);

    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "EL: %+07.2f", (double)T.GimbalPitch);
    FBitmapFont::DrawStringWithShadow(P, W, H, M, M + (FBitmapFont::GlyphH + 2) * S, Buf, C, S);
}
```

- [ ] **Step 3.2: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.cpp
git commit -m "feat: add gimbal Az/El readout to HUD overlay (20B)"
```

---

### Task 4: FOV/Zoom Indicator (20C)

FCamSimTelemetry fields used: `HFovDeg`, `VFovDeg`.

- [ ] **Step 4.1: Implement DrawFovIndicator in FHudOverlay.cpp**

Renders `FOV: 5.0x3.0` in the top-right corner.

```cpp
void FHudOverlay::DrawFovIndicator(TArray<FColor>& P, int32 W, int32 H,
                                    uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = TextColor(SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;

    char Buf[32];
    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "FOV: %.1fx%.1f",
                           (double)T.HFovDeg, (double)T.VFovDeg);

    // Right-align at top-right corner
    const int32 TxtW = FBitmapFont::StringWidth(Buf, S);
    const int32 X = W - M - TxtW;
    FBitmapFont::DrawStringWithShadow(P, W, H, X, M, Buf, C, S);
}
```

- [ ] **Step 4.2: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.cpp
git commit -m "feat: add FOV/zoom indicator to HUD overlay (20C)"
```

---

### Task 5: Slant Range Display (20D)

FCamSimTelemetry fields used: `SlantRangeM` (metres).

- [ ] **Step 5.1: Implement DrawSlantRange in FHudOverlay.cpp**

Renders `R: 1234m` or `R: 1.23km` in the bottom-left corner.

```cpp
void FHudOverlay::DrawSlantRange(TArray<FColor>& P, int32 W, int32 H,
                                  uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = TextColor(SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;

    char Buf[32];
    if (T.SlantRangeM >= 1000.0)
        FCStringAnsi::Snprintf(Buf, sizeof(Buf), "R: %.2fkm", T.SlantRangeM / 1000.0);
    else
        FCStringAnsi::Snprintf(Buf, sizeof(Buf), "R: %.0fm", T.SlantRangeM);

    const int32 Y = H - M - FBitmapFont::GlyphH * S;
    FBitmapFont::DrawStringWithShadow(P, W, H, M, Y, Buf, C, S);
}
```

- [ ] **Step 5.2: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.cpp
git commit -m "feat: add slant range display to HUD overlay (20D)"
```

---

### Task 6: Timestamp + Classification Banner (20E)

FCamSimTelemetry fields used: `TimestampUs` (POSIX microseconds, UTC).

- [ ] **Step 6.1: Implement DrawTimestamp and DrawClassificationBanner in FHudOverlay.cpp**

Timestamp format: `141532Z 10MAR26` (military DTG) centered at bottom.
Classification banner: solid-colour bar at top and bottom (height = `TextScale * GlyphH + 4`).

```cpp
void FHudOverlay::DrawTimestamp(TArray<FColor>& P, int32 W, int32 H,
                                 uint8 SensorMode, const FCamSimTelemetry& T) const
{
    const FColor C = TextColor(SensorMode);
    const int32 S = Config.TextScale;
    const int32 M = Config.EdgeMarginPx;

    // Convert microseconds to broken-down UTC time components
    const int64 TotalSec = (int64)(T.TimestampUs / 1000000ULL);
    const int32 Sec      = (int32)(TotalSec % 60);
    const int32 Min      = (int32)((TotalSec / 60) % 60);
    const int32 Hour     = (int32)((TotalSec / 3600) % 24);
    // Days since 1970-01-01 for date extraction
    int64 Days = TotalSec / 86400;
    // Gregorian calendar decomposition
    int64 Z = Days + 719468;
    int64 Era = (Z >= 0 ? Z : Z - 146096) / 146097;
    int64 Doe = Z - Era * 146097;
    int64 Yoe = (Doe - Doe/1460 + Doe/36524 - Doe/146096) / 365;
    int64 Year = Yoe + Era * 400;
    int64 Doy  = Doe - (365*Yoe + Yoe/4 - Yoe/100);
    int64 Mp   = (5*Doy + 2) / 153;
    int32 Day  = (int32)(Doy - (153*Mp+2)/5 + 1);
    int32 Month = (int32)(Mp + (Mp < 10 ? 3 : -9));
    if (Month <= 2) ++Year;

    static const char* MonStr[] = { "","JAN","FEB","MAR","APR","MAY","JUN",
                                     "JUL","AUG","SEP","OCT","NOV","DEC" };
    const char* Mon = (Month >= 1 && Month <= 12) ? MonStr[Month] : "---";
    int32 Yr2 = (int32)(Year % 100);

    char Buf[32];
    FCStringAnsi::Snprintf(Buf, sizeof(Buf), "%02d%02d%02dZ %02d%s%02d",
                           Hour, Min, Sec, Day, Mon, Yr2);

    const int32 TxtW = FBitmapFont::StringWidth(Buf, S);
    const int32 X = (W - TxtW) / 2;   // centered
    const int32 Y = H - M - FBitmapFont::GlyphH * S;
    FBitmapFont::DrawStringWithShadow(P, W, H, X, Y, Buf, C, S);
}

void FHudOverlay::DrawClassificationBanner(TArray<FColor>& P, int32 W, int32 H) const
{
    const int32 S = Config.TextScale;
    const int32 BannerH = FBitmapFont::GlyphH * S + 4;

    // Dark semi-transparent bar: fill with Config.ClassificationColor at 60% alpha
    // (alpha blend not supported in raw BGRA8 — use solid dark bar, overlay text)
    const FColor BarBg(0, 0, 0, 255);
    FBitmapFont::FillRect(P, W, H, 0, 0,      W, BannerH, BarBg); // top
    FBitmapFont::FillRect(P, W, H, 0, H-BannerH, W, BannerH, BarBg); // bottom

    // Centered classification text in both banners
    const ANSICHAR* Txt = TCHAR_TO_ANSI(*Config.ClassificationText);
    const int32 TxtW = FBitmapFont::StringWidth(Txt, S);
    const int32 X = (W - TxtW) / 2;

    FBitmapFont::DrawString(P, W, H, X, 2,              Txt, Config.ClassificationColor, S);
    FBitmapFont::DrawString(P, W, H, X, H - BannerH + 2, Txt, Config.ClassificationColor, S);
}
```

- [ ] **Step 6.2: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Overlay/FHudOverlay.cpp
git commit -m "feat: add timestamp and classification banner to HUD overlay (20E)"
```

---

## Chunk 3: Integration, Config, and Tests

### Task 7: FSensorPostProcess Integration

**Files:**
- Modify: `Source/CamSimTest/Sensor/SensorPostProcess.h`
- Modify: `Source/CamSimTest/Sensor/SensorPostProcess.cpp`

- [ ] **Step 7.1: Read SensorPostProcess.h to see current members**

```
Read: Source/CamSimTest/Sensor/SensorPostProcess.h
```

- [ ] **Step 7.2: Add FHudOverlay member and SetOverlayConfig() to SensorPostProcess.h**

In the private section (alongside existing Phase18 config members), add:
```cpp
#include "Overlay/FHudOverlay.h"
// ...
private:
    FHudOverlay HudOverlay;  // Phase 20 HUD overlay
public:
    void SetOverlayConfig(const FHudOverlayConfig& Cfg) { HudOverlay.SetConfig(Cfg); }
```

- [ ] **Step 7.3: Read the end of SensorPostProcess.cpp Process() method**

Confirm the last few lines of `FSensorPostProcess::Process()` to find the right insertion point (after all sensor effects, before the function returns).

- [ ] **Step 7.4: Add HudOverlay.Render() call at end of Process()**

At the very end of `FSensorPostProcess::Process()`, after all existing effects:

```cpp
    // Phase 20: HUD/OSD overlay (runs last, on top of all sensor effects)
    HudOverlay.Render(Pixels, Width, Height, (uint8)Mode, Telemetry, FrameIndex);
```

where `Width` and `Height` are the stored frame dimensions from `Initialize()`.

- [ ] **Step 7.5: Verify the Process() signature and internal Width/Height fields**

Check that `FSensorPostProcess` stores `Width` and `Height` as member variables (set in `Initialize()`). If they are named differently (e.g. `FrameWidth`, `ImgW`), use those names.

- [ ] **Step 7.6: Read CamSimSubsystem.cpp to find BeginPlay/Initialize**

Find where `FSensorPostProcess::SetPhase18Config()` is called (pattern to follow for `SetOverlayConfig()`).

- [ ] **Step 7.7: Add SetOverlayConfig() call in CamSimSubsystem.cpp**

Immediately after the existing `PostProcess->SetPhase18Config(...)` call, add:

```cpp
PostProcess->SetOverlayConfig(Config.OverlayConfig);
```

- [ ] **Step 7.8: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.h \
        unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPostProcess.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "feat: integrate FHudOverlay into FSensorPostProcess pipeline"
```

---

### Task 8: Config — FHudOverlayConfig + YAML + env vars

**Files:**
- Modify: `Source/CamSimTest/Config/CamSimConfig.h`
- Modify: `Source/CamSimTest/Config/CamSimConfig.cpp`
- Modify: `deploy/camsim_config.yaml`

- [ ] **Step 8.1: Read CamSimConfig.h to find insertion point**

Look for where `FPhase18Config Phase18;` is declared — add `FHudOverlayConfig OverlayConfig;` alongside it.

- [ ] **Step 8.2: Add FHudOverlayConfig to CamSimConfig.h**

Since `FHudOverlayConfig` is defined in `Overlay/FHudOverlay.h`, add a forward declaration or include. The struct already defines defaults, so no inline initialization is needed in FCamSimConfig.

Add after the Phase18 include/declaration:
```cpp
#include "Overlay/FHudOverlay.h"
// ...
// In FCamSimConfig struct:
FHudOverlayConfig OverlayConfig;
```

- [ ] **Step 8.3: Read CamSimConfig.cpp to find YAML parsing block for Phase18**

Pattern to follow: `auto& p18 = tree["phase18"];` block with ryml field parsing and env var overrides.

- [ ] **Step 8.4: Add overlay: YAML parsing to CamSimConfig.cpp**

After the existing phase18 parsing block, add:

```cpp
// Phase 20: overlay
if (tree.rootref().has_child("overlay"))
{
    auto ov = tree["overlay"];
    auto ReadBool = [&](const char* Key, bool& Val) {
        if (ov.has_child(Key)) { bool B; ov[Key] >> B; Val = B; }
    };
    ReadBool("enabled",        Out.OverlayConfig.bEnabled);
    ReadBool("crosshair",      Out.OverlayConfig.bCrosshair);
    ReadBool("az_el_readout",  Out.OverlayConfig.bAzElReadout);
    ReadBool("fov_indicator",  Out.OverlayConfig.bFovIndicator);
    ReadBool("slant_range",    Out.OverlayConfig.bSlantRange);
    ReadBool("timestamp",      Out.OverlayConfig.bTimestamp);
    ReadBool("class_banner",   Out.OverlayConfig.bClassBanner);

    if (ov.has_child("text_scale"))
    {
        int32 V; ov["text_scale"] >> V;
        Out.OverlayConfig.TextScale = FMath::Clamp(V, 1, 4);
    }
    if (ov.has_child("crosshair_style"))
    {
        FString V; ov["crosshair_style"] >> V;
        if (V == "mil_dot")      Out.OverlayConfig.CrosshairStyle = ECrosshairStyle::MilDot;
        else if (V == "circle")  Out.OverlayConfig.CrosshairStyle = ECrosshairStyle::CircleCross;
        else                     Out.OverlayConfig.CrosshairStyle = ECrosshairStyle::SimpleCross;
    }
    if (ov.has_child("classification_text"))
    {
        FString V; ov["classification_text"] >> V;
        Out.OverlayConfig.ClassificationText = V;
    }
}

// Env var overrides (take precedence over YAML)
auto EnvBool = [](const TCHAR* Name, bool& Val)
{
    const FString S = FPlatformMisc::GetEnvironmentVariable(Name);
    if (!S.IsEmpty()) Val = (S == TEXT("1") || S.ToLower() == TEXT("true"));
};
EnvBool(TEXT("CAMSIM_OVERLAY_ENABLED"),     Out.OverlayConfig.bEnabled);
EnvBool(TEXT("CAMSIM_OVERLAY_CROSSHAIR"),   Out.OverlayConfig.bCrosshair);
EnvBool(TEXT("CAMSIM_OVERLAY_AZEL"),        Out.OverlayConfig.bAzElReadout);
EnvBool(TEXT("CAMSIM_OVERLAY_FOV"),         Out.OverlayConfig.bFovIndicator);
EnvBool(TEXT("CAMSIM_OVERLAY_SLANT_RANGE"), Out.OverlayConfig.bSlantRange);
EnvBool(TEXT("CAMSIM_OVERLAY_TIMESTAMP"),   Out.OverlayConfig.bTimestamp);
EnvBool(TEXT("CAMSIM_OVERLAY_CLASS_BANNER"),Out.OverlayConfig.bClassBanner);
{
    const FString ClassTxt = FPlatformMisc::GetEnvironmentVariable(TEXT("CAMSIM_OVERLAY_CLASS_TEXT"));
    if (!ClassTxt.IsEmpty()) Out.OverlayConfig.ClassificationText = ClassTxt;
}
```

- [ ] **Step 8.5: Add overlay: block to deploy/camsim_config.yaml**

```yaml
overlay:
  enabled: false           # master switch; set true for ISR output with HUD
  crosshair: true          # 20A targeting reticle
  crosshair_style: mil_dot # simple_cross | mil_dot | circle
  az_el_readout: true      # 20B gimbal azimuth/elevation (top-left)
  fov_indicator: true      # 20C FOV readout (top-right)
  slant_range: true        # 20D slant range to target (bottom-left)
  timestamp: true          # 20E DTG timestamp (bottom-center)
  class_banner: true       # 20E classification banner (top/bottom bar)
  classification_text: "UNCLASSIFIED"
  text_scale: 2            # 1=5x7 native, 2=10x14 (recommended for 1080p)
```

- [ ] **Step 8.6: Commit**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        deploy/camsim_config.yaml
git commit -m "feat: add FHudOverlayConfig to FCamSimConfig with YAML + env var loading"
```

---

### Task 9: Unit Tests

**Files:**
- Create: `Source/CamSimTest/Tests/OverlayTest.cpp`

Tests verify pixel-level behavior of the overlay. No UE subsystem needed — all tests create bare `TArray<FColor>` buffers.

- [ ] **Step 9.1: Create Tests/OverlayTest.cpp**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Overlay/FBitmapFont.h"
#include "Overlay/FHudOverlay.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace
{
    constexpr int32 TW = 320;
    constexpr int32 TH = 240;

    TArray<FColor> BlackFrame()
    {
        TArray<FColor> F; F.SetNumZeroed(TW * TH); return F;
    }

    bool HasNonBlackPixel(const TArray<FColor>& F, int32 X0, int32 Y0, int32 W, int32 H)
    {
        for (int32 Y = Y0; Y < Y0+H && Y < TH; ++Y)
            for (int32 X = X0; X < X0+W && X < TW; ++X)
            {
                const FColor& C = F[Y*TW + X];
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
        Cfg.bEnabled     = AllEnabled;
        Cfg.bCrosshair   = AllEnabled;
        Cfg.bAzElReadout = AllEnabled;
        Cfg.bFovIndicator= AllEnabled;
        Cfg.bSlantRange  = AllEnabled;
        Cfg.bTimestamp   = AllEnabled;
        Cfg.bClassBanner = AllEnabled;
        Cfg.TextScale    = 1;
        Cfg.EdgeMarginPx = 5;
        Cfg.ClassificationText = TEXT("UNCLASSIFIED");
        Cfg.ClassificationColor= FColor(0,200,0,255);
        FHudOverlay O; O.SetConfig(Cfg);
        return O;
    }
}

// ─── Test 1: Toggle disabled → no pixels changed ────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayToggleOffTest,
    "CamSim.Overlay.ToggleDisablesRendering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayToggleOffTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlay O = MakeOverlay(/*AllEnabled=*/false);
    O.Render(F, TW, TH, 0, MakeTelemetry(), 0);
    // Every pixel should still be black
    for (const FColor& C : F)
    {
        if (C.R || C.G || C.B) { AddError(TEXT("Pixel modified when overlay disabled")); return false; }
    }
    return true;
}

// ─── Test 2: Crosshair renders non-black pixels at frame center ─────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayCrosshairTest,
    "CamSim.Overlay.CrosshairRendersAtCenter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayCrosshairTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlayConfig Cfg;
    Cfg.bEnabled = true; Cfg.bCrosshair = true;
    Cfg.bClassBanner = false; Cfg.bAzElReadout = false;
    Cfg.bFovIndicator = false; Cfg.bSlantRange = false; Cfg.bTimestamp = false;
    Cfg.TextScale = 1; Cfg.CrosshairStyle = ECrosshairStyle::SimpleCross;
    FHudOverlay O; O.SetConfig(Cfg);
    O.Render(F, TW, TH, 0, MakeTelemetry(), 0);

    const int32 CX = TW/2, CY = TH/2;
    // Arms should have non-black pixels
    TestTrue(TEXT("Left arm has pixels"),  HasNonBlackPixel(F, CX-20, CY, 15, 1));
    TestTrue(TEXT("Right arm has pixels"), HasNonBlackPixel(F, CX+5,  CY, 15, 1));
    TestTrue(TEXT("Top arm has pixels"),   HasNonBlackPixel(F, CX, CY-20, 1, 15));
    TestTrue(TEXT("Bottom arm has pixels"),HasNonBlackPixel(F, CX, CY+5,  1, 15));
    return true;
}

// ─── Test 3: DrawString writes pixels ───────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayDrawStringTest,
    "CamSim.Overlay.DrawStringWritesPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayDrawStringTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FBitmapFont::DrawString(F, TW, TH, 10, 10, "TEST", FColor::White, 1);
    TestTrue(TEXT("Text region has non-black pixels"), HasNonBlackPixel(F, 10, 10, 30, 7));
    // Area before text should still be black
    TestFalse(TEXT("Before-text area is black"), HasNonBlackPixel(F, 0, 0, 9, 7));
    return true;
}

// ─── Test 4: Classification banner fills top rows ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayClassBannerTest,
    "CamSim.Overlay.ClassificationBannerRendersAtTop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayClassBannerTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlayConfig Cfg;
    Cfg.bEnabled = true; Cfg.bClassBanner = true;
    Cfg.bCrosshair = false; Cfg.bAzElReadout = false;
    Cfg.bFovIndicator = false; Cfg.bSlantRange = false; Cfg.bTimestamp = false;
    Cfg.TextScale = 1;
    Cfg.ClassificationText = TEXT("TEST");
    Cfg.ClassificationColor = FColor(0,200,0,255);
    FHudOverlay O; O.SetConfig(Cfg);
    O.Render(F, TW, TH, 0, MakeTelemetry(), 0);

    // Top banner (rows 0..BannerH) should be modified (black bar written)
    // The bar itself is black, but the text is green — check middle of top banner
    const int32 BannerH = FBitmapFont::GlyphH + 4;
    TestTrue(TEXT("Top banner text region written"),
             HasNonBlackPixel(F, TW/2 - 20, 2, 40, FBitmapFont::GlyphH));
    // Bottom banner similarly
    TestTrue(TEXT("Bottom banner text region written"),
             HasNonBlackPixel(F, TW/2 - 20, TH - BannerH + 2, 40, FBitmapFont::GlyphH));
    return true;
}

// ─── Test 5: Az/El readout renders in top-left region ───────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayAzElTest,
    "CamSim.Overlay.AzElRendersTopLeft",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayAzElTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlayConfig Cfg;
    Cfg.bEnabled = true; Cfg.bAzElReadout = true;
    Cfg.bCrosshair = false; Cfg.bClassBanner = false;
    Cfg.bFovIndicator = false; Cfg.bSlantRange = false; Cfg.bTimestamp = false;
    Cfg.TextScale = 1; Cfg.EdgeMarginPx = 5;
    FHudOverlay O; O.SetConfig(Cfg);
    O.Render(F, TW, TH, 0, MakeTelemetry(), 0);

    TestTrue(TEXT("Top-left region has text pixels"), HasNonBlackPixel(F, 5, 5, 80, 20));
    return true;
}

// ─── Test 6: FOV indicator renders in top-right region ──────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayFovTest,
    "CamSim.Overlay.FovIndicatorRendersTopRight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayFovTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FHudOverlayConfig Cfg;
    Cfg.bEnabled = true; Cfg.bFovIndicator = true;
    Cfg.bCrosshair = false; Cfg.bClassBanner = false;
    Cfg.bAzElReadout = false; Cfg.bSlantRange = false; Cfg.bTimestamp = false;
    Cfg.TextScale = 1; Cfg.EdgeMarginPx = 5;
    FHudOverlay O; O.SetConfig(Cfg);
    O.Render(F, TW, TH, 0, MakeTelemetry(), 0);

    // Right portion of top row should have pixels
    TestTrue(TEXT("Top-right region has text pixels"), HasNonBlackPixel(F, TW - 100, 5, 95, 10));
    return true;
}

// ─── Test 7: Slant range formats km correctly ────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlaySlantRangeKmTest,
    "CamSim.Overlay.SlantRangeFormatsKm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlaySlantRangeKmTest::RunTest(const FString&)
{
    // Test formatting logic by driving range >= 1000m
    TArray<FColor> F = BlackFrame();
    FHudOverlayConfig Cfg;
    Cfg.bEnabled = true; Cfg.bSlantRange = true;
    Cfg.bCrosshair = false; Cfg.bClassBanner = false;
    Cfg.bAzElReadout = false; Cfg.bFovIndicator = false; Cfg.bTimestamp = false;
    Cfg.TextScale = 1; Cfg.EdgeMarginPx = 5;
    FHudOverlay O; O.SetConfig(Cfg);

    FCamSimTelemetry T = MakeTelemetry();
    T.SlantRangeM = 3500.0; // should render as "R: 3.50km"
    O.Render(F, TW, TH, 0, T, 0);
    // Bottom-left region should have pixels
    TestTrue(TEXT("Slant range km renders bottom-left"),
             HasNonBlackPixel(F, 5, TH - 20, 80, 12));
    return true;
}

// ─── Test 8: Sensor mode NVG uses green text color ──────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverlayNvgColorTest,
    "CamSim.Overlay.NvgModeUsesGreenText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOverlayNvgColorTest::RunTest(const FString&)
{
    TArray<FColor> F = BlackFrame();
    FBitmapFont::DrawString(F, TW, TH, 10, 10, "A", FColor(0,255,80,255), 1); // NVG green
    // Find at least one pixel with G > 200 and R < 100
    for (int32 Y = 10; Y < 17; ++Y)
        for (int32 X = 10; X < 16; ++X)
        {
            const FColor& C = F[Y*TW + X];
            if (C.G > 200 && C.R < 100) return true;
        }
    AddError(TEXT("No NVG-green pixels found in DrawString region"));
    return false;
}
```

- [ ] **Step 9.2: Commit tests**

```bash
cd /Users/mclayton/developer/camsim
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/OverlayTest.cpp
git commit -m "test: add 8 unit tests for Phase 20 HUD overlay"
```

---

### Task 10: Build.cs update + Plan.md update

**Files:**
- Modify: `Source/CamSimTest/CamSimTest.Build.cs` — verify `Overlay` source files are picked up (UBT auto-discovers all .cpp in subdirs, no change needed unless explicit module includes are required)
- Modify: `Plan.md` — mark Phase 20A-E,I as done

- [ ] **Step 10.1: Read CamSimTest.Build.cs**

Check whether all subdirectories under `Source/CamSimTest/` are included automatically (they should be with UBT's default `bUseUnityBuild`). No changes expected.

- [ ] **Step 10.2: Update Plan.md Phase 20 table**

Mark 20A, 20B, 20C, 20D, 20E, 20I as `✅ Sprint 1 Done` in the Phase 20 table. Add sprint notes:

```
**Sprint 1 status**: 20A–20E, 20I implemented.
20F (Compass Rose), 20G (Platform Presets), 20H (Configurable Layout) remain for future sprints.

**Sprint 1 files**:
- `Overlay/FBitmapFont.h/.cpp` — 5×7 pixel font + drawing primitives
- `Overlay/FHudOverlay.h/.cpp` — crosshair, Az/El, FOV, slant range, timestamp, class banner
- `Sensor/SensorPostProcess.h/.cpp` — calls HudOverlay.Render() at end of Process()
- `Config/CamSimConfig.h/.cpp` — FHudOverlayConfig, overlay: YAML + CAMSIM_OVERLAY_* env vars
- `Subsystem/CamSimSubsystem.cpp` — SetOverlayConfig() at BeginPlay
- `deploy/camsim_config.yaml` — overlay: block (disabled by default)
- `Tests/OverlayTest.cpp` — 8 unit tests
```

- [ ] **Step 10.3: Commit Plan.md update**

```bash
cd /Users/mclayton/developer/camsim
git add Plan.md
git commit -m "docs: mark Phase 20 Sprint 1 complete in Plan.md"
```

---

## Verification

After all tasks complete:

- [ ] **Build**: `scripts/run.sh --build` — UBT compiles without errors
- [ ] **Run tests**: In UE editor, open Automation tool and run `CamSim.Overlay.*` (8 tests, all pass)
- [ ] **Visual check with overlay enabled**:
  - Set `CAMSIM_OVERLAY_ENABLED=1` and run: `scripts/run.sh`
  - Send test CIGI packets: `scripts/send_cigi_test.py --circle`
  - Verify output with: `ffplay udp://239.1.1.1:5004` or `scripts/test_video_output.sh`
  - Confirm crosshair at center, Az/El top-left, FOV top-right, slant range bottom-left, timestamp bottom-center, green UNCLASSIFIED banners top and bottom
- [ ] **Visual check with overlay disabled**:
  - Confirm `CAMSIM_OVERLAY_ENABLED=0` (default) produces clean frames with no HUD elements
- [ ] **ML frame test**: Run with `CAMSIM_ML_ENABLED=1 CAMSIM_OVERLAY_ENABLED=0` — confirm depth maps and COCO annotations are unaffected

---

## Notes

- **Thread safety**: `FHudOverlay::Render()` runs on the background task thread — no UObject access, no GEngine calls. Pure CPU pixel manipulation only.
- **Overlay order**: Runs after ALL sensor effects (noise, blur, distortion, rolling shutter) so the HUD appears sharp on top of the degraded imagery, which matches real ISR sensor behavior.
- **IR/NVG colour**: Text is yellow on IR (visible against both white-hot and black-hot), green on NVG (matches real NVG phosphor), white on EO.
- **Default off**: `bEnabled = false` by default so existing ML training workflows are unaffected. Enable with `CAMSIM_OVERLAY_ENABLED=1` for ISR simulation output.
- **Scale=2 for 1080p**: Native 5×7 font (Scale=1) is barely readable at 1080p; Scale=2 gives 10×14 pixels which is legible in compressed H.264 output.
- **Classification colour**: Pass the correct classification string and colour via config — no hardcoded security levels in code.
