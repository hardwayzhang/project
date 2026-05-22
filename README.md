# crc-hwaccel-test (x86)

A small Linux test program that detects, functionally verifies and (optionally)
benchmarks **CRC hardware acceleration** on **x86 / x86_64**.

The CRC acceleration code is split into a tiny library with a stable C API
(`crc_hw.h` / `crc_hw.c`) plus a CLI driver (`crc_hwaccel_test.c`).

## Public API

```c
#include "crc_hw.h"

bool     crc_hw_supported(void);                                 // is SSE4.2 CRC32 available?
uint32_t crc32c_hw(uint32_t crc, const void *buf, size_t len);   // requires crc_hw_supported() == true
uint32_t crc32c_sw(uint32_t crc, const void *buf, size_t len);   // portable fallback / reference
```

- `crc_hw_supported()` queries CPUID once and caches the result.
- `crc32c_hw()` is the *only* function in the project that uses SSE4.2 codegen,
  via `__attribute__((target("sse4.2")))`.

Typical usage:

```c
static uint32_t (*crc32c)(uint32_t, const void *, size_t);

void crc32c_init(void) {
    crc32c = crc_hw_supported() ? crc32c_hw : crc32c_sw;
}
```

## Build philosophy: no global ISA flags

The Makefile compiles every translation unit at the **baseline x86 ISA** — no
`-msse4.2`, no `-mpclmul`, no `-mavx`. Only `crc32c_hw` opts in to SSE4.2 via
the function-level `target` attribute, so:

- The resulting binary still runs on older x86 CPUs without SSE4.2.
- Other functions cannot accidentally use SSE4.2 intrinsics.
- Build flags stay clean and don't get "polluted" with instruction-set switches.

You can verify this with `objdump -d`: the `crc32` opcode appears only inside
`<crc32c_hw>` and nowhere else.

## Build

```
make            # build with the default compiler (cc)
CC=gcc make     # or pin a specific compiler
```

Compilation is tested with both **gcc 13** and **clang 18**. The Makefile
errors out if invoked on a non-x86 host.

## Run

```
./crc_hwaccel_test           # detection + self-test
./crc_hwaccel_test --bench   # also throughput benchmark (sw vs hw)
./crc_hwaccel_test --help
```

### Example output (x86_64 with SSE4.2)

```
Platform: x86_64
crc_hw_supported() -> true (SSE4.2 CRC32 available)

Self-test (CRC32C of "123456789", expected=0xe3069283):
  software : 0xe3069283   PASS
  hardware : 0xe3069283   PASS

Benchmark (buffer=1048576 B, iterations=256, total=256 MiB)
  software CRC32C       :   463.87 MiB/s
  x86 SSE4.2 CRC32C     :  9725.52 MiB/s
  speedup (hw / sw)     : 20.97x

Result: CRC hardware acceleration supported and verified.
```

## Exit codes

| Code | Meaning                                                           |
|------|-------------------------------------------------------------------|
| 0    | HW CRC32C supported **and** self-test passed.                     |
| 1    | HW CRC32C not supported on this CPU.                              |
| 2    | HW advertised but self-test FAILED (or sw reference is broken).   |
| 3    | Invalid command-line arguments.                                   |
