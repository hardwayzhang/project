/*
 * client.c - UDP traceroute demo client.
 *
 * Classic traceroute technique:
 *   1. Send a UDP probe toward the destination with the IP TTL set to 1.
 *   2. The first router decrements the TTL to 0, drops the packet and returns
 *      an ICMP "time exceeded" (type 11) message whose source address is that
 *      router. We capture it on a raw ICMP socket -> that is hop #1.
 *   3. Repeat with TTL 2, 3, ... to reveal each successive hop.
 *   4. The trace ends when the probe reaches the destination. We detect that
 *      in two ways:
 *        - the companion server sends a UDP reply back to us, or
 *        - a generic host returns an ICMP "destination unreachable /
 *          port unreachable" (type 3, code 3) message.
 *
 * The raw ICMP socket requires root (or CAP_NET_RAW), e.g. run with sudo.
 *
 * Usage:
 *   sudo ./client [-p port] [-m max_hops] [-q nqueries] [-w timeout_sec] host
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

/* Result of waiting for a single probe's reply. */
enum reply_kind {
    REPLY_NONE = 0,   /* timed out */
    REPLY_HOP,        /* ICMP time-exceeded from an intermediate router */
    REPLY_DEST_ICMP,  /* ICMP port-unreachable from the destination host */
    REPLY_DEST_UDP    /* UDP reply from our companion server */
};

struct reply {
    enum reply_kind kind;
    struct in_addr from;   /* address that produced the reply */
    double rtt_ms;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-m max_hops] [-q nqueries] [-w timeout] host\n"
            "  -p port      destination UDP port (default %d)\n"
            "  -m max_hops  maximum number of hops (default 30)\n"
            "  -q nqueries  probes per hop (default 3)\n"
            "  -w timeout   per-probe reply timeout in seconds (default 3)\n",
            prog, TR_DEFAULT_PORT);
}

/*
 * Inspect an ICMP datagram received on the raw socket and decide whether it is
 * a reply to one of *our* probes toward `dst` on `dport`, sent from local port
 * `sport`. On a match, fill `out_kind`. Returns 1 on a match, 0 otherwise.
 *
 * ICMP time-exceeded / dest-unreachable messages embed the IP header of the
 * offending packet plus (at least) its first 8 bytes, i.e. the full UDP
 * header. We use that quoted UDP header to confirm the packet is ours.
 */
static int icmp_matches_probe(const uint8_t *pkt, ssize_t len,
                              struct in_addr dst, uint16_t sport,
                              uint16_t dport, enum reply_kind *out_kind)
{
    if (len < (ssize_t)sizeof(struct iphdr))
        return 0;

    const struct iphdr *oip = (const struct iphdr *)pkt;
    size_t ohl = (size_t)oip->ihl * 4;
    if (len < (ssize_t)(ohl + sizeof(struct icmphdr)))
        return 0;

    const struct icmphdr *icmp = (const struct icmphdr *)(pkt + ohl);

    if (icmp->type != ICMP_TIME_EXCEEDED && icmp->type != ICMP_DEST_UNREACH)
        return 0;

    /* Quoted original datagram starts right after the 8-byte ICMP header. */
    const uint8_t *quote = (const uint8_t *)icmp + sizeof(struct icmphdr);
    ssize_t remaining = len - (ssize_t)(ohl + sizeof(struct icmphdr));
    if (remaining < (ssize_t)sizeof(struct iphdr))
        return 0;

    const struct iphdr *iip = (const struct iphdr *)quote;
    size_t ihl = (size_t)iip->ihl * 4;
    if (remaining < (ssize_t)(ihl + sizeof(struct udphdr)))
        return 0;
    if (iip->protocol != IPPROTO_UDP)
        return 0;

    /* Must be one of our probes: right destination and right ports. */
    if (iip->daddr != dst.s_addr)
        return 0;

    const struct udphdr *iudp = (const struct udphdr *)(quote + ihl);
    if (iudp->source != htons(sport) || iudp->dest != htons(dport))
        return 0;

    if (icmp->type == ICMP_TIME_EXCEEDED)
        *out_kind = REPLY_HOP;
    else
        *out_kind = REPLY_DEST_ICMP;
    return 1;
}

