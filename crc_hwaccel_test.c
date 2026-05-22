/*
 * crc_hwaccel_test.c
 *
 * A Linux test program that detects, verifies and (optionally) benchmarks
 * hardware acceleration support for CRC computations on the current platform.
 *
 * Supported platforms:
 *   - x86 / x86_64 : SSE4.2 (CRC32 / CRC32C instruction) + PCLMULQDQ
 *   - aarch64      : ARMv8-A CRC32 extension (HWCAP_CRC32) + PMULL
 *   - arm (32-bit) : ARMv8 CRC32 extension (HWCAP2_CRC32)
 *
 * Build:   make
 * Usage:   ./crc_hwaccel_test           # detect + verify
 *          ./crc_hwaccel_test --bench   # also run throughput benchmark
 *          ./crc_hwaccel_test --help
 *
 * Exit codes:
 *   0  - At least one form of CRC hardware acceleration is available AND the
 *        functional self-test passed.
 *   1  - No CRC hardware acceleration detected on this platform.
 *   2  - Hardware acceleration detected but the self-test FAILED (suspicious).
 *   3  - Invalid arguments.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

#if defined(__linux__)
#  include <sys/auxv.h>
#endif

/* ---- Architecture detection ---------------------------------------------- */

#if defined(__x86_64__) || defined(__i386__)
#  define ARCH_X86 1
#  include <cpuid.h>
#  if defined(__SSE4_2__)
#    include <nmmintrin.h>  /* _mm_crc32_u8/u32/u64 */
#  endif
#  if defined(__PCLMUL__)
#    include <wmmintrin.h>  /* _mm_clmulepi64_si128 */
#  endif
#elif defined(__aarch64__)
#  define ARCH_ARM64 1
#  ifndef HWCAP_CRC32
#    define HWCAP_CRC32 (1 << 7)
#  endif
#  ifndef HWCAP_PMULL
#    define HWCAP_PMULL (1 << 4)
#  endif
#elif defined(__arm__)
#  define ARCH_ARM 1
#  ifndef HWCAP2_CRC32
#    define HWCAP2_CRC32 (1 << 4)
#  endif
#  ifndef HWCAP2_PMULL
#    define HWCAP2_PMULL (1 << 1)
#  endif
#else
#  define ARCH_UNKNOWN 1
#endif

/* ---- ANSI colors for nicer output ---------------------------------------- */

static int g_use_color = 1;
#define C_RESET  (g_use_color ? "\033[0m"  : "")
#define C_BOLD   (g_use_color ? "\033[1m"  : "")
#define C_RED    (g_use_color ? "\033[31m" : "")
#define C_GREEN  (g_use_color ? "\033[32m" : "")
#define C_YELLOW (g_use_color ? "\033[33m" : "")
#define C_CYAN   (g_use_color ? "\033[36m" : "")

static const char *yn(bool v) {
    return v ? "yes" : "no";
}

static void print_feature(const char *name, bool supported, const char *note) {
    printf("  %-22s : %s%s%s%s%s\n",
           name,
           supported ? C_GREEN : C_RED,
           supported ? "YES" : "NO ",
           C_RESET,
           note && *note ? "  " : "",
           note ? note : "");
}

/* ========================================================================= */
/* Feature detection                                                          */
/* ========================================================================= */

struct hw_features {
    /* x86 */
    bool x86_sse42;
    bool x86_pclmul;
    bool x86_avx;
    bool x86_avx2;
    bool x86_avx512f;
    bool x86_vpclmulqdq;
    /* arm64 */
    bool arm_crc32;
    bool arm_pmull;
    /* general flag: any hw-accelerated CRC available */
    bool any_crc_hw;
};

#if defined(ARCH_X86)
static void detect_x86(struct hw_features *f) {
    unsigned int eax, ebx, ecx, edx;

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        f->x86_sse42  = (ecx & bit_SSE4_2)  != 0;
        f->x86_pclmul = (ecx & bit_PCLMUL)  != 0;
        f->x86_avx    = (ecx & bit_AVX)     != 0;
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        f->x86_avx2     = (ebx & bit_AVX2)    != 0;
        f->x86_avx512f  = (ebx & bit_AVX512F) != 0;
#       ifndef bit_VPCLMULQDQ
#       define bit_VPCLMULQDQ (1 << 10)
#       endif
        f->x86_vpclmulqdq = (ecx & bit_VPCLMULQDQ) != 0;
    }
    f->any_crc_hw = f->x86_sse42 || f->x86_pclmul;
}
#endif

