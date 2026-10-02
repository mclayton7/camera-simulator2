/* Copyright CamSim Contributors. All Rights Reserved. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "cst_platform.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif
#endif

uint64_t cst_mono_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq; /* constant after boot */
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* split to avoid overflowing 64 bits at high counter values */
    uint64_t sec = (uint64_t)(now.QuadPart / freq.QuadPart);
    uint64_t rem = (uint64_t)(now.QuadPart % freq.QuadPart);
    return sec * 1000000000ull + rem * 1000000000ull / (uint64_t)freq.QuadPart;
#elif defined(__APPLE__)
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0)
        mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    if (tb.numer == tb.denom)
        return t; /* Intel: 1/1 */
    /* arm64: 125/3; split to keep the multiply in range */
    return (t / tb.denom) * tb.numer + (t % tb.denom) * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#if defined(_WIN32)
#define CST_INVALID ((intptr_t)INVALID_SOCKET)
static int last_net_error(void) { return WSAGetLastError(); }
#else
#define CST_INVALID ((intptr_t)-1)
static int last_net_error(void) { return errno; }
#endif

int cst_net_init(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

void cst_net_cleanup(void)
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

int cst_sender_open(cst_sender *s, const char *host, int port, char *err, size_t err_len)
{
    struct addrinfo hints, *res = NULL;
    char port_str[16];
    int rc;

    s->sock = CST_INVALID;
    s->addr_len = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(port_str, sizeof port_str, "%d", port);
    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || res == NULL) {
        snprintf(err, err_len, "cannot resolve %s:%d (getaddrinfo %d)", host, port, rc);
        return -1;
    }
    if (res->ai_addrlen > sizeof s->addr) {
        freeaddrinfo(res);
        snprintf(err, err_len, "address too long");
        return -1;
    }
    memcpy(s->addr, res->ai_addr, res->ai_addrlen);
    s->addr_len = (int)res->ai_addrlen;

#if defined(_WIN32)
    {
        SOCKET sk = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        u_long nb = 1;
        freeaddrinfo(res);
        if (sk == INVALID_SOCKET) {
            snprintf(err, err_len, "socket() failed (WSA %d)", WSAGetLastError());
            return -1;
        }
        if (ioctlsocket(sk, FIONBIO, &nb) != 0) {
            snprintf(err, err_len, "ioctlsocket(FIONBIO) failed (WSA %d)", WSAGetLastError());
            closesocket(sk);
            return -1;
        }
        s->sock = (intptr_t)sk;
    }
#else
    {
        int sk = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        int fl;
        freeaddrinfo(res);
        if (sk < 0) {
            snprintf(err, err_len, "socket() failed (%s)", strerror(errno));
            return -1;
        }
        fl = fcntl(sk, F_GETFL, 0);
        if (fl < 0 || fcntl(sk, F_SETFL, fl | O_NONBLOCK) < 0) {
            snprintf(err, err_len, "fcntl(O_NONBLOCK) failed (%s)", strerror(errno));
            close(sk);
            return -1;
        }
        fcntl(sk, F_SETFD, FD_CLOEXEC);
        s->sock = (intptr_t)sk;
    }
#endif
    return 0;
}

int cst_sender_send(cst_sender *s, const void *data, size_t len)
{
    if (s->sock == CST_INVALID)
        return -1;
#if defined(_WIN32)
    if (sendto((SOCKET)s->sock, (const char *)data, (int)len, 0, (const struct sockaddr *)s->addr, s->addr_len) == SOCKET_ERROR)
        return last_net_error();
#else
    if (sendto((int)s->sock, data, len, 0, (const struct sockaddr *)s->addr, (socklen_t)s->addr_len) < 0)
        return last_net_error();
#endif
    return 0;
}

void cst_sender_close(cst_sender *s)
{
    if (s->sock == CST_INVALID)
        return;
#if defined(_WIN32)
    closesocket((SOCKET)s->sock);
#else
    close((int)s->sock);
#endif
    s->sock = CST_INVALID;
}
