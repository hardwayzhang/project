/*
 * crc_hwaccel_test.c - x86-only test program for the crc_hw module.
 *
 * Exercises the public API in crc_hw.h:
 *   - crc_hw_supported() - is SSE4.2 CRC32 available on this CPU?
 *   - crc32c_hw()        - hardware CRC32C (SSE4.2)
 *   - crc32c_sw()        - portable software CRC32C reference
 *
 * Usage:
 *   ./crc_hwaccel_test           # detection + self-test
 *   ./crc_hwaccel_test --bench   # also run throughput benchmark
 *   ./crc_hwaccel_test --help
 *
 * Exit codes:
 *   0 - hardware CRC32C supported AND self-test passed
 *   1 - hardware CRC32C not supported on this CPU
 *   2 - hardware CRC32C reported as supported but self-test FAILED
 *   3 - invalid arguments
 */

#define _POSIX_C_SOURCE 200809L

#include "crc_hw.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_use_color = 1;
#define C_RESET  (g_use_color ? "\033[0m"  : "")
#define C_BOLD   (g_use_color ? "\033[1m"  : "")
#define C_RED    (g_use_color ? "\033[31m" : "")
#define C_GREEN  (g_use_color ? "\033[32m" : "")
#define C_YELLOW (g_use_color ? "\033[33m" : "")
#define C_CYAN   (g_use_color ? "\033[36m" : "")

static const char *arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86 (i386)";
#else
    return "unknown";
#endif
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef uint32_t (*crc_fn)(uint32_t, const void *, size_t);

static double bench_one(crc_fn fn, const uint8_t *buf, size_t buf_len,
                        size_t iters, uint32_t *out_sum) {
    volatile uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = fn(v, buf, buf_len);  /* warm-up */

    double t0 = now_seconds();
    uint32_t acc = 0;
    for (size_t i = 0; i < iters; i++) acc = fn(acc, buf, buf_len);
    double t1 = now_seconds();
    if (out_sum) *out_sum = acc;
    return t1 - t0;
}

static void run_benchmark(bool hw_ok) {
    const size_t buf_len = 1u << 20;
    const size_t iters   = 256;

    uint8_t *buf = (uint8_t *)malloc(buf_len);
    if (!buf) {
        fprintf(stderr, "[bench] malloc(%zu) failed: %s\n",
                buf_len, strerror(errno));
        return;
    }
    for (size_t i = 0; i < buf_len; i++) buf[i] = (uint8_t)(i * 1315423911u);

    printf("\n%sBenchmark%s (buffer=%zu B, iterations=%zu, total=%.0f MiB)\n",
           C_BOLD, C_RESET, buf_len, iters,
           (double)(buf_len * iters) / (1024.0 * 1024.0));

    uint32_t sw_sum = 0;
    double t_sw = bench_one(crc32c_sw, buf, buf_len, iters, &sw_sum);
    double mib  = (double)(buf_len * iters) / (1024.0 * 1024.0);
    double sw_thr = mib / t_sw;
    printf("  software CRC32C       : %8.2f MiB/s   (%.3fs, checksum=0x%08x)\n",
           sw_thr, t_sw, sw_sum);

    if (hw_ok) {
        uint32_t hw_sum = 0;
        double t_hw = bench_one(crc32c_hw, buf, buf_len, iters, &hw_sum);
        double hw_thr = mib / t_hw;
        printf("  x86 SSE4.2 CRC32C     : %8.2f MiB/s   (%.3fs, checksum=0x%08x)\n",
               hw_thr, t_hw, hw_sum);
        printf("  %sspeedup (hw / sw)     : %.2fx%s\n",
               C_CYAN, hw_thr / sw_thr, C_RESET);
    }
    free(buf);
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
    if (g_use_color) {
        const char *t = getenv("TERM");
        if (!t || !strcmp(t, "dumb")) g_use_color = 0;
    }

    printf("%sPlatform%s: %s\n", C_BOLD, C_RESET, arch_name());

    bool hw_ok = crc_hw_supported();
    printf("%scrc_hw_supported()%s -> %s%s%s\n",
           C_BOLD, C_RESET,
           hw_ok ? C_GREEN : C_YELLOW,
           hw_ok ? "true (SSE4.2 CRC32 available)" : "false (no SSE4.2 CRC32)",
           C_RESET);

    /* Functional self-test against the well-known CRC32C vector. */
    const char     *msg      = "123456789";
    const size_t    mlen     = 9;
    const uint32_t  expected = 0xE3069283u;

    uint32_t sw = crc32c_sw(0, msg, mlen);
    bool sw_ok = (sw == expected);

    printf("\n%sSelf-test%s (CRC32C of \"123456789\", expected=0x%08x):\n",
           C_BOLD, C_RESET, expected);
    printf("  software : 0x%08x   %s%s%s\n",
           sw, sw_ok ? C_GREEN : C_RED,
           sw_ok ? "PASS" : "FAIL", C_RESET);

    bool hw_test_ok = true;
    if (hw_ok) {
        uint32_t hw = crc32c_hw(0, msg, mlen);
        hw_test_ok = (hw == expected);
        printf("  hardware : 0x%08x   %s%s%s\n",
               hw, hw_test_ok ? C_GREEN : C_RED,
               hw_test_ok ? "PASS" : "FAIL", C_RESET);
    } else {
        printf("  hardware : skipped (no SSE4.2)\n");
    }

    if (do_bench) {
        run_benchmark(hw_ok);
    }

    printf("\n");
    if (!sw_ok) {
        printf("%sSoftware reference produced an unexpected result; the build is broken.%s\n",
               C_RED, C_RESET);
        return 2;
    }
    if (!hw_ok) {
        printf("Result: %sNO%s CRC hardware acceleration on this CPU.\n",
               C_YELLOW, C_RESET);
        return 1;
    }
    if (!hw_test_ok) {
        printf("Result: hardware CRC reported as supported but self-test FAILED.\n");
        return 2;
    }
    printf("Result: %sCRC hardware acceleration supported and verified%s.\n",
           C_GREEN, C_RESET);
    return 0;
}
