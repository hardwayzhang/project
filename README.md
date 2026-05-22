# crc-hwaccel-test

A small Linux test program that detects, functionally verifies and (optionally)
benchmarks **CRC hardware acceleration** on the current platform.

## Supported platforms

| Arch         | CRC instructions detected                              |
|--------------|--------------------------------------------------------|
| x86 / x86_64 | SSE4.2 `crc32` (CRC32C), `PCLMULQDQ`, AVX/AVX2/AVX-512, `VPCLMULQDQ` |
| aarch64      | ARMv8-A CRC32 extension (`HWCAP_CRC32`), PMULL         |
| arm (32-bit) | ARMv8 CRC32 extension (`HWCAP2_CRC32`), PMULL          |

Detection is done via:
- **x86**: `CPUID` (function 1 ECX bits for SSE4.2/PCLMUL/AVX, leaf 7 EBX/ECX
  for AVX2/AVX-512F/VPCLMULQDQ).
- **ARM**: `getauxval(AT_HWCAP)` / `AT_HWCAP2` (the same mechanism the kernel
  uses to advertise CPU features to userspace).

The program then **actually executes** the hardware instruction on the well-
known CRC32C test vector `"123456789"` (expected value `0xE3069283`) and
compares it against a portable, table-driven software reference.

## Build

```
make
```

The `Makefile` automatically adds the appropriate `-march`/`-m...` flags for
the host architecture (`-msse4.2 -mpclmul` on x86, `-march=armv8-a+crc+crypto`
on aarch64, etc.).

## Run

```
./crc_hwaccel_test           # detection + self-test
./crc_hwaccel_test --bench   # also throughput benchmark (sw vs hw)
./crc_hwaccel_test --help
```

### Example output (x86_64 with SSE4.2 + PCLMULQDQ + AVX-512)

```
Platform: x86_64
CPU features relevant for CRC acceleration:
  SSE4.2 (CRC32)         : YES  - CRC32C instruction
  PCLMULQDQ              : YES  - folding-based CRC
  AVX                    : YES
  AVX2                   : YES
  AVX-512F               : YES
  VPCLMULQDQ             : YES  - wide folding CRC

Summary: hardware CRC acceleration ... AVAILABLE

Self-test (CRC32C of "123456789", expected=0xe3069283):
  impl     : x86 SSE4.2 CRC32C
  result   : 0xe3069283
  status   : PASS

Benchmark (buffer=1048576 B, iterations=256, total=256 MiB)
  software CRC32C       :   473.27 MiB/s
  x86 SSE4.2 CRC32C     : 10112.34 MiB/s
  speedup (hw / sw)     : 21.37x

OK: CRC hardware acceleration is supported and verified on this platform.
```

## Exit codes

| Code | Meaning                                                                 |
|------|-------------------------------------------------------------------------|
| 0    | Hardware CRC acceleration is supported **and** the self-test passed.   |
| 1    | No CRC hardware acceleration detected on this platform.                |
| 2    | Hardware was advertised but the self-test FAILED (suspicious CPU/OS).  |
| 3    | Invalid command-line arguments.                                        |

These make it convenient to use the program in scripts / CI checks.
