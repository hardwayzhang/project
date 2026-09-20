# UDP Port Convergence Proxy (C++ example)

Reference implementation of [`docs/udp-port-convergence-proxy-requirements.md`](../docs/udp-port-convergence-proxy-requirements.md).

## Build

```bash
cmake -S udpproxy -B udpproxy/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build udpproxy/build -j
```

## Run demo

```bash
./udpproxy/build/proxy_demo
```

## Run acceptance tests

```bash
./udpproxy/build/proxy_test
# or
ctest --test-dir udpproxy/build --output-on-failure
```

## Layout

| Path | Role |
|---|---|
| `include/udpproxy/` | Public headers |
| `src/` | Proxy, session, port pool, registry, protocol, room sim |
| `cmd/proxy_demo.cpp` | Same-process demo |
| `tests/proxy_test.cpp` | Acceptance checks A1–A11 |

## Behavior summary

- One public UDP port; control packets start with magic `UPDC`
- `Bind(roomId)` creates a session and allocates exclusive local port `L` on `127.0.0.1`
- Data payloads are forwarded unchanged via loopback NAT
- Unbound data is dropped; idle sessions expire (configurable)