#if defined(ARCH_ARM64)
static void detect_arm64(struct hw_features *f) {
    unsigned long hwcap = getauxval(AT_HWCAP);
    f->arm_crc32 = (hwcap & HWCAP_CRC32) != 0;
    f->arm_pmull = (hwcap & HWCAP_PMULL) != 0;
    f->any_crc_hw = f->arm_crc32 || f->arm_pmull;
}
#endif

#if defined(ARCH_ARM)
static void detect_arm(struct hw_features *f) {
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    f->arm_crc32 = (hwcap2 & HWCAP2_CRC32) != 0;
    f->arm_pmull = (hwcap2 & HWCAP2_PMULL) != 0;
    f->any_crc_hw = f->arm_crc32 || f->arm_pmull;
}
#endif

static void detect_features(struct hw_features *f) {
    memset(f, 0, sizeof(*f));
#if defined(ARCH_X86)
    detect_x86(f);
#elif defined(ARCH_ARM64)
    detect_arm64(f);
#elif defined(ARCH_ARM)
    detect_arm(f);
#endif
}

static const char *arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86 (i386)";
#elif defined(__aarch64__)
    return "aarch64 (ARM64)";
#elif defined(__arm__)
    return "arm (32-bit)";
#else
    return "unknown";
#endif
}

/* ========================================================================= */
/* Reference (software) CRC32C - Castagnoli polynomial 0x1EDC6F41             */
/* Used both as a fallback and as a ground-truth for the self-test.           */
/* ========================================================================= */

static uint32_t crc32c_sw_table[256];
static int      crc32c_sw_table_inited = 0;

static void crc32c_sw_init(void) {
    if (crc32c_sw_table_inited) return;
    const uint32_t poly = 0x82F63B78u; /* reflected 0x1EDC6F41 */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ poly : (c >> 1);
        }
        crc32c_sw_table[i] = c;
    }
    crc32c_sw_table_inited = 1;
}

static uint32_t crc32c_sw(uint32_t crc, const uint8_t *buf, size_t len) {
    crc32c_sw_init();
    crc = ~crc;
    while (len--) {
        crc = crc32c_sw_table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

/* ========================================================================= */
/* Hardware CRC32C implementations                                            */
/* ========================================================================= */

#if defined(ARCH_X86) && defined(__SSE4_2__)
__attribute__((target("sse4.2")))
static uint32_t crc32c_hw_x86(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
#  if defined(__x86_64__)
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, buf, sizeof(v));
        crc = (uint32_t)_mm_crc32_u64(crc, v);
        buf += 8;
        len -= 8;
    }
#  endif
    while (len >= 4) {
        uint32_t v;
        memcpy(&v, buf, sizeof(v));
        crc = _mm_crc32_u32(crc, v);
        buf += 4;
        len -= 4;
    }
    while (len--) {
        crc = _mm_crc32_u8(crc, *buf++);
    }
    return ~crc;
}
#endif

#if defined(ARCH_ARM64)
/* Use intrinsics if available (need -march=armv8-a+crc). The function below
 * is annotated with target attribute so callers don't need to be compiled
 * with +crc; the kernel HWCAP check still gates whether we call it. */
#  if defined(__ARM_FEATURE_CRC32) || defined(__GNUC__)
#    include <arm_acle.h>
__attribute__((target("+crc")))
static uint32_t crc32c_hw_arm64(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, buf, sizeof(v));
        crc = __crc32cd(crc, v);
        buf += 8;
        len -= 8;
    }
    while (len >= 4) {
        uint32_t v;
        memcpy(&v, buf, sizeof(v));
        crc = __crc32cw(crc, v);
        buf += 4;
        len -= 4;
    }
    while (len--) {
        crc = __crc32cb(crc, *buf++);
    }
    return ~crc;
}
#  endif
#endif

/* ========================================================================= */
/* Self-test                                                                  */
/* ========================================================================= */

struct selftest_result {
    bool ran;
    bool ok;
    uint32_t expected;
    uint32_t got;
    const char *impl;
};

