/* Copyright CamSim Contributors. All Rights Reserved. */
#include "xplm_stub.h"

#include "XPLMDefs.h"
#include "XPLMGraphics.h"
#include "XPLMPlugin.h"
#include "XPLMUtilities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DREFS 160
#define MAX_ELEMS 8

typedef struct stub_dref {
    char name[128];
    XPLMDataTypeID types;
    double value;
    double arr[MAX_ELEMS];
    int n;
    int present;
} stub_dref;

static stub_dref g_drefs[MAX_DREFS];
static int g_find_calls;
static char g_plugin_path[1024];
static char g_log[65536];
static int g_native_paths;

static XPLMCreateFlightLoop_t g_fl;
static int g_fl_exists;
static float g_fl_interval;
static int g_cycle;
static float g_last_call_ret;

static int g_probe_exists, g_probe_calls;
static XPLMProbeResult g_probe_result = xplm_ProbeHitTerrain;
static double g_terrain_msl;
static float g_probe_x, g_probe_z;

void stub_reset(void)
{
    memset(g_drefs, 0, sizeof g_drefs);
    g_find_calls = 0;
    g_plugin_path[0] = '\0';
    g_log[0] = '\0';
    g_fl_exists = 0;
    g_fl_interval = 0;
    g_probe_exists = g_probe_calls = 0;
    g_probe_result = xplm_ProbeHitTerrain;
    g_terrain_msl = 0;
    (void)g_last_call_ret;
}

static stub_dref *lookup(const char *name, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_DREFS; ++i) {
        if (g_drefs[i].name[0] && strcmp(g_drefs[i].name, name) == 0)
            return &g_drefs[i];
        if (!g_drefs[i].name[0] && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;
    snprintf(g_drefs[free_slot].name, sizeof g_drefs[free_slot].name, "%s", name);
    return &g_drefs[free_slot];
}

void stub_set(const char *name, XPLMDataTypeID types, double value)
{
    stub_dref *d = lookup(name, 1);
    d->types = types;
    d->value = value;
    d->n = 0;
    d->present = 1;
}

void stub_set_array(const char *name, XPLMDataTypeID types, int n, const double *values)
{
    stub_dref *d = lookup(name, 1);
    d->types = types;
    d->n = n > MAX_ELEMS ? MAX_ELEMS : n;
    memcpy(d->arr, values, sizeof(double) * (size_t)d->n);
    d->present = 1;
}

void stub_remove(const char *name)
{
    stub_dref *d = lookup(name, 0);
    if (d)
        d->present = 0; /* keep the slot so a stale handle still points at valid memory */
}

int stub_find_calls(void) { return g_find_calls; }
void stub_set_plugin_path(const char *path) { snprintf(g_plugin_path, sizeof g_plugin_path, "%s", path); }

int stub_flight_loop_exists(void) { return g_fl_exists; }
int stub_flight_loop_phase(void) { return g_fl.phase; }
float stub_flight_loop_interval(void) { return g_fl_interval; }
int stub_cycle(void) { return g_cycle; }

float stub_run_flight_loop(void)
{
    if (!g_fl_exists || !g_fl.callbackFunc) {
        fprintf(stderr, "stub: no flight loop registered\n");
        abort();
    }
    ++g_cycle;
    return g_fl.callbackFunc(1.0f / 32.0f, 1.0f / 32.0f, g_cycle, g_fl.refcon);
}

void stub_set_probe(XPLMProbeResult result, double terrain_msl)
{
    g_probe_result = result;
    g_terrain_msl = terrain_msl;
}
int stub_probe_exists(void) { return g_probe_exists; }
int stub_probe_calls(void) { return g_probe_calls; }

/* Fake local frame: x = east-ish, y = altitude, z = south-ish. Scale chosen so
 * floats keep about 1e-5 deg near the test position. */
void stub_world_to_local(double lat, double lon, double alt, double *x, double *y, double *z)
{
    *x = (lon + 117.0) * 1000.0;
    *y = alt;
    *z = -(lat - 34.0) * 1000.0;
}

void stub_last_probe_point(double *lat, double *lon)
{
    *lon = g_probe_x / 1000.0 - 117.0;
    *lat = -g_probe_z / 1000.0 + 34.0;
}

const char *stub_log(void) { return g_log; }
void stub_clear_log(void) { g_log[0] = '\0'; }
int stub_log_count(const char *needle)
{
    int n = 0;
    const char *p = g_log;
    while ((p = strstr(p, needle)) != NULL) {
        ++n;
        p += strlen(needle);
    }
    return n;
}
int stub_native_paths_enabled(void) { return g_native_paths; }

/* ---------------------------------------------------------------------- */
/* XPLM API                                                                */

XPLM_API void XPLMDebugString(const char *inString)
{
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "%s", inString);
    fputs(inString, stderr);
}

XPLM_API XPLMDataRef XPLMFindDataRef(const char *inDataRefName)
{
    stub_dref *d;
    ++g_find_calls;
    d = lookup(inDataRefName, 0);
    return d && d->present ? (XPLMDataRef)d : NULL;
}

XPLM_API XPLMDataTypeID XPLMGetDataRefTypes(XPLMDataRef inDataRef)
{
    stub_dref *d = (stub_dref *)inDataRef;
    return d && d->present ? d->types : xplmType_Unknown;
}

