/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * CamSimTruth test harness: runs the real plugin code against a stub XPLM
 * (xplm_stub.c), receives its UDP datagrams on 127.0.0.1 and checks every
 * field and flag against hitl/PROTOCOL.md section 1. Offsets here are typed in
 * from PROTOCOL.md, independently of truth_packet.h.
 *
 * usage: camsim_truth_test <work_dir> <dump_file>
 */
#define _POSIX_C_SOURCE 200809L
#include "xplm_stub.h"

#include "cst_config.h"
#include "cst_internal.h"
#include "cst_platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Plugin entry points (exported by the .xpl; linked directly here). */
int XPluginStart(char *outName, char *outSig, char *outDesc);
void XPluginStop(void);
int XPluginEnable(void);
void XPluginDisable(void);
void XPluginReceiveMessage(int inFrom, int inMsg, void *inParam);

/* ------------------------------------------------------------------------ */

static int g_failures, g_checks;
static const char *g_ctx = "";

static void fail(const char *fmt, ...)
{
    va_list ap;
    ++g_failures;
    fprintf(stderr, "FAIL [%s] ", g_ctx);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) fail(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------ */
/* Fake clock                                                                */

static uint64_t g_fake_ns;
static uint64_t fake_clock(void) { return g_fake_ns; }
#define STEP_NS 31250000ull /* 1/32 s: exact in binary, so cadence checks are exact */

/* ------------------------------------------------------------------------ */
/* Receiver                                                                  */

static int g_rx = -1;
static int g_rx_port;

static void rx_open(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    g_rx = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (g_rx < 0 || bind(g_rx, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(g_rx, (struct sockaddr *)&a, &len) != 0) {
        perror("receiver");
        exit(2);
    }
    g_rx_port = ntohs(a.sin_port);
}

/* Returns the datagram length, or -1 on timeout. */
static int rx_recv(unsigned char *buf, int cap, int timeout_ms)
{
    struct pollfd p = {g_rx, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    return (int)recv(g_rx, buf, (size_t)cap, 0);
}

static void rx_drain(void)
{
    unsigned char b[512];
    while (rx_recv(b, sizeof b, 50) >= 0) {
    }
}

/* ------------------------------------------------------------------------ */
/* Little-endian field readers at PROTOCOL.md offsets                        */

static uint16_t u16_at(const unsigned char *b, int o) { return (uint16_t)(b[o] | b[o + 1] << 8); }
static uint32_t u32_at(const unsigned char *b, int o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
static uint64_t u64_at(const unsigned char *b, int o) { return (uint64_t)u32_at(b, o) | (uint64_t)u32_at(b, o + 4) << 32; }
static float f32_at(const unsigned char *b, int o) { uint32_t u = u32_at(b, o); float f; memcpy(&f, &u, 4); return f; }
static double f64_at(const unsigned char *b, int o) { uint64_t u = u64_at(b, o); double d; memcpy(&d, &u, 8); return d; }

/* ------------------------------------------------------------------------ */
/* Dataref table: what the fake X-Plane publishes and where it must land     */

typedef enum { K_F64, K_F32, K_U8, K_BOOL8 } kind_t;

typedef struct field {
    const char *dref;      /* name as XP11 publishes it */
    XPLMDataTypeID type;
    int offset;            /* PROTOCOL.md */
    kind_t kind;
    double base;           /* value at frame 0 */
    double step;           /* + step per frame */
    int weather;           /* in the weather block */
} field;

#define T_D xplmType_Double
#define T_F xplmType_Float
#define T_I xplmType_Int
#define POS "sim/flightmodel/position/"

static const field FIELDS[] = {
    {POS "latitude", T_D, 24, K_F64, 34.123456789012345, 1.0e-7, 0},
    {POS "longitude", T_D, 32, K_F64, -116.987654321098765, -1.0e-7, 0},
    {POS "elevation", T_D, 40, K_F64, 812.3456789, 0.125, 0},
    {POS "true_psi", T_F, 56, K_F32, 271.5, 0.25, 0},
    {POS "true_theta", T_F, 60, K_F32, 3.25, 0.5, 0},
    {POS "true_phi", T_F, 64, K_F32, -12.75, 0.5, 0},
    {POS "Prad", T_F, 68, K_F32, 0.125, 0.0625, 0},
    {POS "Qrad", T_F, 72, K_F32, -0.25, 0.0625, 0},
    {POS "Rrad", T_F, 76, K_F32, 0.375, 0.0625, 0},
    {POS "local_vx", T_F, 80, K_F32, 41.5, 1.0, 0},
    {POS "local_vy", T_F, 84, K_F32, -2.25, 1.0, 0},
    {POS "local_vz", T_F, 88, K_F32, -17.75, 1.0, 0},
    {POS "true_airspeed", T_F, 92, K_F32, 45.5, 1.0, 0},
    {POS "groundspeed", T_F, 96, K_F32, 44.25, 1.0, 0},
    {POS "indicated_airspeed", T_F, 100, K_F32, 86.5, 1.0, 0},
    {POS "mag_psi", T_F, 104, K_F32, 259.25, 0.25, 0},
    {POS "y_agl", T_F, 108, K_F32, 304.5, 1.0, 0},
    {"sim/time/sim_speed", T_I, 112, K_F32, 1, 0, 0},
    {"sim/time/sim_speed_actual_ogl", T_F, 116, K_F32, 0.9921875, 0, 0},
    {"sim/time/zulu_time_sec", T_F, 120, K_F32, 61200.5, 0.03125, 0},
    {"sim/time/local_time_sec", T_F, 124, K_F32, 32400.5, 0.03125, 0},
    {"sim/cockpit2/clock_timer/current_month", T_I, 128, K_U8, 7, 0, 0},
    {"sim/cockpit2/clock_timer/current_day", T_I, 129, K_U8, 4, 1, 0},
    {"sim/time/use_system_time", T_I, 130, K_BOOL8, 1, 0, 0},
    {"sim/physics/earth_radius_m", T_F, 132, K_F32, 6378145.0, 0, 0},
    {"sim/weather/visibility_reported_m", T_F, 136, K_F32, 16093.0, 1.0, 1},
    {"sim/weather/temperature_ambient_c", T_F, 140, K_F32, 21.5, 0.25, 1},
    {"sim/weather/relative_humidity_sealevel_percent", T_F, 144, K_F32, 40.5, 0.5, 1},
    {"sim/weather/wind_speed_kt", T_F, 148, K_F32, 5.25, 0.25, 1},
    {"sim/weather/wind_direction_degt", T_F, 152, K_F32, 245.5, 1.0, 1},
    {"sim/weather/barometer_sealevel_inhg", T_F, 156, K_F32, 29.875, 0.0, 1},
    {"sim/weather/cloud_base_msl_m[0]", T_F, 160, K_F32, 1500.5, 1.0, 1},
    {"sim/weather/cloud_base_msl_m[1]", T_F, 164, K_F32, 3000.5, 1.0, 1},
    {"sim/weather/cloud_base_msl_m[2]", T_F, 168, K_F32, 6000.5, 1.0, 1},
    {"sim/weather/cloud_tops_msl_m[0]", T_F, 172, K_F32, 2000.25, 1.0, 1},
    {"sim/weather/cloud_tops_msl_m[1]", T_F, 176, K_F32, 4000.25, 1.0, 1},
    {"sim/weather/cloud_tops_msl_m[2]", T_F, 180, K_F32, 7000.25, 1.0, 1},
    {"sim/weather/cloud_coverage[0]", T_F, 184, K_F32, 2.5, 0.0, 1},
    {"sim/weather/cloud_coverage[1]", T_F, 188, K_F32, 4.0, 0.0, 1},
    {"sim/weather/cloud_coverage[2]", T_F, 192, K_F32, 0.5, 0.0, 1},
    {"sim/weather/cloud_type[0]", T_I, 196, K_F32, 2, 0, 1},
    {"sim/weather/cloud_type[1]", T_I, 200, K_F32, 3, 0, 1},
    {"sim/weather/cloud_type[2]", T_I, 204, K_F32, 1, 0, 1},
    {"sim/weather/wave_amplitude", T_F, 208, K_F32, 0.75, 0.0, 1},
    {"sim/weather/wave_length", T_F, 212, K_F32, 22.5, 0.0, 1},
    {"sim/weather/wave_speed", T_F, 216, K_F32, 5.5, 0.0, 1},
    {"sim/weather/wave_dir", T_I, 220, K_F32, 135, 1, 1},
};
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static const char *PAUSED = "sim/time/paused";
static const char *REPLAY = "sim/time/is_in_replay";

/* frames are numbered from 0 within a scenario */
static double value_at(const field *f, int frame) { return f->base + f->step * frame; }

static int is_removed(const char *name, const char *const *removed)
{
    for (; removed && *removed; ++removed)
        if (strcmp(*removed, name) == 0)
            return 1;
    return 0;
}

/* XP11-style world: weather layers as scalars named "name[i]", plus decoys the
 * plugin must not pick (the per-layer wind is knots, the effective one m/s). */
static void publish_world(int frame, const char *const *removed, int paused, int replay)
{
    int i;
    for (i = 0; i < NFIELDS; ++i) {
        if (is_removed(FIELDS[i].dref, removed))
            stub_remove(FIELDS[i].dref);
        else
            stub_set(FIELDS[i].dref, FIELDS[i].type, value_at(&FIELDS[i], frame));
    }
    if (!is_removed(PAUSED, removed)) stub_set(PAUSED, T_I, paused);
    if (!is_removed(REPLAY, removed)) stub_set(REPLAY, T_I, replay);
    stub_set("sim/weather/wind_speed_kt[0]", T_F, 999.0);
    stub_set("sim/weather/wind_direction_degt[0]", T_F, 999.0);
    stub_set("sim/flightmodel/position/psi", T_F, 999.0);
}

static double terrain_at(int frame) { return 600.25 + frame; }

typedef struct expect {
    int frame;          /* scenario frame number (dataref values) */
    uint32_t seq;
    int weather;        /* bit2 expected */
    int terrain;        /* bit3 expected */
    int paused, replay;
    const char *const *removed;
} expect;

static void check_datagram(const unsigned char *b, int len, const expect *e)
{
    int i;
    uint16_t flags;
    CHECK(len == 224, "frame %d: length %d", e->frame, len);
    if (len != 224)
        return;
    CHECK(memcmp(b, "CSTR", 4) == 0, "frame %d: magic", e->frame);
    CHECK(u16_at(b, 4) == 1, "frame %d: version %u", e->frame, u16_at(b, 4));
    flags = u16_at(b, 6);
    CHECK(((flags >> 0) & 1) == (e->paused && !is_removed(PAUSED, e->removed)), "frame %d: paused bit (flags %#x)", e->frame, flags);
    CHECK(((flags >> 1) & 1) == (e->replay && !is_removed(REPLAY, e->removed)), "frame %d: replay bit (flags %#x)", e->frame, flags);
    CHECK(((flags >> 2) & 1) == e->weather, "frame %d: weather bit (flags %#x)", e->frame, flags);
    CHECK(((flags >> 3) & 1) == e->terrain, "frame %d: terrain bit (flags %#x)", e->frame, flags);
    CHECK((flags & ~0xFu) == 0, "frame %d: undefined flag bits set (%#x)", e->frame, flags);
    CHECK(u32_at(b, 8) == (uint32_t)stub_cycle(), "frame %d: cycle %u != %d", e->frame, u32_at(b, 8), stub_cycle());
    CHECK(u32_at(b, 12) == e->seq, "frame %d: seq %u != %u", e->frame, u32_at(b, 12), e->seq);
    CHECK(u64_at(b, 16) == g_fake_ns, "frame %d: mono_ns %llu != %llu", e->frame, (unsigned long long)u64_at(b, 16), (unsigned long long)g_fake_ns);
    if (e->terrain)
        CHECK(f64_at(b, 48) == terrain_at(e->frame), "frame %d: terrain_msl %.6f", e->frame, f64_at(b, 48));
    else
        CHECK(f64_at(b, 48) == 0.0, "frame %d: terrain_msl not zero without bit3", e->frame);
    CHECK(b[131] == 0, "frame %d: reserved byte", e->frame);

    for (i = 0; i < NFIELDS; ++i) {
        const field *f = &FIELDS[i];
        double v = value_at(f, e->frame);
        int zero = is_removed(f->dref, e->removed) || (f->weather && !e->weather);
        switch (f->kind) {
        case K_F64: {
            double got = f64_at(b, f->offset);
            CHECK(got == (zero ? 0.0 : v), "frame %d: %s @%d = %.15g, want %.15g", e->frame, f->dref, f->offset, got, zero ? 0.0 : v);
            break;
        }
        case K_F32: {
            float got = f32_at(b, f->offset);
            float want = zero ? 0.0f : (float)v;
            CHECK(got == want, "frame %d: %s @%d = %.9g, want %.9g", e->frame, f->dref, f->offset, got, want);
            break;
        }
        case K_U8:
        case K_BOOL8: {
            int want = zero ? 0 : (f->kind == K_BOOL8 ? v != 0 : (int)v);
            CHECK(b[f->offset] == want, "frame %d: %s @%d = %d, want %d", e->frame, f->dref, f->offset, b[f->offset], want);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Scenario plumbing                                                         */

static char g_work[1024];

static void mkdirs(const char *path)
{
    char tmp[1024];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; ++p)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* Lays out <work>/<name>/CamSimTruth/lin_x64/ and writes the ini (NULL: none)
 * into the plugin folder (in_platform_dir = 0) or next to the .xpl (1). */
static void setup_install(const char *name, const char *ini, int in_platform_dir)
{
    char dir[1100], path[1200];
    FILE *f;
    snprintf(dir, sizeof dir, "%s/%s/CamSimTruth/lin_x64", g_work, name);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/CamSimTruth.xpl", dir);
    stub_set_plugin_path(path);
    snprintf(path, sizeof path, "%s/%s/CamSimTruth/CamSimTruth.ini", g_work, name);
    remove(path);
    snprintf(path, sizeof path, "%s/CamSimTruth.ini", dir);
    remove(path);
    if (!ini)
        return;
    if (in_platform_dir)
        snprintf(path, sizeof path, "%s/CamSimTruth.ini", dir);
    else
        snprintf(path, sizeof path, "%s/%s/CamSimTruth/CamSimTruth.ini", g_work, name);
    f = fopen(path, "w");
    fputs(ini, f);
    fclose(f);
}

static uint32_t g_expected_seq;

/* Runs one flight loop and receives its datagram into buf; returns its length. */
static int run_frame(unsigned char *buf, int cap)
{
    float ret = stub_run_flight_loop();
    CHECK(ret == -1.0f, "flight loop returned %g, want -1 (every frame)", ret);
    return rx_recv(buf, cap, 1000);
}

/* ------------------------------------------------------------------------ */
/* Scenarios                                                                 */

static void test_config_parser(void)
{
    cst_config c;
    g_ctx = "config";
    cst_config_defaults(&c);
    CHECK(strcmp(c.host, "127.0.0.1") == 0 && c.port == 49300 && c.enabled == 1, "defaults");
    CHECK(c.weather_period_s == 1.0 && c.terrain_probe_hz < 0.0, "default rates");
    CHECK(cst_config_parse_dest(&c, "10.1.2.3:5000") == 0 && strcmp(c.host, "10.1.2.3") == 0 && c.port == 5000, "ipv4 dest");
    CHECK(cst_config_parse_dest(&c, "[::1]:6000") == 0 && strcmp(c.host, "::1") == 0 && c.port == 6000, "ipv6 dest");
    CHECK(cst_config_parse_dest(&c, "ig-host.local:49300") == 0 && strcmp(c.host, "ig-host.local") == 0, "name dest");
    CHECK(cst_config_parse_dest(&c, "nohost") != 0, "dest without port rejected");
    CHECK(cst_config_parse_dest(&c, "h:0") != 0 && cst_config_parse_dest(&c, "h:70000") != 0, "bad port rejected");
}

/* 1. Full XP11 world, default terrain probe (every datagram), weather 1 s.
 *    Checks every field of 70 datagrams, the 1 s weather cadence (frames 0, 32,
 *    64 at 32 fps), paused/replay bits, and that handles are cached. */
static void test_full(const char *dump_path)
{
    unsigned char buf[512];
    char ini[256];
    int frame, finds_after_enable;
    g_ctx = "full";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "# test\ndest = 127.0.0.1:%d\nweather_period_s = 1.0\n", g_rx_port);
    setup_install("full", ini, 0);
    g_fake_ns = 5000000000ull;

    {
        char name[256], sig[256], desc[256];
        CHECK(XPluginStart(name, sig, desc) == 1, "XPluginStart");
        CHECK(strcmp(name, "CamSimTruth") == 0, "plugin name");
        CHECK(stub_native_paths_enabled(), "XPLM_USE_NATIVE_PATHS enabled");
    }
    CHECK(XPluginEnable() == 1, "XPluginEnable");
    CHECK(stub_flight_loop_exists(), "flight loop created");
    CHECK(stub_flight_loop_phase() == xplm_FlightLoop_Phase_AfterFlightModel, "phase AfterFlightModel");
    CHECK(stub_flight_loop_interval() == -1.0f, "scheduled every frame");
    CHECK(stub_probe_exists(), "probe created");
    CHECK(stub_log_count("CamSimTruth: 1.0.0") == 1, "startup line");
    {
        char want[64];
        snprintf(want, sizeof want, "to 127.0.0.1:%d every flight loop", g_rx_port);
        CHECK(strstr(stub_log(), want) != NULL, "dest in startup line:\n%s", stub_log());
    }
    CHECK(stub_log_count("all datarefs found") == 1, "no missing datarefs:\n%s", stub_log());
    finds_after_enable = stub_find_calls();

    for (frame = 0; frame < 70; ++frame) {
        int paused = frame % 5 == 1, replay = frame % 7 == 3;
        expect e = {frame, g_expected_seq, frame % 32 == 0, 1, paused, replay, NULL};
        int len;
        publish_world(frame, NULL, paused, replay);
        stub_set_probe(xplm_ProbeHitTerrain, terrain_at(frame));
        len = run_frame(buf, sizeof buf);
        CHECK(len >= 0, "frame %d: no datagram", frame);
        if (len >= 0) {
            check_datagram(buf, len, &e);
            if (frame == 0 && dump_path) {
                FILE *f = fopen(dump_path, "wb");
                if (f) { fwrite(buf, 1, (size_t)len, f); fclose(f); }
            }
        }
        if (frame == 40) {
            double lat, lon;
            stub_last_probe_point(&lat, &lon);
            CHECK(fabs(lat - value_at(&FIELDS[0], frame)) < 1e-4 && fabs(lon - value_at(&FIELDS[1], frame)) < 1e-4,
                  "probe under the aircraft (%.6f, %.6f)", lat, lon);
        }
        ++g_expected_seq;
        g_fake_ns += STEP_NS;
    }
    CHECK(stub_find_calls() == finds_after_enable, "datarefs looked up in the flight loop (%d -> %d)", finds_after_enable, stub_find_calls());
    CHECK(stub_probe_calls() == 70, "probe every frame (%d)", stub_probe_calls());
    CHECK(rx_recv(buf, sizeof buf, 100) < 0, "extra datagram");

    /* Stall (e.g. a loading pause) of 5 s: weather refreshes on the next frame,
     * then 1 s later, not in a burst. */
    g_fake_ns += 5000000000ull;
    for (frame = 70; frame < 110; ++frame) {
        expect e = {frame, g_expected_seq, frame == 70 || frame == 102, 1, 0, 0, NULL};
        int len;
        publish_world(frame, NULL, 0, 0);
        stub_set_probe(xplm_ProbeHitTerrain, terrain_at(frame));
        len = run_frame(buf, sizeof buf);
        CHECK(len >= 0, "frame %d: no datagram", frame);
        if (len >= 0)
            check_datagram(buf, len, &e);
        ++g_expected_seq;
        g_fake_ns += STEP_NS;
    }

    XPluginDisable();
    CHECK(!stub_flight_loop_exists(), "flight loop destroyed on disable");
    CHECK(!stub_probe_exists(), "probe destroyed on disable");
    CHECK(stub_log_count("send errors") == 1, "disable summary");
}

/* 2. Terrain probe at 8 Hz (every 4th frame at 32 fps) with a miss. */
static void test_terrain_rate(void)
{
    unsigned char buf[512];
    char ini[256];
    int frame;
    g_ctx = "terrain_8hz";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\nterrain_probe_hz = 8\nweather_period_s = 10 ; long\n", g_rx_port);
    setup_install("terrain", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(strstr(stub_log(), "terrain probe 8 Hz") != NULL, "rate in startup line");
    for (frame = 0; frame < 20; ++frame) {
        int due = frame % 4 == 0, miss = frame == 8;
        expect e = {frame, g_expected_seq, frame == 0, due && !miss, 0, 0, NULL};
        int len;
        publish_world(frame, NULL, 0, 0);
        stub_set_probe(miss ? xplm_ProbeMissed : xplm_ProbeHitTerrain, terrain_at(frame));
        len = run_frame(buf, sizeof buf);
        CHECK(len >= 0, "frame %d: no datagram", frame);
        if (len >= 0)
            check_datagram(buf, len, &e);
        ++g_expected_seq;
        g_fake_ns += STEP_NS;
    }
    CHECK(stub_probe_calls() == 5, "5 probes in 20 frames (%d)", stub_probe_calls());
    XPluginDisable();

    /* terrain_probe_hz = 0: no probe at all, bit3 never set; weather every datagram */
    g_ctx = "terrain_off";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\nterrain_probe_hz = 0\nweather_period_s = 0\n", g_rx_port);
    setup_install("terrain_off", ini, 1); /* ini next to the .xpl */
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(!stub_probe_exists(), "no probe when off");
    for (frame = 0; frame < 5; ++frame) {
        expect e = {frame, g_expected_seq, 1, 0, 0, 0, NULL};
        int len;
        publish_world(frame, NULL, 0, 0);
        len = run_frame(buf, sizeof buf);
        CHECK(len >= 0, "frame %d: no datagram", frame);
        if (len >= 0)
            check_datagram(buf, len, &e);
        ++g_expected_seq;
        g_fake_ns += STEP_NS;
    }
    XPluginDisable();
}

/* 3. Version-dependent datarefs missing (X-Plane older than 11.35 / odd builds):
 *    sent as 0, flags clear, logged once, no crash. Weather every 0.5 s. */
static void test_missing(void)
{
    static const char *const removed[] = {
        "sim/flightmodel/position/true_psi",
        "sim/time/is_in_replay",
        "sim/time/sim_speed_actual_ogl",
        "sim/weather/relative_humidity_sealevel_percent",
        "sim/weather/cloud_base_msl_m[1]",
        "sim/time/paused",
        NULL};
    unsigned char buf[512];
    char ini[256];
    int frame;
    g_ctx = "missing";
    stub_reset();
    publish_world(0, removed, 1, 1);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\nweather_period_s = 0.5\n", g_rx_port);
    setup_install("missing", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(stub_log_count("missing datarefs") == 1, "missing line logged once");
    CHECK(strstr(stub_log(), "sim/flightmodel/position/true_psi") && strstr(stub_log(), "sim/time/is_in_replay") &&
          strstr(stub_log(), "sim/time/sim_speed_actual_ogl") && strstr(stub_log(), "relative_humidity_sealevel_percent") &&
          strstr(stub_log(), "sim/weather/cloud_base_msl_m[1]") && strstr(stub_log(), "sim/time/paused"),
          "missing names in log:\n%s", stub_log());
    for (frame = 0; frame < 40; ++frame) {
        expect e = {frame, g_expected_seq, frame % 16 == 0, 1, 1, 1, removed};
        int len;
        publish_world(frame, removed, 1, 1);
        stub_set_probe(xplm_ProbeHitTerrain, terrain_at(frame));
        len = run_frame(buf, sizeof buf);
        CHECK(len >= 0, "frame %d: no datagram", frame);
        if (len >= 0)
            check_datagram(buf, len, &e);
        ++g_expected_seq;
        g_fake_ns += STEP_NS;
    }
    CHECK(stub_log_count("missing datarefs") == 1, "missing logged once, not per frame");
    XPluginDisable();
}

/* 4. X-Plane 12-style weather arrays ("name" float[3] / int[3], no "name[i]"):
 *    the plugin falls back to array elements. */
static void test_array_fallback(void)
{
    static const char *const layer_names[] = {
        "sim/weather/cloud_base_msl_m[0]", "sim/weather/cloud_base_msl_m[1]", "sim/weather/cloud_base_msl_m[2]",
        "sim/weather/cloud_tops_msl_m[0]", "sim/weather/cloud_tops_msl_m[1]", "sim/weather/cloud_tops_msl_m[2]",
        "sim/weather/cloud_coverage[0]", "sim/weather/cloud_coverage[1]", "sim/weather/cloud_coverage[2]",
        "sim/weather/cloud_type[0]", "sim/weather/cloud_type[1]", "sim/weather/cloud_type[2]", NULL};
    unsigned char buf[512];
    char ini[256];
    int i, len;
    double base[3], tops[3], cov[3], type[3];
    g_ctx = "xp12_arrays";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    for (i = 0; layer_names[i]; ++i)
        stub_remove(layer_names[i]);
    for (i = 0; i < 3; ++i) {
        base[i] = value_at(&FIELDS[31 + i], 0);
        tops[i] = value_at(&FIELDS[34 + i], 0);
        cov[i] = value_at(&FIELDS[37 + i], 0);
        type[i] = value_at(&FIELDS[40 + i], 0);
    }
    stub_set_array("sim/weather/cloud_base_msl_m", xplmType_FloatArray, 3, base);
    stub_set_array("sim/weather/cloud_tops_msl_m", xplmType_FloatArray, 3, tops);
    stub_set_array("sim/weather/cloud_coverage", xplmType_FloatArray, 3, cov);
    stub_set_array("sim/weather/cloud_type", xplmType_IntArray, 3, type);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\n", g_rx_port);
    setup_install("xp12", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(stub_log_count("all datarefs found") == 1, "arrays bound:\n%s", stub_log());
    stub_set_probe(xplm_ProbeHitTerrain, terrain_at(0));
    len = run_frame(buf, sizeof buf);
    CHECK(len >= 0, "no datagram");
    if (len >= 0) {
        expect e = {0, g_expected_seq, 1, 1, 0, 0, NULL};
        check_datagram(buf, len, &e); /* same expected values as the scalar layers */
    }
    ++g_expected_seq;
    g_fake_ns += STEP_NS;
    XPluginDisable();
}

/* 5. enabled = 0: nothing registered, nothing sent. */
static void test_disabled(void)
{
    unsigned char buf[512];
    char ini[256];
    g_ctx = "disabled";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\nenabled = false\n", g_rx_port);
    setup_install("disabled", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(!stub_flight_loop_exists(), "no flight loop when disabled");
    CHECK(stub_log_count("disabled by config") == 1, "disabled logged");
    CHECK(rx_recv(buf, sizeof buf, 100) < 0, "nothing sent");
    XPluginDisable();
}

/* 6. No ini: defaults (127.0.0.1:49300, weather 1 s, probe every datagram) and
 *    bad lines in an ini are reported, not fatal. */
static void test_defaults(void)
{
    char ini[256];
    g_ctx = "defaults";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    setup_install("defaults", NULL, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(strstr(stub_log(), "to 127.0.0.1:49300 ") != NULL, "default dest:\n%s", stub_log());
    CHECK(strstr(stub_log(), "weather every 1 s, terrain probe every datagram") != NULL, "default rates");
    CHECK(strstr(stub_log(), "config: defaults") != NULL, "defaults noted");
    XPluginDisable();

    g_ctx = "bad_ini";
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "[CamSimTruth]\ndest = 127.0.0.1:%d\nbogus = 1\nweather_period_s = soon\nterrain_probe_hz\n", g_rx_port);
    setup_install("bad_ini", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    CHECK(stub_log_count("unknown key 'bogus'") == 1, "unknown key reported");
    CHECK(stub_log_count("bad line ignored") == 2, "bad lines reported:\n%s", stub_log());
    CHECK(strstr(stub_log(), "weather every 1 s") != NULL, "bad value keeps default");
    XPluginDisable();
}

/* 7. The real monotonic clock: mono_ns increases and matches CLOCK_MONOTONIC. */
static void test_real_clock(void)
{
    unsigned char buf[512];
    char ini[256];
    uint64_t prev = 0;
    int frame;
    g_ctx = "real_clock";
    cst_test_set_clock(NULL);
    stub_reset();
    publish_world(0, NULL, 0, 0);
    snprintf(ini, sizeof ini, "dest = 127.0.0.1:%d\n", g_rx_port);
    setup_install("clock", ini, 0);
    CHECK(XPluginEnable() == 1, "enable");
    for (frame = 0; frame < 5; ++frame) {
        struct timespec ts, sl = {0, 2000000};
        uint64_t before, after, got;
        int len;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        before = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        len = run_frame(buf, sizeof buf);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        after = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        CHECK(len == 224, "datagram");
        got = u64_at(buf, 16);
        CHECK(got >= before && got <= after, "mono_ns %llu outside [%llu, %llu]", (unsigned long long)got, (unsigned long long)before, (unsigned long long)after);
        CHECK(got > prev, "mono_ns not increasing");
        CHECK(u32_at(buf, 12) == g_expected_seq, "seq");
        prev = got;
        ++g_expected_seq;
        nanosleep(&sl, NULL);
    }
    XPluginDisable();
    cst_test_set_clock(fake_clock);
}

int main(int argc, char **argv)
{
    const char *dump = argc > 2 ? argv[2] : NULL;
    snprintf(g_work, sizeof g_work, "%s", argc > 1 ? argv[1] : "./camsim_truth_test_work");
    mkdirs(g_work);
    rx_open();
    cst_test_set_clock(fake_clock);

    test_config_parser();
    test_full(dump);
    test_terrain_rate();
    test_missing();
    test_array_fallback();
    test_disabled();
    test_defaults();
    test_real_clock();
    rx_drain();
    XPluginStop();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
