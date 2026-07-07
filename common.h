/*
 * common.h - shared definitions and helpers for the UDP traceroute demo.
 *
 * The demo consists of two programs:
 *   - server.c : a small UDP server that echoes a reply back to any probe it
 *                receives. It lets the traceroute client positively detect that
 *                a probe reached the destination, without relying on the target
 *                host sending back an ICMP "port unreachable" message.
 *   - client.c : a traceroute implementation that sends UDP probes with an
 *                increasing IP TTL and listens on a raw ICMP socket for the
 *                "time exceeded" replies produced by intermediate routers.
 */
#ifndef UDP_TRACEROUTE_COMMON_H
#define UDP_TRACEROUTE_COMMON_H

#include <stdint.h>
#include <sys/time.h>

/* Default UDP port the probes are sent to (matches classic traceroute base). */
#define TR_DEFAULT_PORT 33434

/* Magic marker placed in every probe payload so the server and the client can
 * recognise packets that belong to this demo and ignore unrelated traffic. */
#define TR_MAGIC 0x54524345u /* "TRCE" */

/* Payload carried inside each UDP probe datagram. */
struct tr_probe {
    uint32_t magic;   /* TR_MAGIC, network byte order */
    uint32_t ttl;     /* TTL used for this probe, network byte order */
    uint32_t seq;     /* probe sequence number, network byte order */
};

/* Return the difference (b - a) in milliseconds as a double. */
static inline double tr_ms_diff(const struct timeval *a, const struct timeval *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_usec - a->tv_usec) / 1000.0;
}

#endif /* UDP_TRACEROUTE_COMMON_H */