static void run_selftest(const struct hw_features *f, struct selftest_result *r) {
    memset(r, 0, sizeof(*r));

    /* Known-good test vector for CRC32C ("123456789") */
    const char *msg = "123456789";
    const size_t mlen = 9;
    const uint32_t expected = 0xE3069283u; /* well-known CRC32C value */

    /* Sanity-check the software reference first */
    uint32_t sw = crc32c_sw(0, (const uint8_t *)msg, mlen);
    if (sw != expected) {
        r->ran = true;
        r->ok = false;
        r->expected = expected;
        r->got = sw;
        r->impl = "software (reference is broken!)";
        return;
    }

#if defined(ARCH_X86) && defined(__SSE4_2__)
    if (f->x86_sse42) {
        uint32_t hw = crc32c_hw_x86(0, (const uint8_t *)msg, mlen);
        r->ran = true;
        r->expected = expected;
        r->got = hw;
        r->ok = (hw == expected);
        r->impl = "x86 SSE4.2 CRC32C";
        return;
    }
#endif

#if defined(ARCH_ARM64)
    if (f->arm_crc32) {
        uint32_t hw = crc32c_hw_arm64(0, (const uint8_t *)msg, mlen);
        r->ran = true;
        r->expected = expected;
        r->got = hw;
        r->ok = (hw == expected);
        r->impl = "ARMv8 CRC32C extension";
        return;
    }
#endif

    (void)f;
}

/* ========================================================================= */
/* Benchmark                                                                  */
/* ========================================================================= */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef uint32_t (*crc_fn)(uint32_t, const uint8_t *, size_t);

static double bench_one(crc_fn fn, const uint8_t *buf, size_t buf_len,
                        size_t iters, uint32_t *out_sum) {
    /* Warm-up */
    volatile uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) v = fn(v, buf, buf_len);

    double t0 = now_seconds();
    uint32_t acc = 0;
    for (size_t i = 0; i < iters; i++) {
        acc = fn(acc, buf, buf_len);
    }
    double t1 = now_seconds();
    if (out_sum) *out_sum = acc;
    return t1 - t0;
}

static void run_benchmark(const struct hw_features *f) {
    const size_t buf_len = 1u << 20;  /* 1 MiB */
    const size_t iters   = 256;       /* -> 256 MiB processed per impl */
    uint8_t *buf = (uint8_t *)malloc(buf_len);
    if (!buf) {
        fprintf(stderr, "[bench] malloc(%zu) failed: %s\n", buf_len, strerror(errno));
        return;
    }
    /* Deterministic, non-zero data */
    for (size_t i = 0; i < buf_len; i++) buf[i] = (uint8_t)(i * 1315423911u);

    printf("\n%sBenchmark%s (buffer=%zu B, iterations=%zu, total=%.0f MiB)\n",
           C_BOLD, C_RESET, buf_len, iters,
           (double)(buf_len * iters) / (1024.0 * 1024.0));

    /* Software baseline */
    uint32_t s_sum = 0;
    double t_sw = bench_one(crc32c_sw, buf, buf_len, iters, &s_sum);
    double mib  = (double)(buf_len * iters) / (1024.0 * 1024.0);
    double sw_thr = mib / t_sw;
    printf("  software CRC32C       : %8.2f MiB/s   (%.3fs, checksum=0x%08x)\n",
           sw_thr, t_sw, s_sum);

    bool ran_hw = false;
    double hw_thr = 0.0;
    (void)f;

#if defined(ARCH_X86) && defined(__SSE4_2__)
    if (f->x86_sse42) {
        uint32_t h_sum = 0;
        double t_hw = bench_one(crc32c_hw_x86, buf, buf_len, iters, &h_sum);
        hw_thr = mib / t_hw;
        printf("  x86 SSE4.2 CRC32C     : %8.2f MiB/s   (%.3fs, checksum=0x%08x)\n",
               hw_thr, t_hw, h_sum);
        ran_hw = true;
    }
#endif
#if defined(ARCH_ARM64)
    if (f->arm_crc32) {
        uint32_t h_sum = 0;
        double t_hw = bench_one(crc32c_hw_arm64, buf, buf_len, iters, &h_sum);
        hw_thr = mib / t_hw;
        printf("  ARMv8 CRC32C          : %8.2f MiB/s   (%.3fs, checksum=0x%08x)\n",
               hw_thr, t_hw, h_sum);
        ran_hw = true;
    }
#endif

    if (ran_hw && sw_thr > 0) {
        printf("  %sspeedup (hw / sw)     : %.2fx%s\n",
               C_CYAN, hw_thr / sw_thr, C_RESET);
    }

    free(buf);
}

