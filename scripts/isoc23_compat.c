/*
 * isoc23_compat.c
 *
 * glibc 2.38+ makes _GNU_SOURCE imply _ISOC2X_SOURCE, which redirects
 * strtol, sscanf, and related functions to __isoc23_* variants.  These
 * symbols do not exist in older glibc (UE's Linux sysroot targets glibc
 * 2.17 / CentOS 7), causing undefined-symbol link errors.
 *
 * This file provides thin wrappers so the linker is satisfied.
 * It MUST be compiled without _GNU_SOURCE (and without any feature-test
 * macro that would re-enable the C23 redirects), otherwise the wrappers
 * would call themselves recursively.
 *
 * Built and injected into libx264.a and libavcodec.a by
 * scripts/build_thirdparty.sh.
 */

/* No _GNU_SOURCE, _POSIX_SOURCE, or _ISOC2X_SOURCE — intentional. */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

long int __isoc23_strtol(const char *nptr, char **endptr, int base)
    { return strtol(nptr, endptr, base); }

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base)
    { return strtoul(nptr, endptr, base); }

long long int __isoc23_strtoll(const char *nptr, char **endptr, int base)
    { return strtoll(nptr, endptr, base); }

unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base)
    { return strtoull(nptr, endptr, base); }

float __isoc23_strtof(const char *nptr, char **endptr)
    { return strtof(nptr, endptr); }

double __isoc23_strtod(const char *nptr, char **endptr)
    { return strtod(nptr, endptr); }

long double __isoc23_strtold(const char *nptr, char **endptr)
    { return strtold(nptr, endptr); }

int __isoc23_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsscanf(str, fmt, ap);
    va_end(ap);
    return ret;
}

int __isoc23_vsscanf(const char *str, const char *fmt, va_list ap)
    { return vsscanf(str, fmt, ap); }

int __isoc23_scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vscanf(fmt, ap);
    va_end(ap);
    return ret;
}

int __isoc23_vscanf(const char *fmt, va_list ap)
    { return vscanf(fmt, ap); }

int __isoc23_fscanf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vfscanf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int __isoc23_vfscanf(FILE *stream, const char *fmt, va_list ap)
    { return vfscanf(stream, fmt, ap); }
