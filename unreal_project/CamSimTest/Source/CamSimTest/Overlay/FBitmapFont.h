// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/**
 * Minimal 5x7 bitmap font for HUD overlays.
 * Characters 0x20 (space) through 0x7E (~), 95 glyphs.
 * Encoding: each glyph = 7 bytes (one per row), each byte has 5 bits
 *   bit 4 (0x10) = column 0 (leftmost)
 *   bit 0 (0x01) = column 4 (rightmost)
 *   row 0 = top row
 *
 * All methods operate directly on TArray<FColor> pixel buffers in BGRA8 layout.
 */
struct FBitmapFont
{
    static constexpr int32 GlyphW = 5;
    static constexpr int32 GlyphH = 7;
    static constexpr int32 CharSpacing = 1;
    static constexpr int32 CharStride = GlyphW + CharSpacing; // 6

    static int32 DrawChar(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                          int32 X, int32 Y, char Ch,
                          FColor Fg, int32 Scale = 1);

    static int32 DrawString(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                            int32 X, int32 Y, const ANSICHAR* Str,
                            FColor Fg, int32 Scale = 1);

    static int32 DrawStringWithShadow(TArray<FColor>& Pixels, int32 BufW, int32 BufH,
                                      int32 X, int32 Y, const ANSICHAR* Str,
                                      FColor Fg, int32 Scale = 1);

    static int32 StringWidth(const ANSICHAR* Str, int32 Scale = 1);

    static void SetPixel(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, FColor C);
    static void DrawHLine(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 Len, FColor C);
    static void DrawVLine(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 Len, FColor C);
    static void DrawRect(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 W, int32 H, FColor C);
    static void FillRect(TArray<FColor>& Pixels, int32 BufW, int32 BufH, int32 X, int32 Y, int32 W, int32 H, FColor C);

    static const uint8 GFont5x7[95][7];
};
