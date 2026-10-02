/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * CamSimTruth: read-only X-Plane 11 plugin that sends the aircraft's truth
 * state to the Hooter HITL IG host, one UDP datagram per flight loop
 * (hitl/PROTOCOL.md section 1, design in HITL.md "Getting truth out of X-Plane 11").
 *
 * - Never writes a dataref and never touches X-Plane's network/data-output settings.
 * - Every XPLM call happens on X-Plane's main thread (plugin callbacks and the
 *   flight loop); there are no other threads.
 * - Dataref handles are looked up once per enable; a missing dataref reads as 0
 *   and is logged once.
 */
#include "XPLMDataAccess.h"
#include "XPLMDefs.h"
#include "XPLMGraphics.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"

#include "cst_config.h"
#include "cst_internal.h"
#include "cst_platform.h"
#include "truth_packet.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */

static void cst_log(const char *msg)
{
    char buf[2304];
    snprintf(buf, sizeof buf, "CamSimTruth: %s\n", msg);
    XPLMDebugString(buf);
}

/* ------------------------------------------------------------------------ */
/* Datarefs                                                                  */

typedef enum { RD_NONE = 0, RD_DOUBLE, RD_FLOAT, RD_INT, RD_FLOAT_ARRAY, RD_INT_ARRAY } cst_read_mode;

typedef struct cst_dref {
    const char *name;
    int index; /* -1: scalar; >= 0: layer index (XP11 "name[i]" scalar, else element i of array "name") */
    XPLMDataRef ref;
    cst_read_mode mode;
    int elem; /* element for array reads */
} cst_dref;

enum {
    D_LAT, D_LON, D_ELEV,
    D_TRUE_PSI, D_TRUE_THETA, D_TRUE_PHI,
    D_P, D_Q, D_R,
    D_VX, D_VY, D_VZ,
    D_TAS, D_GS, D_IAS, D_MAG_PSI, D_Y_AGL,
    D_PAUSED, D_REPLAY, D_SIM_SPEED, D_SIM_SPEED_OGL,
    D_ZULU, D_LOCAL, D_MONTH, D_DAY, D_USE_SYS_TIME,
    D_EARTH_RADIUS,
    /* weather block */
    D_VIS, D_TEMP, D_HUMIDITY, D_WIND_SPEED, D_WIND_DIR, D_BARO,
    D_CLOUD_BASE0, D_CLOUD_BASE1, D_CLOUD_BASE2,
    D_CLOUD_TOPS0, D_CLOUD_TOPS1, D_CLOUD_TOPS2,
    D_CLOUD_COV0, D_CLOUD_COV1, D_CLOUD_COV2,
    D_CLOUD_TYPE0, D_CLOUD_TYPE1, D_CLOUD_TYPE2,
    D_WAVE_AMP, D_WAVE_LEN, D_WAVE_SPEED, D_WAVE_DIR,
    D_COUNT
};

