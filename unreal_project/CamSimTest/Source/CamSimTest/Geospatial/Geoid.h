// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace CamSim::Geospatial
{
	/** Project-relative location of the EGM96 grid (see scripts/make_egm96_dac.py). */
	FString GetGeoidGridPath();

	/**
	 * EGM96 geoid height (undulation) above the WGS-84 ellipsoid in metres, so
	 * MSL altitude = ellipsoid height - undulation. Bilinear over NGA's
	 * 15-arcminute grid. The grid is loaded on first use;
	 * returns unset if it is missing or unreadable (logged once). Thread-safe.
	 */
	TOptional<double> GetGeoidUndulation(double LatDeg, double LonDeg);
}
