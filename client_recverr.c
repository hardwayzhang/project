/*
 * client_recverr.c - UDP traceroute client that needs NO root privileges.
 *
 * The companion client.c reads ICMP "time exceeded" replies from a raw ICMP
 * socket, which requires root (or CAP_NET_RAW). This variant achieves the same
 * result on Linux using only an ordinary UDP (SOCK_DGRAM) socket:
 *
 *   - Enable the IP_RECVERR socket option. The kernel then tracks ICMP errors
 *     related to datagrams sent on this socket and queues them on the socket's
 *     *error queue* instead of silently dropping them.
 *   - Send UDP probes with an increasing IP TTL, exactly like classic
 *     traceroute.
 *   - When a router replies with ICMP "time exceeded", read the error from the
 *     error queue with recvmsg(..., MSG_ERRQUEUE). The IP_RECVERR control
 *     message carries a `struct sock_extended_err`; SO_EE_OFFENDER(ee) gives
 *     the address of the router that generated the error -> that hop.
 *   - The trace ends when the destination is reached, detected two ways:
 *       * the companion server sends a normal UDP reply (POLLIN), or
 *       * a generic host returns ICMP port-unreachable, surfaced on the error
 *         queue as ee_type == ICMP_DEST_UNREACH.
 *
 * Because IP_RECVERR only surfaces errors for datagrams this socket sent, no
 * manual packet matching is required, and no elevated privileges are needed.
 *
 * Usage:
 *   ./client_recverr [-p port] [-m max_hops] [-q nqueries] [-w timeout] host
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/errqueue.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"

struct config {
    const char *host;
    int port;
    int max_hops;
    int nqueries;
    double timeout;   /* per-probe reply timeout in seconds */
};

enum reply_kind {
    REPLY_NONE = 0,   /* timed out */
    REPLY_HOP,        /* ICMP time-exceeded from an intermediate router */
    REPLY_DEST_ICMP,  /* ICMP port-unreachable from the destination host */
    REPLY_DEST_UDP    /* UDP reply from our companion server */
};

struct reply {
    enum reply_kind kind;
    struct in_addr from;
    double rtt_ms;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-m max_hops] [-q nqueries] [-w timeout] host\n"
            "  -p port      destination UDP port (default %d)\n"
            "  -m max_hops  maximum number of hops (default 30)\n"
            "  -q nqueries  probes per hop (default 3)\n"
            "  -w timeout   per-probe reply timeout in seconds (default 3)\n"
            "\nNo root privileges required (uses IP_RECVERR error queue).\n",
            prog, TR_DEFAULT_PORT);
}

/* Drain and interpret one message from the socket error queue. */
static int read_errqueue(int fd, struct reply *out)
{
    uint8_t data[1500];
    uint8_t control[512];
    struct iovec iov = { data, sizeof(data) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(fd, &msg, MSG_ERRQUEUE);
    if (n < 0)
        return 0;

    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL;
         cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != IPPROTO_IP || cm->cmsg_type != IP_RECVERR)
            continue;

        struct sock_extended_err *ee =
            (struct sock_extended_err *)CMSG_DATA(cm);
        if (ee->ee_origin != SO_EE_ORIGIN_ICMP)
            continue;

        struct sockaddr_in *off = (struct sockaddr_in *)SO_EE_OFFENDER(ee);
        out->from = off->sin_addr;

        if (ee->ee_type == ICMP_TIME_EXCEEDED)
            out->kind = REPLY_HOP;
        else if (ee->ee_type == ICMP_DEST_UNREACH)
            out->kind = REPLY_DEST_ICMP;
        else
            out->kind = REPLY_HOP; /* treat other ICMP as an intermediate hit */
        return 1;
    }
    return 0;
}

static struct reply send_probe(const struct config *cfg, int fd,
                               struct in_addr dst, uint32_t seq, int ttl)
{
    struct reply r;
    memset(&r, 0, sizeof(r));
    r.kind = REPLY_NONE;

