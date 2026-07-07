/*
 * server.c - UDP traceroute demo server.
 *
 * Binds a UDP socket to a port and, for every probe datagram it receives,
 * sends a short reply back to the sender. When the traceroute client uses this
 * server as its destination, the reply is what tells the client that the probe
 * finally reached the target (the last hop), instead of depending on the OS of
 * the destination host to emit an ICMP "port unreachable" message.
 *
 * Usage:
 *   ./server [port]
 *
 * The server does not need any special privileges.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

int main(int argc, char **argv)
{
    int port = TR_DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("udp-traceroute server listening on 0.0.0.0:%d\n", port);
    fflush(stdout);

    for (;;) {
        char buf[1500];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

        /* Try to decode the probe payload for a nicer log line. */
        uint32_t seq = 0, ttl = 0;
        int is_probe = 0;
        if ((size_t)n >= sizeof(struct tr_probe)) {
            struct tr_probe p;
            memcpy(&p, buf, sizeof(p));
            if (ntohl(p.magic) == TR_MAGIC) {
                is_probe = 1;
                seq = ntohl(p.seq);
                ttl = ntohl(p.ttl);
            }
        }

        if (is_probe) {
            printf("probe from %s:%d  seq=%u ttl=%u (%zd bytes)\n",
                   ip, ntohs(from.sin_port), seq, ttl, n);
        } else {
            printf("datagram from %s:%d (%zd bytes)\n",
                   ip, ntohs(from.sin_port), n);
        }
        fflush(stdout);

        /* Reply so the client can detect the final hop. Echo the payload back
         * verbatim; it already carries the magic/seq the client expects. */
        if (sendto(fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&from, fromlen) < 0) {
            perror("sendto");
        }
    }

    close(fd);
    return 0;
}
