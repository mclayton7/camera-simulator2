// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "PixelFormat.h"

/**
 * CamSimConvertReadbackPixels
 *
 * Converts a raw GPU readback buffer into a packed TArray<FColor> (BGRA in
 * memory, matching FColor's B/G/R/A layout) ready for sws_scale(AV_PIX_FMT_BGRA).
 *
 * @param RawData        Pointer to the locked GPU readback buffer.
 * @param RowPitch       Row pitch value returned by FRHIGPUTextureReadback::Lock().
 *                       The function handles both "pitch in bytes" and "pitch in pixels"
 *                       conventions that different RHIs use.
 * @param W / H          Render target dimensions in pixels.
 * @param PixelFormat    UE pixel format of the render target (PF_B8G8R8A8 / PF_R8G8B8A8).
 * @param DesiredFormat  Explicit readback format from config, or Auto for auto-detection.
 * @param bForceSwap     When true, treat BGRA readback bytes as RGBA (swaps R and B).
 *                       Only meaningful when DesiredFormat is Auto or BGRA; has no
 *                       effect when DesiredFormat is already RGBA/ARGB/ABGR.
 * @param OutPixels      Output array, sized W*H on entry (caller pre-allocates).
 * @param FrameIdx       Frame index for debug logging (first 3 frames only).
 */
inline void CamSimConvertReadbackPixels(
	const void*                          RawData,
	int32                                RowPitch,
	int32                                W,
	int32                                H,
	EPixelFormat                         PixelFormat,
	FCamSimConfig::EReadbackFormat       DesiredFormat,
	bool                                 bForceSwap,
	TArray<FColor>&                      OutPixels,
	uint64                               FrameIdx)
{
	const bool bIsRgba = (PixelFormat == PF_R8G8B8A8);
	const bool bIsBgra = (PixelFormat == PF_B8G8R8A8);

	// Resolve effective format: explicit config → auto-detected → unchanged Auto
	FCamSimConfig::EReadbackFormat EffectiveFormat = DesiredFormat;
	if (EffectiveFormat == FCamSimConfig::EReadbackFormat::Auto)
	{
		// Both Metal and Vulkan deliver BGRA bytes for PF_B8G8R8A8:
		//   Metal: MTLPixelFormatBGRA8Unorm_sRGB — native BGRA
		//   Vulkan: VK_FORMAT_B8G8R8A8_SRGB — native BGRA
		// vkCmdCopyImageToBuffer / Metal readback both copy raw memory bytes.
		if      (bIsBgra) EffectiveFormat = FCamSimConfig::EReadbackFormat::BGRA;
		else if (bIsRgba) EffectiveFormat = FCamSimConfig::EReadbackFormat::RGBA;
		// else: stays Auto — memcpy fallback below
	}

	// bForceSwap: flip BGRA→RGBA interpretation (corrects R/B swap on some RHIs)
	if (bForceSwap && EffectiveFormat == FCamSimConfig::EReadbackFormat::BGRA)
	{
		EffectiveFormat = FCamSimConfig::EReadbackFormat::RGBA;
	}

	constexpr int32 BytesPerPixel = 4;
	// RHI row pitch may be reported in bytes or in pixels; normalise to bytes
	const int32 RowPitchBytes =
		(RowPitch >= W * BytesPerPixel) ? RowPitch : RowPitch * BytesPerPixel;

	const uint8* Src = static_cast<const uint8*>(RawData);
	for (int32 Y = 0; Y < H; ++Y)
	{
		const uint8* RowSrc = Src + static_cast<SIZE_T>(Y) * RowPitchBytes;
		FColor* RowDst = &OutPixels[Y * W];
		switch (EffectiveFormat)
		{
			// GPU gives BGRA bytes → FColor memory is also BGRA → direct copy
			case FCamSimConfig::EReadbackFormat::BGRA:
				FMemory::Memcpy(RowDst, RowSrc, W * BytesPerPixel);
				break;

			// GPU gives RGBA bytes → assign to FColor fields (stored as BGRA)
			case FCamSimConfig::EReadbackFormat::RGBA:
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = X * BytesPerPixel;
					FColor& P = RowDst[X];
					P.R = RowSrc[I + 0];
					P.G = RowSrc[I + 1];
					P.B = RowSrc[I + 2];
					P.A = RowSrc[I + 3];
				}
				break;

			case FCamSimConfig::EReadbackFormat::ARGB:
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = X * BytesPerPixel;
					FColor& P = RowDst[X];
					P.A = RowSrc[I + 0];
					P.R = RowSrc[I + 1];
					P.G = RowSrc[I + 2];
					P.B = RowSrc[I + 3];
				}
				break;

			case FCamSimConfig::EReadbackFormat::ABGR:
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = X * BytesPerPixel;
					FColor& P = RowDst[X];
					P.A = RowSrc[I + 0];
					P.B = RowSrc[I + 1];
					P.G = RowSrc[I + 2];
					P.R = RowSrc[I + 3];
				}
				break;

			case FCamSimConfig::EReadbackFormat::Auto:
			default:
				FMemory::Memcpy(RowDst, RowSrc, W * BytesPerPixel);
				break;
		}
	}

	if (FrameIdx < 3)
	{
		const TCHAR* FmtStr = TEXT("auto");
		switch (EffectiveFormat)
		{
			case FCamSimConfig::EReadbackFormat::BGRA: FmtStr = TEXT("bgra"); break;
			case FCamSimConfig::EReadbackFormat::RGBA: FmtStr = TEXT("rgba"); break;
			case FCamSimConfig::EReadbackFormat::ARGB: FmtStr = TEXT("argb"); break;
			case FCamSimConfig::EReadbackFormat::ABGR: FmtStr = TEXT("abgr"); break;
			default: break;
		}

		// Log raw readback bytes BEFORE conversion — critical for diagnosing
		// BGRA vs RGBA byte order differences between Metal and Vulkan.
		const uint8* RawBytes = static_cast<const uint8*>(RawData);
		UE_LOG(LogCamSim, Log,
			TEXT("CamSimReadback frame %llu: raw bytes[0..7]=[%u %u %u %u | %u %u %u %u] "
			     "rowpitch_raw=%d rowpitch_bytes=%d format=%s readback=%s force_swap=%d"),
			FrameIdx,
			RawBytes[0], RawBytes[1], RawBytes[2], RawBytes[3],
			RawBytes[4], RawBytes[5], RawBytes[6], RawBytes[7],
			RowPitch, RowPitchBytes,
			GetPixelFormatString(PixelFormat), FmtStr, bForceSwap ? 1 : 0);

		// Also log the converted FColor fields for the first pixel
		if (OutPixels.Num() > 0)
		{
			const FColor& P0 = OutPixels[0];
			UE_LOG(LogCamSim, Log,
				TEXT("CamSimReadback frame %llu: pixel[0] FColor(B=%u G=%u R=%u A=%u) "
				     "raw_byte0123=(%u,%u,%u,%u)"),
				FrameIdx, P0.B, P0.G, P0.R, P0.A,
				RawBytes[0], RawBytes[1], RawBytes[2], RawBytes[3]);
		}
	}
}
