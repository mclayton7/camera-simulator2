/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * CamSimTruth datagram, protocol version 1: hitl/PROTOCOL.md section 1.
 * Shared with the IG host's Python receiver; change both ends together.
 *
 * Little-endian, packed, 224 bytes, every field naturally aligned. The struct
 * below is copied to the wire as is, so the static asserts pin every offset to
 * the table in PROTOCOL.md and the build refuses big-endian hosts.
 */
#ifndef CAMSIM_TRUTH_PACKET_H
#define CAMSIM_TRUTH_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define CST_MAGIC "CSTR"
#define CST_VERSION 1u
#define CST_PACKET_SIZE 224u

/* flags */
#define CST_FLAG_PAUSED 0x0001u        /* bit0 sim/time/paused */
#define CST_FLAG_REPLAY 0x0002u        /* bit1 sim/time/is_in_replay */
#define CST_FLAG_WEATHER_VALID 0x0004u /* bit2 weather block (136..223) valid */
#define CST_FLAG_TERRAIN_VALID 0x0008u /* bit3 terrain_msl valid (probe hit) */

typedef struct cst_truth_v1 {
    char magic[4];          /*   0 "CSTR" */
    uint16_t version;       /*   4 */
    uint16_t flags;         /*   6 */
    uint32_t cycle;         /*   8 XPLMGetCycleNumber() */
    uint32_t seq;           /*  12 */
    uint64_t mono_ns;       /*  16 */
    double latitude;        /*  24 */
    double longitude;       /*  32 */
    double elevation;       /*  40 */
    double terrain_msl;     /*  48 */
    float true_psi;         /*  56 */
    float true_theta;       /*  60 */
    float true_phi;         /*  64 */
    float p;                /*  68 Prad */
    float q;                /*  72 Qrad */
    float r;                /*  76 Rrad */
    float local_vx;         /*  80 */
    float local_vy;         /*  84 */
    float local_vz;         /*  88 */
    float true_airspeed;    /*  92 */
    float groundspeed;      /*  96 */
    float indicated_airspeed; /* 100 */
    float mag_psi;          /* 104 */
    float y_agl;            /* 108 */
    float sim_speed;        /* 112 */
    float sim_speed_actual_ogl; /* 116 */
    float zulu_time_sec;    /* 120 */
    float local_time_sec;   /* 124 */
    uint8_t local_month;    /* 128 */
    uint8_t local_day;      /* 129 */
    uint8_t use_system_time; /* 130 */
    uint8_t reserved;       /* 131 */
    float earth_radius_m;   /* 132 */
    /* weather block (flag bit2) */
    float visibility_m;     /* 136 */
    float temperature_c;    /* 140 */
    float humidity_pct;     /* 144 */
    float wind_speed;       /* 148 */
    float wind_dir_degt;    /* 152 */
    float baro_inhg;        /* 156 */
    float cloud_base_msl_m[3]; /* 160 */
    float cloud_tops_msl_m[3]; /* 172 */
    float cloud_coverage[3];   /* 184 */
    float cloud_type[3];       /* 196 */
    float wave_amplitude;   /* 208 */
    float wave_length;      /* 212 */
    float wave_speed;       /* 216 */
    float wave_dir;         /* 220 */
} cst_truth_v1;             /* 224 */

#if defined(__cplusplus)
#define CST_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define CST_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
CST_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CamSimTruth sends its struct raw: little-endian hosts only");
#elif !defined(_WIN32)
#error "Cannot determine byte order; CamSimTruth needs a little-endian host"
#endif

CST_STATIC_ASSERT(sizeof(float) == 4 && sizeof(double) == 8, "IEEE-754 binary32/64 required");
CST_STATIC_ASSERT(sizeof(cst_truth_v1) == CST_PACKET_SIZE, "datagram must be 224 bytes");

#define CST_AT(field, off) CST_STATIC_ASSERT(offsetof(cst_truth_v1, field) == (off), "offset of " #field " differs from PROTOCOL.md")
CST_AT(magic, 0);
CST_AT(version, 4);
CST_AT(flags, 6);
CST_AT(cycle, 8);
CST_AT(seq, 12);
CST_AT(mono_ns, 16);
CST_AT(latitude, 24);
CST_AT(longitude, 32);
CST_AT(elevation, 40);
CST_AT(terrain_msl, 48);
CST_AT(true_psi, 56);
CST_AT(true_theta, 60);
CST_AT(true_phi, 64);
CST_AT(p, 68);
CST_AT(q, 72);
CST_AT(r, 76);
CST_AT(local_vx, 80);
CST_AT(local_vy, 84);
CST_AT(local_vz, 88);
CST_AT(true_airspeed, 92);
CST_AT(groundspeed, 96);
CST_AT(indicated_airspeed, 100);
CST_AT(mag_psi, 104);
CST_AT(y_agl, 108);
CST_AT(sim_speed, 112);
CST_AT(sim_speed_actual_ogl, 116);
CST_AT(zulu_time_sec, 120);
CST_AT(local_time_sec, 124);
CST_AT(local_month, 128);
CST_AT(local_day, 129);
CST_AT(use_system_time, 130);
CST_AT(reserved, 131);
CST_AT(earth_radius_m, 132);
CST_AT(visibility_m, 136);
CST_AT(temperature_c, 140);
CST_AT(humidity_pct, 144);
CST_AT(wind_speed, 148);
CST_AT(wind_dir_degt, 152);
CST_AT(baro_inhg, 156);
CST_AT(cloud_base_msl_m, 160);
CST_AT(cloud_tops_msl_m, 172);
CST_AT(cloud_coverage, 184);
CST_AT(cloud_type, 196);
CST_AT(wave_amplitude, 208);
CST_AT(wave_length, 212);
CST_AT(wave_speed, 216);
CST_AT(wave_dir, 220);
#undef CST_AT

#endif /* CAMSIM_TRUTH_PACKET_H */
