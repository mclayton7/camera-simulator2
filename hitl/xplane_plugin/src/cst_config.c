/* Copyright CamSim Contributors. All Rights Reserved. */
#include "cst_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cst_config_defaults(cst_config *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->host, "127.0.0.1");
    c->port = 49300;
    c->enabled = 1;
    c->weather_period_s = 1.0;
    c->terrain_probe_hz = -1.0; /* every datagram */
}

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        ++s;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        --e;
    *e = '\0';
    return s;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int parse_bool(const char *v, int *out)
{
    if (ieq(v, "1") || ieq(v, "true") || ieq(v, "yes") || ieq(v, "on")) { *out = 1; return 0; }
    if (ieq(v, "0") || ieq(v, "false") || ieq(v, "no") || ieq(v, "off")) { *out = 0; return 0; }
    return -1;
}

static int parse_double(const char *v, double *out)
{
    char *end;
    double d = strtod(v, &end);
    if (end == v || *trim(end) != '\0')
        return -1;
    *out = d;
    return 0;
}

int cst_config_parse_dest(cst_config *c, const char *value)
{
    char buf[300];
    char *host, *colon, *end;
    long port;

    if (strlen(value) >= sizeof buf)
        return -1;
    strcpy(buf, value);
    host = trim(buf);
    if (*host == '[') { /* [v6]:port */
        char *rb = strchr(host, ']');
        if (!rb || rb[1] != ':')
            return -1;
        *rb = '\0';
        colon = rb + 1;
        ++host;
    } else {
        colon = strrchr(host, ':');
        if (!colon)
            return -1;
        *colon = '\0';
    }
    port = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535)
        return -1;
    if (*host == '\0' || strlen(host) >= sizeof c->host)
        return -1;
    strcpy(c->host, host);
    c->port = (int)port;
    return 0;
}

int cst_config_load(cst_config *c, const char *path, void (*warn)(const char *msg))
{
    char line[512], msg[640];
    int lineno = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val, *hash;
        int ok = 0;
        ++lineno;
        /* strip comments ('#' or ';' anywhere: neither appears in valid values) */
        if ((hash = strpbrk(line, "#;")) != NULL)
            *hash = '\0';
        s = trim(line);
        if (*s == '\0' || *s == '[') /* blank or [section] header: ignored */
            continue;
        eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            key = trim(s);
            val = trim(eq + 1);
            if (ieq(key, "dest")) {
                ok = cst_config_parse_dest(c, val) == 0;
            } else if (ieq(key, "enabled")) {
                ok = parse_bool(val, &c->enabled) == 0;
            } else if (ieq(key, "weather_period_s")) {
                double d;
                ok = parse_double(val, &d) == 0 && d >= 0.0;
                if (ok)
                    c->weather_period_s = d;
            } else if (ieq(key, "terrain_probe_hz")) {
                double d;
                ok = parse_double(val, &d) == 0;
                if (ok)
                    c->terrain_probe_hz = d;
            } else {
                if (warn) {
                    snprintf(msg, sizeof msg, "%s:%d: unknown key '%s' ignored", path, lineno, key);
                    warn(msg);
                }
                continue;
            }
        }
        if (!ok && warn) {
            snprintf(msg, sizeof msg, "%s:%d: bad line ignored", path, lineno);
            warn(msg);
        }
    }
    fclose(f);
    return 1;
}