XPLM_API int XPLMGetDatai(XPLMDataRef inDataRef)
{
    stub_dref *d = (stub_dref *)inDataRef;
    return d && d->present && (d->types & xplmType_Int) ? (int)d->value : 0;
}

XPLM_API float XPLMGetDataf(XPLMDataRef inDataRef)
{
    stub_dref *d = (stub_dref *)inDataRef;
    return d && d->present && (d->types & xplmType_Float) ? (float)d->value : 0.0f;
}

XPLM_API double XPLMGetDatad(XPLMDataRef inDataRef)
{
    stub_dref *d = (stub_dref *)inDataRef;
    return d && d->present && (d->types & xplmType_Double) ? d->value : 0.0;
}

XPLM_API int XPLMGetDatavf(XPLMDataRef inDataRef, float *outValues, int inOffset, int inMax)
{
    stub_dref *d = (stub_dref *)inDataRef;
    int i, n = 0;
    if (!d || !d->present || !(d->types & xplmType_FloatArray))
        return 0;
    if (!outValues)
        return d->n;
    for (i = inOffset; i < d->n && n < inMax; ++i)
        outValues[n++] = (float)d->arr[i];
    return n;
}

XPLM_API int XPLMGetDatavi(XPLMDataRef inDataRef, int *outValues, int inOffset, int inMax)
{
    stub_dref *d = (stub_dref *)inDataRef;
    int i, n = 0;
    if (!d || !d->present || !(d->types & xplmType_IntArray))
        return 0;
    if (!outValues)
        return d->n;
    for (i = inOffset; i < d->n && n < inMax; ++i)
        outValues[n++] = (int)d->arr[i];
    return n;
}

XPLM_API int XPLMGetCycleNumber(void) { return g_cycle; }

XPLM_API XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t *inParams)
{
    if (inParams->structSize != (int)sizeof(XPLMCreateFlightLoop_t)) {
        fprintf(stderr, "stub: bad XPLMCreateFlightLoop_t structSize\n");
        abort();
    }
    g_fl = *inParams;
    g_fl_exists = 1;
    return (XPLMFlightLoopID)&g_fl;
}

XPLM_API void XPLMScheduleFlightLoop(XPLMFlightLoopID inFlightLoopID, float inInterval, int inRelativeToNow)
{
    (void)inFlightLoopID; (void)inRelativeToNow;
    g_fl_interval = inInterval;
}

XPLM_API void XPLMDestroyFlightLoop(XPLMFlightLoopID inFlightLoopID)
{
    (void)inFlightLoopID;
    memset(&g_fl, 0, sizeof g_fl);
    g_fl_exists = 0;
}

static int g_probe_token;
XPLM_API XPLMProbeRef XPLMCreateProbe(XPLMProbeType inProbeType)
{
    if (inProbeType != xplm_ProbeY)
        abort();
    g_probe_exists = 1;
    return (XPLMProbeRef)&g_probe_token;
}

XPLM_API void XPLMDestroyProbe(XPLMProbeRef inProbe)
{
    (void)inProbe;
    g_probe_exists = 0;
}

XPLM_API XPLMProbeResult XPLMProbeTerrainXYZ(XPLMProbeRef inProbe, float inX, float inY, float inZ, XPLMProbeInfo_t *outInfo)
{
    (void)inY;
    if (inProbe != (XPLMProbeRef)&g_probe_token || outInfo->structSize != (int)sizeof(XPLMProbeInfo_t)) {
        fprintf(stderr, "stub: bad probe call\n");
        abort();
    }
    ++g_probe_calls;
    g_probe_x = inX;
    g_probe_z = inZ;
    if (g_probe_result != xplm_ProbeHitTerrain)
        return g_probe_result;
    outInfo->locationX = inX;
    outInfo->locationY = (float)g_terrain_msl;
    outInfo->locationZ = inZ;
    outInfo->normalY = 1.0f;
    return xplm_ProbeHitTerrain;
}

XPLM_API void XPLMWorldToLocal(double inLatitude, double inLongitude, double inAltitude, double *outX, double *outY, double *outZ)
{
    stub_world_to_local(inLatitude, inLongitude, inAltitude, outX, outY, outZ);
}

XPLM_API void XPLMLocalToWorld(double inX, double inY, double inZ, double *outLatitude, double *outLongitude, double *outAltitude)
{
    *outLongitude = inX / 1000.0 - 117.0;
    *outAltitude = inY;
    *outLatitude = -inZ / 1000.0 + 34.0;
}

XPLM_API XPLMPluginID XPLMGetMyID(void) { return 7; }

XPLM_API void XPLMGetPluginInfo(XPLMPluginID inPlugin, char *outName, char *outFilePath, char *outSignature, char *outDescription)
{
    (void)inPlugin;
    if (outName) strcpy(outName, "CamSimTruth");
    if (outFilePath) strcpy(outFilePath, g_plugin_path);
    if (outSignature) strcpy(outSignature, "camsim.hitl.truth");
    if (outDescription) strcpy(outDescription, "");
}

XPLM_API void XPLMEnableFeature(const char *inFeature, int inEnable)
{
    if (strcmp(inFeature, "XPLM_USE_NATIVE_PATHS") == 0)
        g_native_paths = inEnable;
}
