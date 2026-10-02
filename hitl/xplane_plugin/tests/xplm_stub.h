/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * Fake X-Plane for the CamSimTruth test harness: implements the XPLM functions
 * the plugin calls (and nothing that writes a dataref, so a plugin that tried
 * would fail to link).
 */
#ifndef CAMSIM_TRUTH_XPLM_STUB_H
#define CAMSIM_TRUTH_XPLM_STUB_H

#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"

void stub_reset(void);

/* Datarefs */
void stub_set(const char *name, XPLMDataTypeID types, double value);
void stub_set_array(const char *name, XPLMDataTypeID types, int n, const double *values);
void stub_remove(const char *name);
int stub_find_calls(void); /* XPLMFindDataRef calls so far */

/* Plugin path reported by XPLMGetPluginInfo */
void stub_set_plugin_path(const char *path);

/* Flight loop */
int stub_flight_loop_exists(void);
int stub_flight_loop_phase(void);
float stub_flight_loop_interval(void); /* last XPLMScheduleFlightLoop interval */
float stub_run_flight_loop(void);       /* bumps the cycle number, calls the callback */
int stub_cycle(void);

/* Terrain probe: result and the local Y of the hit (= MSL altitude in the fake frame) */
void stub_set_probe(XPLMProbeResult result, double terrain_msl);
int stub_probe_exists(void);
int stub_probe_calls(void);
void stub_last_probe_point(double *lat, double *lon); /* probe start point mapped back to lat/lon */

/* Fake local frame (exactly invertible for the test) */
void stub_world_to_local(double lat, double lon, double alt, double *x, double *y, double *z);

/* Log.txt */
const char *stub_log(void);
void stub_clear_log(void);
int stub_log_count(const char *needle);
int stub_native_paths_enabled(void);

#endif