    if (setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
        perror("setsockopt IP_TTL");
        return r;
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr = dst;
    to.sin_port = htons((uint16_t)cfg->port);

    struct tr_probe probe;
    probe.magic = htonl(TR_MAGIC);
    probe.ttl = htonl((uint32_t)ttl);
    probe.seq = htonl(seq);

    struct timeval sent;
    gettimeofday(&sent, NULL);

    if (sendto(fd, &probe, sizeof(probe), 0,
               (struct sockaddr *)&to, sizeof(to)) < 0) {
        /* A queued ICMP error can make sendto fail with e.g. ECONNREFUSED;
         * that itself is meaningful, so fall through to drain the queue. */
        if (errno != ECONNREFUSED && errno != EHOSTUNREACH &&
            errno != ENETUNREACH) {
            perror("sendto");
            return r;
        }
    }

    for (;;) {
        struct timeval now;
        gettimeofday(&now, NULL);
        double left = cfg->timeout - tr_ms_diff(&sent, &now) / 1000.0;
        if (left <= 0)
            return r;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN | POLLERR;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, (int)(left * 1000.0));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return r;
        }
        if (rc == 0)
            return r; /* timed out */

        struct timeval rcv;
        gettimeofday(&rcv, NULL);

        /* Error queue: ICMP time-exceeded / unreachable. */
        if (pfd.revents & POLLERR) {
            if (read_errqueue(fd, &r)) {
                r.rtt_ms = tr_ms_diff(&sent, &rcv);
                return r;
            }
        }

        /* Normal inbound data: the companion server's UDP reply. */
        if (pfd.revents & POLLIN) {
            uint8_t buf[1500];
            struct sockaddr_in src;
            socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&src, &srclen);
            if (n >= (ssize_t)sizeof(struct tr_probe)) {
                struct tr_probe p;
                memcpy(&p, buf, sizeof(p));
                if (ntohl(p.magic) == TR_MAGIC) {
                    r.kind = REPLY_DEST_UDP;
                    r.from = src.sin_addr;
                    r.rtt_ms = tr_ms_diff(&sent, &rcv);
                    return r;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    struct config cfg;
    cfg.host = NULL;
    cfg.port = TR_DEFAULT_PORT;
    cfg.max_hops = 30;
    cfg.nqueries = 3;
    cfg.timeout = 3.0;

    int opt;
    while ((opt = getopt(argc, argv, "p:m:q:w:h")) != -1) {
        switch (opt) {
        case 'p': cfg.port = atoi(optarg); break;
        case 'm': cfg.max_hops = atoi(optarg); break;
        case 'q': cfg.nqueries = atoi(optarg); break;
        case 'w': cfg.timeout = atof(optarg); break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }
    cfg.host = argv[optind];

    if (cfg.port <= 0 || cfg.port > 65535 || cfg.max_hops <= 0 ||
        cfg.nqueries <= 0 || cfg.timeout <= 0) {
        fprintf(stderr, "invalid argument value\n");
        return 1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int gai = getaddrinfo(cfg.host, NULL, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "cannot resolve %s: %s\n", cfg.host, gai_strerror(gai));
        return 1;
    }
    struct in_addr dst = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    char dstip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst, dstip, sizeof(dstip));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(SOCK_DGRAM)");
        return 1;
    }

    /* This is the key that removes the need for a raw socket / root: ask the
     * kernel to deliver ICMP errors for this socket to its error queue. */
    int on = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_RECVERR, &on, sizeof(on)) < 0) {
        perror("setsockopt IP_RECVERR");
        close(fd);
        return 1;
    }

    printf("udp-traceroute (no-root, IP_RECVERR) to %s (%s), "
           "%d hops max, dest port %d\n",
           cfg.host, dstip, cfg.max_hops, cfg.port);
    fflush(stdout);

    uint32_t seq = 0;
    int reached = 0;

    for (int ttl = 1; ttl <= cfg.max_hops && !reached; ttl++) {
        printf("%2d ", ttl);

        struct in_addr last_from;
        int have_last = 0;

        for (int q = 0; q < cfg.nqueries; q++) {
            struct reply r = send_probe(&cfg, fd, dst, ++seq, ttl);

            if (r.kind == REPLY_NONE) {
                printf(" *");
                fflush(stdout);
                continue;
            }

            if (!have_last || last_from.s_addr != r.from.s_addr) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &r.from, ip, sizeof(ip));
                printf(" %s", ip);
                last_from = r.from;
                have_last = 1;
            }
            printf("  %.3f ms", r.rtt_ms);
            fflush(stdout);

            if (r.kind == REPLY_DEST_UDP || r.kind == REPLY_DEST_ICMP)
                reached = 1;
        }

        printf("\n");
        fflush(stdout);
    }

    if (reached)
        printf("reached destination %s\n", dstip);
    else
        printf("did not reach %s within %d hops\n", dstip, cfg.max_hops);

    close(fd);
    return 0;
}
