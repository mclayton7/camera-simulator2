// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declaration — module loaded in Open(), cached pointer held here
class IImageWrapperModule;

/**
 * FDepthMapWriter
 *
 * Writes one 16-bit grayscale PNG depth map per frame.
 * Input: float buffer (metres): InstanceIdCS's linear view depth of the primary view (WRITE_DEPTH).
 * Output: <OutputDir>/depth/depth_NNNNNNNN.png
 *
 * Depth is linearly quantized: 0 m → 0,  DepthFarPlaneM → 65535.
 * Values beyond DepthFarPlaneM are clamped to 65535.
 *
 * Not an IAnnotationWriter because input type differs (float vs. entity data).
 */
class FDepthMapWriter
{
public:
	bool Open(const FString& OutputDir, float InDepthFarPlaneM = 5000.0f);

	/** Write one depth frame. Task thread only. */
	void WriteDepthFrame(const TArray<float>& DepthMetres,
	                     int32 Width, int32 Height, uint64 FrameIdx);

	/**
	 * The 16-bit PNG this writer stores for a frame (0 m → 0, FarPlaneM → 65535, beyond clamped). Empty on a size
	 * mismatch or failure. Any thread once ImageWrapper is loaded (Open() or the control API load it).
	 */
	static TArray64<uint8> EncodeDepthPng(const TArray<float>& DepthMetres, int32 Width, int32 Height, float FarPlaneM);

	float GetDepthFarPlaneM() const { return DepthFarPlaneM; }

	void  Close();
	bool  IsOpen() const { return bIsOpen; }

private:
	bool    bIsOpen        = false;
	FString DepthDir;
	float   DepthFarPlaneM = 5000.0f;

	// Cached module pointer — loaded in Open() on game thread
	IImageWrapperModule* ImageWrapperModule = nullptr;
};
