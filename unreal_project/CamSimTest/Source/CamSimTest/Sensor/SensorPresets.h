// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FSensorModeConfig;

// ---------------------------------------------------------------------------
// CamSimSensorPresets — sensor-class presets (ROADMAP 3B.2 Task 5).
//
// A preset supplies typical datasheet Optics/Detector values for a sensor
// class; yaml optics:/detector: blocks then override individual fields on
// top (see FCamSimConfig::LoadFromYaml). Values match
// docs/superpowers/specs/2026-09-27-physical-sensor-model-design.md
// "Presets and configuration".
// ---------------------------------------------------------------------------
namespace CamSimSensorPresets
{
	/**
	 * Apply the named preset's Optics/Detector values onto InOut. Known names:
	 * "eo_hd_cmos" (1080p industrial CMOS), "mwir_cooled" (640x512 InSb),
	 * "lwir_uncooled" (640x512 VOx).
	 *
	 * Returns false for an unknown name, leaving InOut untouched — the caller
	 * keeps whichever defaults were already applied and reports the bad name
	 * via FCamSimConfig::Validate().
	 */
	bool Apply(const FString& Name, FSensorModeConfig& InOut);
}
