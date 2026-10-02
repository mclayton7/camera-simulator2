/* Copyright CamSim Contributors. All Rights Reserved.
 *
 * Portable monotonic clock and non-blocking UDP sender (Linux, macOS, Windows).
 * No XPLM calls here.
 */
#ifndef CAMSIM_TRUTH_PLATFORM_H
#define CAMSIM_TRUTH_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic clock in ns: CLOCK_MONOTONIC (Linux), mach_absolute_time (macOS),
 * QueryPerformanceCounter (Windows). */
uint64_t cst_mono_ns(void);

/* Process-wide socket library init/cleanup (WSAStartup on Windows, no-op elsewhere). */
int cst_net_init(void);
void cst_net_cleanup(void);

typedef struct cst_sender {
    intptr_t sock;          /* -1 when closed (SOCKET on Windows) */
    unsigned char addr[128]; /* struct sockaddr_storage */
    int addr_len;
} cst_sender;

/* Resolves host (IPv4 or IPv6 literal, or a name; resolution may block once,
 * at enable) and opens a non-blocking UDP socket. Returns 0 on success, else
 * writes a message into err. */
int cst_sender_open(cst_sender *s, const char *host, int port, char *err, size_t err_len);

/* Non-blocking sendto. Returns 0 on success, else the OS error code
 * (errno / WSAGetLastError()); a full socket buffer is an error, the datagram is dropped. */
int cst_sender_send(cst_sender *s, const void *data, size_t len);

void cst_sender_close(cst_sender *s);

#endif
