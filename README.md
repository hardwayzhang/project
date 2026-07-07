# UDP Traceroute Demo (Linux)

A minimal, self-contained example of how `traceroute` works on Linux using
**UDP probe packets**, with both a **client** (the traceroute tool) and a
companion **server**.

## How it works

Traceroute discovers the routers on the path to a destination by exploiting the
IP **TTL** (time-to-live) field:

1. The client sends a UDP probe toward the destination with `TTL = 1`.
2. The first router decrements the TTL to `0`, discards the packet, and sends
   back an ICMP **Time Exceeded** (type 11) message. Its source address is that
   router — hop #1.
3. The client repeats with `TTL = 2, 3, ...`, revealing each successive hop.
4. The trace ends when a probe reaches the destination. This demo detects that
   in two complementary ways:
   - the companion **server** replies to the probe over UDP, or
   - a generic host returns an ICMP **Destination Unreachable / Port
     Unreachable** (type 3, code 3) message.

There are **two client implementations** that differ only in how they read the
ICMP replies:

- `client.c` — reads ICMP on a **raw ICMP socket**; requires **root** (or
  `CAP_NET_RAW`).
- `client_recverr.c` — **needs no privileges**. It uses the Linux `IP_RECVERR`
  socket option on a plain UDP socket, so the kernel delivers the relevant ICMP
  errors to the socket's *error queue*, read with `recvmsg(..., MSG_ERRQUEUE)`.

Pick whichever fits your environment; both produce the same hop-by-hop output.

```
client  --UDP(TTL=n)-->  [ router1 ] [ router2 ] ... [ server ]
   ^                          |           |               |
   |   ICMP time-exceeded  <--+           |               |
   |   ICMP time-exceeded  <--------------+               |
   |   UDP reply           <------------------------------+
```

## Files

| File                | Description                                                    |
|---------------------|----------------------------------------------------------------|
| `client.c`          | UDP traceroute client using a raw ICMP socket (**needs root**) |
| `client_recverr.c`  | UDP traceroute client using `IP_RECVERR` (**no root needed**)  |
| `server.c`          | UDP server that echoes a reply so the last hop is known        |
| `common.h`          | Shared probe format and helpers                                |
| `Makefile`          | Build rules                                                    |

## Build

```sh
make
```

Produces three binaries: `server`, `client`, and `client_recverr`.

## Run

### Option A — no root required (recommended): `client_recverr`

```sh
./client_recverr 8.8.8.8
```

Uses `IP_RECVERR`, so it runs as an ordinary user.

### Option B — raw ICMP socket: `client` (needs root)

```sh
sudo ./client 8.8.8.8
```

### With the companion server

Start the server (no privileges required):

```sh
./server 33434
```

Run either client against it; the final hop is confirmed by the server's UDP
reply:

```sh
./client_recverr -p 33434 <server-ip>     # no root
sudo ./client     -p 33434 <server-ip>     # root
```

Against ordinary hosts (no server) the final hop is instead detected via the
ICMP port-unreachable message.

## Options

Both clients share the same options:

```
Usage: client_recverr [-p port] [-m max_hops] [-q nqueries] [-w timeout] host
  -p port      destination UDP port (default 33434)
  -m max_hops  maximum number of hops (default 30)
  -q nqueries  probes per hop (default 3)
  -w timeout   per-probe reply timeout in seconds (default 3)
```

## Notes

- **`client.c` (raw socket):** the raw ICMP socket receives *all* ICMP traffic
  on the host, so the client matches replies to its own probes by inspecting
  the quoted IP + UDP header carried inside each ICMP error and comparing the
  destination address and the source/destination UDP ports.
- **`client_recverr.c` (`IP_RECVERR`):** the kernel only queues errors for
  datagrams this socket sent, so no manual matching is needed and no elevated
  privileges are required. The offending router's address comes from
  `SO_EE_OFFENDER(ee)` in the `IP_RECVERR` control message.
- IPv4 only, to keep the example short.