#define POS "sim/flightmodel/position/"
static cst_dref g_drefs[D_COUNT] = {
    [D_LAT] = {POS "latitude", -1},
    [D_LON] = {POS "longitude", -1},
    [D_ELEV] = {POS "elevation", -1},
    [D_TRUE_PSI] = {POS "true_psi", -1},
    [D_TRUE_THETA] = {POS "true_theta", -1},
    [D_TRUE_PHI] = {POS "true_phi", -1},
    [D_P] = {POS "Prad", -1},
    [D_Q] = {POS "Qrad", -1},
    [D_R] = {POS "Rrad", -1},
    [D_VX] = {POS "local_vx", -1},
    [D_VY] = {POS "local_vy", -1},
    [D_VZ] = {POS "local_vz", -1},
    [D_TAS] = {POS "true_airspeed", -1},
    [D_GS] = {POS "groundspeed", -1},
    [D_IAS] = {POS "indicated_airspeed", -1},
    [D_MAG_PSI] = {POS "mag_psi", -1},
    [D_Y_AGL] = {POS "y_agl", -1},
    [D_PAUSED] = {"sim/time/paused", -1},
    [D_REPLAY] = {"sim/time/is_in_replay", -1},
    [D_SIM_SPEED] = {"sim/time/sim_speed", -1},
    [D_SIM_SPEED_OGL] = {"sim/time/sim_speed_actual_ogl", -1},
    [D_ZULU] = {"sim/time/zulu_time_sec", -1},
    [D_LOCAL] = {"sim/time/local_time_sec", -1},
    [D_MONTH] = {"sim/cockpit2/clock_timer/current_month", -1},
    [D_DAY] = {"sim/cockpit2/clock_timer/current_day", -1},
    [D_USE_SYS_TIME] = {"sim/time/use_system_time", -1},
    [D_EARTH_RADIUS] = {"sim/physics/earth_radius_m", -1},
    [D_VIS] = {"sim/weather/visibility_reported_m", -1},
    [D_TEMP] = {"sim/weather/temperature_ambient_c", -1},
    [D_HUMIDITY] = {"sim/weather/relative_humidity_sealevel_percent", -1},
    [D_WIND_SPEED] = {"sim/weather/wind_speed_kt", -1},
    [D_WIND_DIR] = {"sim/weather/wind_direction_degt", -1},
    [D_BARO] = {"sim/weather/barometer_sealevel_inhg", -1},
    [D_CLOUD_BASE0] = {"sim/weather/cloud_base_msl_m", 0},
    [D_CLOUD_BASE1] = {"sim/weather/cloud_base_msl_m", 1},
    [D_CLOUD_BASE2] = {"sim/weather/cloud_base_msl_m", 2},
    [D_CLOUD_TOPS0] = {"sim/weather/cloud_tops_msl_m", 0},
    [D_CLOUD_TOPS1] = {"sim/weather/cloud_tops_msl_m", 1},
    [D_CLOUD_TOPS2] = {"sim/weather/cloud_tops_msl_m", 2},
    [D_CLOUD_COV0] = {"sim/weather/cloud_coverage", 0},
    [D_CLOUD_COV1] = {"sim/weather/cloud_coverage", 1},
    [D_CLOUD_COV2] = {"sim/weather/cloud_coverage", 2},
    [D_CLOUD_TYPE0] = {"sim/weather/cloud_type", 0},
    [D_CLOUD_TYPE1] = {"sim/weather/cloud_type", 1},
    [D_CLOUD_TYPE2] = {"sim/weather/cloud_type", 2},
    [D_WAVE_AMP] = {"sim/weather/wave_amplitude", -1},
    [D_WAVE_LEN] = {"sim/weather/wave_length", -1},
    [D_WAVE_SPEED] = {"sim/weather/wave_speed", -1},
    [D_WAVE_DIR] = {"sim/weather/wave_dir", -1},
};
#undef POS

/* Picks how to read a dataref from its type bits: double > float > int for a
 * scalar, float[] > int[] for an array. */
static cst_read_mode mode_for(XPLMDataTypeID t, int want_array)
{
    if (!want_array) {
        if (t & xplmType_Double) return RD_DOUBLE;
        if (t & xplmType_Float) return RD_FLOAT;
        if (t & xplmType_Int) return RD_INT;
    }
    if (t & xplmType_FloatArray) return RD_FLOAT_ARRAY;
    if (t & xplmType_IntArray) return RD_INT_ARRAY;
    return RD_NONE;
}

static void bind_dref(cst_dref *d)
{
    d->ref = NULL;
    d->mode = RD_NONE;
    d->elem = 0;
    if (d->index >= 0) {
        /* X-Plane 11 publishes weather layers as separate scalars named "name[i]";
         * X-Plane 12 also has real arrays named "name". */
        char layered[160];
        snprintf(layered, sizeof layered, "%s[%d]", d->name, d->index);
        d->ref = XPLMFindDataRef(layered);
        if (d->ref) {
            d->mode = mode_for(XPLMGetDataRefTypes(d->ref), 0);
            if (d->mode == RD_FLOAT_ARRAY || d->mode == RD_INT_ARRAY)
                d->elem = 0;
        } else {
            d->ref = XPLMFindDataRef(d->name);
            if (d->ref) {
                d->mode = mode_for(XPLMGetDataRefTypes(d->ref), 1);
                d->elem = d->index;
            }
        }
    } else {
        d->ref = XPLMFindDataRef(d->name);
        if (d->ref) {
            d->mode = mode_for(XPLMGetDataRefTypes(d->ref), 0);
            d->elem = 0; /* scalar published only as an array: element 0 */
        }
    }
    if (d->mode == RD_NONE)
        d->ref = NULL;
}

static double read_dref(const cst_dref *d)
{
    switch (d->mode) {
    case RD_DOUBLE: return XPLMGetDatad(d->ref);
    case RD_FLOAT: return (double)XPLMGetDataf(d->ref);
    case RD_INT: return (double)XPLMGetDatai(d->ref);
    case RD_FLOAT_ARRAY: {
        float v = 0.0f;
        return XPLMGetDatavf(d->ref, &v, d->elem, 1) == 1 ? (double)v : 0.0;
    }
    case RD_INT_ARRAY: {
        int v = 0;
        return XPLMGetDatavi(d->ref, &v, d->elem, 1) == 1 ? (double)v : 0.0;
    }
    default: return 0.0;
    }
}

