// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"

/**
 * Which celestial sources are drawn (ROADMAP 6A): CIGI Celestial Sphere Control's Sun / Moon / Star Field Enable and
 * Star Field Intensity, folded with environment.celestial. Pure, header-only.
 */
struct FCelestialEnables
{
	bool  bSun = true, bMoon = true, bStars = true;
	float StarScale = 1.0f;   // CIGI Star Field Intensity (0..1); 1 = physical

	/** Before any packet: config defaults (stars_default decides stars). After: the host's flags; moon_enabled false
	 *  still removes the moon. A non-finite intensity is 1. With stars enabled the scale is the host's StarInt, so the
	 *  CIGI default intensity 0 draws invisible stars: that is the host's choice. */
	static FCelestialEnables From(const FCamSimConfig::FCelestialConfig& Cfg, const FCigiCelestialState* LastPacketOrNull)
	{
		FCelestialEnables E;
		if (!LastPacketOrNull)
		{
			E.bSun = true;
			E.bMoon = Cfg.bMoonEnabled;
			E.bStars = Cfg.bStarsDefault;
			E.StarScale = 1.0f;
			return E;
		}
		const FCigiCelestialState& P = *LastPacketOrNull;
		E.bSun = P.bSunEn;
		E.bMoon = P.bMoonEn && Cfg.bMoonEnabled;
		E.bStars = P.bStarEn;
		E.StarScale = FMath::IsFinite(P.StarInt) ? FMath::Clamp(P.StarInt, 0.0f, 1.0f) : 1.0f;
		return E;
	}
};
