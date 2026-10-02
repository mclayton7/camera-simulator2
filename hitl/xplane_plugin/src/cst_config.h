/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * CamSimTruth.ini: "key = value" lines, '#' or ';' comments. Keys:
 *   dest             = host:port  (default 127.0.0.1:49300; [v6]:port for IPv6)
 *   enabled          = 1|0|true|false|yes|no|on|off   (default 1)
 *   weather_period_s = seconds between weather refreshes (default 1.0; 0 = every datagram)
 *   terrain_probe_hz = terrain probes per second (default: every datagram; 0 = off;
 *                      negative = every datagram)
 */
#ifndef CAMSIM_TRUTH_CONFIG_H
#define CAMSIM_TRUTH_CONFIG_H

#include <stddef.h>

typedef struct cst_config {
    char host[256];
    int port;
    int enabled;
    double weather_period_s;
    double terrain_probe_hz; /* < 0: every datagram, 0: off */
} cst_config;

void cst_config_defaults(cst_config *c);

/* Applies the file's settings on top of c. Returns 1 if the file was read,
 * 0 if it doesn't exist. Bad lines are reported through warn (may be NULL)
 * and otherwise ignored. */
int cst_config_load(cst_config *c, const char *path, void (*warn)(const char *msg));

/* Parses "host:port" / "[v6]:port" into c. Returns 0 on success. */
int cst_config_parse_dest(cst_config *c, const char *value);

#endif