/*
 * Send one probe and wait up to cfg->timeout for a reply. `udp_fd` is the UDP
 * send/receive socket (bound to a known local port `sport`), `icmp_fd` the raw
 * ICMP socket.
 */
static struct reply send_probe(const struct config *cfg, int udp_fd, int icmp_fd,
                               struct in_addr dst, uint16_t sport, uint32_t seq,
                               int ttl)
{
    struct reply r;
    memset(&r, 0, sizeof(r));
    r.kind = REPLY_NONE;

    if (setsockopt(udp_fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
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

    if (sendto(udp_fd, &probe, sizeof(probe), 0,
               (struct sockaddr *)&to, sizeof(to)) < 0) {
        perror("sendto");
        return r;
    }

    /* Wait for a reply on either the ICMP raw socket or the UDP socket. */
    for (;;) {
        struct timeval now;
        gettimeofday(&now, NULL);
        double elapsed = tr_ms_diff(&sent, &now) / 1000.0;
        double left = cfg->timeout - elapsed;
        if (left <= 0)
            return r; /* REPLY_NONE */

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);
        FD_SET(icmp_fd, &rfds);
        int maxfd = udp_fd > icmp_fd ? udp_fd : icmp_fd;

        struct timeval tv;
        tv.tv_sec = (time_t)left;
        tv.tv_usec = (suseconds_t)((left - (double)tv.tv_sec) * 1e6);

        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return r;
        }
        if (rc == 0)
            return r; /* timed out */

        struct timeval rcv;
        gettimeofday(&rcv, NULL);

        if (FD_ISSET(icmp_fd, &rfds)) {
            uint8_t buf[1500];
            struct sockaddr_in src;
            socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(icmp_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&src, &srclen);
            if (n > 0) {
                enum reply_kind kind;
                if (icmp_matches_probe(buf, n, dst, sport,
                                       (uint16_t)cfg->port, &kind)) {
                    r.kind = kind;
                    r.from = src.sin_addr;
                    r.rtt_ms = tr_ms_diff(&sent, &rcv);
                    return r;
                }
                /* Not ours (unrelated ICMP): keep waiting. */
            }
        }

        if (FD_ISSET(udp_fd, &rfds)) {
            uint8_t buf[1500];
            struct sockaddr_in src;
            socklen_t srclen = sizeof(src);
            ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
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
            /* Unrelated UDP data: keep waiting. */
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

    /* Resolve the destination host name / dotted address. */
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

    /* Raw ICMP socket to receive time-exceeded / unreachable messages. */
    int icmp_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (icmp_fd < 0) {
        perror("socket(SOCK_RAW, IPPROTO_ICMP)");
        fprintf(stderr, "hint: raw sockets need root; try running with sudo\n");
        return 1;
    }

    /* UDP socket used to send probes and to receive the server's reply. */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket(SOCK_DGRAM)");
        close(icmp_fd);
        return 1;
    }

    /* Bind to a fixed ephemeral local port so we can match the source port in
     * the ICMP-quoted UDP header, and so the server replies come back here. */
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind udp");
        close(icmp_fd);
        close(udp_fd);
        return 1;
    }
    socklen_t locallen = sizeof(local);
    getsockname(udp_fd, (struct sockaddr *)&local, &locallen);
    uint16_t sport = ntohs(local.sin_port);

    printf("udp-traceroute to %s (%s), %d hops max, dest port %d, src port %u\n",
           cfg.host, dstip, cfg.max_hops, cfg.port, sport);
    fflush(stdout);

    uint32_t seq = 0;
    int reached = 0;

    for (int ttl = 1; ttl <= cfg.max_hops && !reached; ttl++) {
        printf("%2d ", ttl);

        struct in_addr last_from;
        int have_last = 0;

        for (int q = 0; q < cfg.nqueries; q++) {
            struct reply r = send_probe(&cfg, udp_fd, icmp_fd, dst, sport,
                                        ++seq, ttl);

            if (r.kind == REPLY_NONE) {
                printf(" *");
                fflush(stdout);
                continue;
            }

            /* Print the responder address once per distinct address. */
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

    close(icmp_fd);
    close(udp_fd);
    return 0;
}