static float rf(int i) { return (float)read_dref(&g_drefs[i]); }
static int has(int i) { return g_drefs[i].mode != RD_NONE; }

static uint8_t to_u8(double v)
{
    if (!(v > 0.0)) return 0; /* also NaN */
    if (v > 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

/* ------------------------------------------------------------------------ */
/* State                                                                     */

static uint64_t (*g_clock)(void) = cst_mono_ns;
static int g_net_ok;
static cst_config g_cfg;
static cst_sender g_sender = {-1, {0}, 0};
static XPLMFlightLoopID g_loop;
static XPLMProbeRef g_probe;
static uint32_t g_seq;
static int g_weather_due_set, g_terrain_due_set;
static uint64_t g_weather_due, g_terrain_due;
static unsigned long g_send_errors;
static int g_last_send_error;
static char g_plugin_dir[1024];

void cst_test_set_clock(uint64_t (*clock_fn)(void)) { g_clock = clock_fn ? clock_fn : cst_mono_ns; }

/* Fixed-rate schedule on the monotonic clock. period_ns 0 = every call.
 * Keeps the cadence (due += period) unless it fell more than one period
 * behind (pause, stall), then restarts from now. */
static int schedule_due(uint64_t now, uint64_t period_ns, int *due_set, uint64_t *due)
{
    if (period_ns == 0)
        return 1;
    if (!*due_set || now >= *due) {
        if (!*due_set || now - *due >= period_ns)
            *due = now + period_ns;
        else
            *due += period_ns;
        *due_set = 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Datagram                                                                  */

static void fill_weather(cst_truth_v1 *t)
{
    int i;
    t->visibility_m = rf(D_VIS);
    t->temperature_c = rf(D_TEMP);
    t->humidity_pct = rf(D_HUMIDITY);
    t->wind_speed = rf(D_WIND_SPEED);
    t->wind_dir_degt = rf(D_WIND_DIR);
    t->baro_inhg = rf(D_BARO);
    for (i = 0; i < 3; ++i) {
        t->cloud_base_msl_m[i] = rf(D_CLOUD_BASE0 + i);
        t->cloud_tops_msl_m[i] = rf(D_CLOUD_TOPS0 + i);
        t->cloud_coverage[i] = rf(D_CLOUD_COV0 + i);
        t->cloud_type[i] = rf(D_CLOUD_TYPE0 + i);
    }
    t->wave_amplitude = rf(D_WAVE_AMP);
    t->wave_length = rf(D_WAVE_LEN);
    t->wave_speed = rf(D_WAVE_SPEED);
    t->wave_dir = rf(D_WAVE_DIR);
    t->flags |= CST_FLAG_WEATHER_VALID;
}

/* Probes X-Plane's terrain straight below the aircraft. The probe runs in
 * local OpenGL coordinates from the aircraft's position (XPLMWorldToLocal of
 * the double lat/lon/elevation), and the hit point goes back through
 * XPLMLocalToWorld, whose altitude is metres MSL. */
static void fill_terrain(cst_truth_v1 *t)
{
    double x, y, z, lat, lon, alt;
    XPLMProbeInfo_t info;

    if (!g_probe || !has(D_LAT) || !has(D_LON) || !has(D_ELEV))
        return;
    XPLMWorldToLocal(t->latitude, t->longitude, t->elevation, &x, &y, &z);
    memset(&info, 0, sizeof info);
    info.structSize = (int)sizeof info;
    if (XPLMProbeTerrainXYZ(g_probe, (float)x, (float)y, (float)z, &info) != xplm_ProbeHitTerrain)
        return;
    XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &lat, &lon, &alt);
    t->terrain_msl = alt;
    t->flags |= CST_FLAG_TERRAIN_VALID;
}

static void build_datagram(cst_truth_v1 *t, uint64_t now)
{
    uint64_t weather_period = (uint64_t)(g_cfg.weather_period_s * 1e9 + 0.5);
    uint64_t terrain_period = g_cfg.terrain_probe_hz > 0.0 ? (uint64_t)(1e9 / g_cfg.terrain_probe_hz + 0.5) : 0;

    memset(t, 0, sizeof *t);
    memcpy(t->magic, CST_MAGIC, 4);
    t->version = (uint16_t)CST_VERSION;
    t->cycle = (uint32_t)XPLMGetCycleNumber();
    t->seq = g_seq++;
    t->mono_ns = now;

    t->latitude = read_dref(&g_drefs[D_LAT]);
    t->longitude = read_dref(&g_drefs[D_LON]);
    t->elevation = read_dref(&g_drefs[D_ELEV]);

    t->true_psi = rf(D_TRUE_PSI);
    t->true_theta = rf(D_TRUE_THETA);
    t->true_phi = rf(D_TRUE_PHI);
    t->p = rf(D_P);
    t->q = rf(D_Q);
    t->r = rf(D_R);
    t->local_vx = rf(D_VX);
    t->local_vy = rf(D_VY);
    t->local_vz = rf(D_VZ);
    t->true_airspeed = rf(D_TAS);
    t->groundspeed = rf(D_GS);
    t->indicated_airspeed = rf(D_IAS);
    t->mag_psi = rf(D_MAG_PSI);
    t->y_agl = rf(D_Y_AGL);
    t->sim_speed = rf(D_SIM_SPEED);
    t->sim_speed_actual_ogl = rf(D_SIM_SPEED_OGL);
    t->zulu_time_sec = rf(D_ZULU);
    t->local_time_sec = rf(D_LOCAL);
    t->local_month = to_u8(read_dref(&g_drefs[D_MONTH]));
    t->local_day = to_u8(read_dref(&g_drefs[D_DAY]));
    t->use_system_time = read_dref(&g_drefs[D_USE_SYS_TIME]) != 0.0 ? 1 : 0;
    t->earth_radius_m = rf(D_EARTH_RADIUS);

    if (read_dref(&g_drefs[D_PAUSED]) != 0.0)
        t->flags |= CST_FLAG_PAUSED;
    if (read_dref(&g_drefs[D_REPLAY]) != 0.0)
        t->flags |= CST_FLAG_REPLAY;

    if (g_cfg.terrain_probe_hz != 0.0 && schedule_due(now, terrain_period, &g_terrain_due_set, &g_terrain_due))
        fill_terrain(t);
    if (schedule_due(now, weather_period, &g_weather_due_set, &g_weather_due))
        fill_weather(t);
}

static float flight_loop_cb(float since_last_call, float since_last_loop, int counter, void *refcon)
{
    cst_truth_v1 t;
    int err;
    (void)since_last_call; (void)since_last_loop; (void)counter; (void)refcon;

    build_datagram(&t, g_clock());
    err = cst_sender_send(&g_sender, &t, sizeof t);
    if (err != 0) {
        ++g_send_errors;
        if (err != g_last_send_error) { /* log each new error once, not every frame */
            char msg[128];
            snprintf(msg, sizeof msg, "sendto failed (error %d); datagrams are being dropped", err);
            cst_log(msg);
            g_last_send_error = err;
        }
    } else {
        g_last_send_error = 0;
    }
    return -1.0f; /* every frame */
}

/* ------------------------------------------------------------------------ */
/* Plugin entry points                                                       */

static void find_plugin_dir(void)
{
    char path[1024] = {0};
    char *slash;
    XPLMGetPluginInfo(XPLMGetMyID(), NULL, path, NULL, NULL);
    g_plugin_dir[0] = '\0';
    slash = strrchr(path, '/');
#if defined(_WIN32)
    {
        char *bs = strrchr(path, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
    }
#endif
    if (!slash)
        return;
    *slash = '\0';
    snprintf(g_plugin_dir, sizeof g_plugin_dir, "%s", path);
}

static int load_config(char *used, size_t used_len)
{
    /* <plugin>/CamSimTruth.ini (next to the platform folders), then
     * <plugin>/<plat>_x64/CamSimTruth.ini (next to the .xpl). */
    char a[1100], b[1100];
    const char *dir = g_plugin_dir;
    char parent[1024];
    char *slash;
    snprintf(parent, sizeof parent, "%s", dir);
    slash = strrchr(parent, '/');
#if defined(_WIN32)
    {
        char *bs = strrchr(parent, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
    }
#endif
    if (slash)
        *slash = '\0';
    cst_config_defaults(&g_cfg);
    if (slash) {
        snprintf(a, sizeof a, "%s/CamSimTruth.ini", parent);
        if (cst_config_load(&g_cfg, a, cst_log)) {
            snprintf(used, used_len, "%s", a);
            return 1;
        }
    }
    snprintf(b, sizeof b, "%s/CamSimTruth.ini", dir);
    if (dir[0] && cst_config_load(&g_cfg, b, cst_log)) {
        snprintf(used, used_len, "%s", b);
        return 1;
    }
    snprintf(used, used_len, "defaults (no CamSimTruth.ini in %s)", slash ? parent : dir);
    return 0;
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
    strcpy(outName, "CamSimTruth");
    strcpy(outSig, "camsim.hitl.truth");
    strcpy(outDesc, "Read-only truth state to the CamSim HITL IG host over UDP");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    g_net_ok = cst_net_init() == 0;
    if (!g_net_ok)
        cst_log("socket library init failed");
    return 1;
}

PLUGIN_API void XPluginStop(void)
{
    if (g_net_ok)
        cst_net_cleanup();
    g_net_ok = 0;
}

PLUGIN_API int XPluginEnable(void)
{
    char used[1200], msg[2048], err[256];
    char missing[1024] = "";
    char probe_desc[64];
    int i;
    XPLMCreateFlightLoop_t fl;

    find_plugin_dir();
    load_config(used, sizeof used);

    if (!g_cfg.enabled) {
        snprintf(msg, sizeof msg, "%s (protocol v%u) disabled by config (%s); sending nothing", CST_PLUGIN_VERSION, CST_VERSION, used);
        cst_log(msg);
        return 1;
    }

    /* Dataref handles, once per enable. */
    for (i = 0; i < D_COUNT; ++i) {
        bind_dref(&g_drefs[i]);
        if (!g_drefs[i].ref) {
            size_t n = strlen(missing);
            if (g_drefs[i].index >= 0)
                snprintf(missing + n, sizeof missing - n, "%s%s[%d]", n ? ", " : "", g_drefs[i].name, g_drefs[i].index);
            else
                snprintf(missing + n, sizeof missing - n, "%s%s", n ? ", " : "", g_drefs[i].name);
        }
    }

    if (!g_net_ok || cst_sender_open(&g_sender, g_cfg.host, g_cfg.port, err, sizeof err) != 0) {
        snprintf(msg, sizeof msg, "cannot open UDP sender to %s:%d: %s; sending nothing", g_cfg.host, g_cfg.port,
                 g_net_ok ? err : "socket library unavailable");
        cst_log(msg);
        return 1;
    }

    g_probe = g_cfg.terrain_probe_hz != 0.0 ? XPLMCreateProbe(xplm_ProbeY) : NULL;
    g_weather_due_set = g_terrain_due_set = 0;
    g_send_errors = 0;
    g_last_send_error = 0;

    memset(&fl, 0, sizeof fl);
    fl.structSize = (int)sizeof fl;
    fl.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc = flight_loop_cb;
    fl.refcon = NULL;
    g_loop = XPLMCreateFlightLoop(&fl);
    XPLMScheduleFlightLoop(g_loop, -1.0f, 1);

    if (g_cfg.terrain_probe_hz < 0.0)
        snprintf(probe_desc, sizeof probe_desc, "every datagram");
    else if (g_cfg.terrain_probe_hz == 0.0)
        snprintf(probe_desc, sizeof probe_desc, "off");
    else
        snprintf(probe_desc, sizeof probe_desc, "%g Hz", g_cfg.terrain_probe_hz);
    snprintf(msg, sizeof msg,
             "%s (protocol v%u, XPLM303) sending %u-byte datagrams to %s:%d every flight loop; "
             "weather every %g s, terrain probe %s; config: %s",
             CST_PLUGIN_VERSION, CST_VERSION, CST_PACKET_SIZE, g_cfg.host, g_cfg.port, g_cfg.weather_period_s,
             probe_desc, used);
    cst_log(msg);
    if (missing[0]) {
        snprintf(msg, sizeof msg, "missing datarefs (sent as 0, flags clear): %s", missing);
        cst_log(msg);
    } else {
        cst_log("all datarefs found");
    }
    return 1;
}

PLUGIN_API void XPluginDisable(void)
{
    if (g_loop) {
        XPLMDestroyFlightLoop(g_loop);
        g_loop = NULL;
    }
    if (g_probe) {
        XPLMDestroyProbe(g_probe);
        g_probe = NULL;
    }
    if (g_sender.sock != -1) {
        char msg[128];
        cst_sender_close(&g_sender);
        snprintf(msg, sizeof msg, "disabled; %u datagrams sent since load (%lu send errors this session)", g_seq, g_send_errors);
        cst_log(msg);
    }
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg, void *inParam)
{
    (void)inFrom; (void)inMsg; (void)inParam;
}