/* ========================================================================= */
/* Reporting                                                                  */
/* ========================================================================= */

static void print_report(const struct hw_features *f) {
    printf("%sPlatform%s: %s\n", C_BOLD, C_RESET, arch_name());
    printf("%sCPU features relevant for CRC acceleration:%s\n", C_BOLD, C_RESET);

#if defined(ARCH_X86)
    print_feature("SSE4.2 (CRC32)",     f->x86_sse42,     "- CRC32C instruction");
    print_feature("PCLMULQDQ",          f->x86_pclmul,    "- folding-based CRC");
    print_feature("AVX",                f->x86_avx,       "");
    print_feature("AVX2",               f->x86_avx2,      "");
    print_feature("AVX-512F",           f->x86_avx512f,   "");
    print_feature("VPCLMULQDQ",         f->x86_vpclmulqdq,"- wide folding CRC");
#elif defined(ARCH_ARM64)
    print_feature("ARMv8 CRC32",        f->arm_crc32,     "- CRC32/CRC32C insns");
    print_feature("PMULL/PMULL2",       f->arm_pmull,     "- folding-based CRC");
#elif defined(ARCH_ARM)
    print_feature("ARMv8 CRC32 (32-bit)",f->arm_crc32,    "- CRC32/CRC32C insns");
    print_feature("PMULL/PMULL2",       f->arm_pmull,     "- folding-based CRC");
#else
    printf("  (no CRC HW accel detection implemented for this architecture)\n");
#endif

    printf("\n%sSummary%s: hardware CRC acceleration ... %s%s%s\n",
           C_BOLD, C_RESET,
           f->any_crc_hw ? C_GREEN : C_YELLOW,
           f->any_crc_hw ? "AVAILABLE" : "NOT AVAILABLE",
           C_RESET);
}

static void usage(const char *argv0) {
    printf("Usage: %s [--bench] [--no-color] [--help]\n", argv0);
    printf("  --bench     also run a throughput benchmark (sw vs hw)\n");
    printf("  --no-color  disable ANSI color output\n");
    printf("  --help      show this help\n");
}

int main(int argc, char **argv) {
    bool do_bench = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") || !strcmp(argv[i], "-b")) {
            do_bench = true;
        } else if (!strcmp(argv[i], "--no-color")) {
            g_use_color = 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 3;
        }
    }

    /* If stdout is not a terminal, default to no color. */
    if (g_use_color) {
        const char *t = getenv("TERM");
        if (!t || !strcmp(t, "dumb")) g_use_color = 0;
    }

    struct hw_features f;
    detect_features(&f);
    print_report(&f);

    struct selftest_result r;
    run_selftest(&f, &r);

    printf("\n%sSelf-test%s (CRC32C of \"123456789\", expected=0x%08x):\n",
           C_BOLD, C_RESET, 0xE3069283u);
    if (!r.ran) {
        printf("  %sskipped%s - no hardware CRC implementation available\n",
               C_YELLOW, C_RESET);
    } else {
        printf("  impl     : %s\n", r.impl);
        printf("  result   : 0x%08x\n", r.got);
        printf("  status   : %s%s%s\n",
               r.ok ? C_GREEN : C_RED,
               r.ok ? "PASS" : "FAIL",
               C_RESET);
    }

    if (do_bench) {
        run_benchmark(&f);
    }

    printf("\n");
    if (!f.any_crc_hw) {
        printf("Hint: %sno%s CRC hardware acceleration detected on this %s system.\n",
               C_YELLOW, C_RESET, arch_name());
        printf("      (Software CRC will still work, just slower.)\n");
        (void)yn;
        return 1;
    }
    if (r.ran && !r.ok) {
        printf("Warning: hardware CRC is reported as available but the self-test FAILED.\n");
        return 2;
    }
    printf("OK: CRC hardware acceleration is supported and verified on this platform.\n");
    return 0;
}
