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

The client listens for the ICMP replies on a **raw ICMP socket**, which is why
it must run as root (or with `CAP_NET_RAW`).

```
client  --UDP(TTL=n)-->  [ router1 ] [ router2 ] ... [ server ]
   ^                          |           |               |
   |   ICMP time-exceeded  <--+           |               |
   |   ICMP time-exceeded  <--------------+               |
   |   UDP reply           <------------------------------+
```

## Files

| File        | Description                                             |
|-------------|---------------------------------------------------------|
| `client.c`  | UDP traceroute client (sends probes, reads ICMP)        |
| `server.c`  | UDP server that echoes a reply so the last hop is known |
| `common.h`  | Shared probe format and helpers                         |
| `Makefile`  | Build rules                                             |

## Build

```sh
make
```

Produces two binaries: `server` and `client`.

## Run

### 1. Client + server on the same/known destination

Start the server (no privileges required):

```sh
./server 33434
```

Run the client against it (raw ICMP socket needs root):

```sh
sudo ./client -p 33434 <server-ip>
```

When the destination is the demo server, the final hop is confirmed by the
server's UDP reply.

### 2. Trace to any host on the Internet

The client also works against ordinary hosts (no server needed); the final hop
is then detected via the ICMP port-unreachable message:

```sh
sudo ./client 8.8.8.8
```

## Options

```
Usage: client [-p port] [-m max_hops] [-q nqueries] [-w timeout] host
  -p port      destination UDP port (default 33434)
  -m max_hops  maximum number of hops (default 30)
  -q nqueries  probes per hop (default 3)
  -w timeout   per-probe reply timeout in seconds (default 3)
```

## Notes

- The raw ICMP socket receives *all* ICMP traffic on the host; the client
  matches replies to its own probes by inspecting the quoted IP + UDP header
  carried inside each ICMP error and comparing the destination address and the
  source/destination UDP ports.
- IPv4 only, to keep the example short.
