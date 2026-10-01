// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * ROADMAP 4A: when thermal IR can run. Pure logic (no renderer), so CamSim.Thermal.Availability.* tests it under NullRHI.
 */
namespace CamSimThermalAvailability
{
	/** ThermalCS reads the custom stencil (entity classes), which UE only writes with r.CustomDepth = 3 ("enabled with stencil"). */
	inline bool CustomDepthModeHasStencil(int32 RCustomDepth) { return RCustomDepth >= 3; }

	/**
	 * This tick's thermal availability (the argument of UCamSimCaptureComponent::ShouldRunThermal): the subsystem's startup
	 * decision, the live thermal.enabled, and not given up by the render thread (FThermalInputsMonitor).
	 */
	inline bool IsAvailableThisTick(bool bStartupAvailable, bool bEnabled, bool bInputsMissing)
	{
		return bStartupAvailable && bEnabled && !bInputsMissing;
	}
}

/**
 * Render thread (FCamSimFrameGrabExtension): decides that thermal radiance can't reach the sensor graph, so the game thread
 * stops committing frames to radiance (exposure, signal weights, AE slot, TSR alpha) and IR becomes the luminance proxy.
 * Sticky for the session. Call Observe once per sensor-graph frame.
 */
struct FThermalInputsMonitor
{
	enum class EGiveUp : uint8 { None, InputsMissing, NoRadiance };

	/** Consecutive thermal frames whose scene colour wasn't radiance before giving up (tolerates a one-off hiccup). */
	static constexpr int32 MaxFramesWithoutRadiance = 3;

	/**
	 * bThermalRequested: this frame carries thermal parameters. bInputsMissing: ThermalCS found no scene depth / custom
	 * depth / custom stencil / scene colour at BeforeDOF this frame. bRadianceDelivered: scene colour at the sensor graph
	 * is ThermalCS radiance. Returns the reason on the frame the monitor gives up, None otherwise (and on every later frame).
	 */
	EGiveUp Observe(bool bThermalRequested, bool bInputsMissing, bool bRadianceDelivered)
	{
		if (bGaveUp) return EGiveUp::None;
		if (!bThermalRequested || bRadianceDelivered) { FramesWithoutRadiance = 0; return EGiveUp::None; }
		if (bInputsMissing) { bGaveUp = true; return EGiveUp::InputsMissing; }
		if (++FramesWithoutRadiance >= MaxFramesWithoutRadiance) { bGaveUp = true; return EGiveUp::NoRadiance; }
		return EGiveUp::None;
	}

	bool HasGivenUp() const { return bGaveUp; }

private:
	int32 FramesWithoutRadiance = 0;
	bool  bGaveUp = false;
};
