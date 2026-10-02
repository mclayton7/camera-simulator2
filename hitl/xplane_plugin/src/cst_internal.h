/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * Hooks for the test harness only. Not exported from the .xpl (hidden visibility).
 */
#ifndef CAMSIM_TRUTH_INTERNAL_H
#define CAMSIM_TRUTH_INTERNAL_H

#include <stdint.h>

#define CST_PLUGIN_VERSION "1.0.0"

/* Replaces the monotonic clock used for mono_ns and the weather/terrain
 * schedules; NULL restores cst_mono_ns. */
void cst_test_set_clock(uint64_t (*clock_fn)(void));

#endif
