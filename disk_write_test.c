/*
 * disk_write_test.c
 *
 * A small benchmark tool that measures disk write throughput on Linux
 * under several different I/O modes:
 *
 *   1. Buffered write       - plain write(), kernel may keep data in page cache.
 *                             Measures "best case" throughput visible to apps.
 *   2. Buffered + fsync     - write() followed by a single fsync() at the end.
 *                             Measures the cost of actually persisting all
 *                             data to the storage device.
 *   3. O_SYNC               - each write() is synchronous (durable on return).
 *                             Measures worst case for many small writes.
 *   4. O_DIRECT             - bypasses the page cache entirely. This is the
 *                             closest userspace approximation of the raw
 *                             device throughput.
 *
 * The test is repeated for several block sizes so we can see how throughput
 * scales with I/O granularity.
 *
 * Usage:
 *   disk_write_test [-f path] [-s total_MiB] [-m modes]
 *
 *     -f  path of the test file (default: ./disk_write_test.dat)
 *     -s  total bytes to write per run, in MiB (default: 512)
 *     -m  comma separated list of modes to run.
 *         valid: buffered,fsync,osync,odirect (default: all)
 *
 * The program creates the test file, writes `size` bytes to it, measures
 * the wall clock time, computes throughput, then removes the file before
 * the next mode runs.  Memory for the data buffer is page aligned so that
 * O_DIRECT works on any common filesystem.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PATH        "./disk_write_test.dat"
#define DEFAULT_TOTAL_MIB   512

/* Block sizes to sweep, in bytes. */
static const size_t BLOCK_SIZES[] = {
    4   * 1024,
    64  * 1024,
    1   * 1024 * 1024,
    4   * 1024 * 1024,
};
static const size_t NUM_BLOCK_SIZES =
    sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

typedef enum {
    MODE_BUFFERED = 0,
    MODE_FSYNC,
    MODE_OSYNC,
    MODE_ODIRECT,
    MODE_COUNT
} mode_t_test;

static const char *MODE_NAMES[MODE_COUNT] = {
    "buffered",
    "fsync",
    "osync",
    "odirect",
};

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void format_bytes(double bytes, char *out, size_t out_len)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        unit++;
    }
    snprintf(out, out_len, "%.2f %s", bytes, units[unit]);
}

/*
 * Run a single benchmark.
 *
 *   path         file path to (re)create
 *   mode         which mode to run
 *   block_size   size of each write() call
 *   total_bytes  total amount to write
 *
 * Returns throughput in MiB/s, or -1.0 on error.
 */
static double run_one(const char *path,
                      mode_t_test mode,
                      size_t block_size,
                      uint64_t total_bytes)
{
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (mode == MODE_OSYNC)   flags |= O_SYNC;
    if (mode == MODE_ODIRECT) flags |= O_DIRECT;

    /* O_DIRECT requires the buffer and the request to be aligned to the
     * device's logical block size (commonly 512 or 4096 bytes).  We align
     * both the buffer and the block size to PAGE_SIZE to be safe. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;

    if (mode == MODE_ODIRECT && (block_size % (size_t)page) != 0) {
        /* Skip block sizes that don't satisfy O_DIRECT alignment. */
        return 0.0;
    }

    void *buf = NULL;
    if (posix_memalign(&buf, (size_t)page, block_size) != 0) {
        fprintf(stderr, "posix_memalign(%zu) failed: %s\n",
                block_size, strerror(errno));
        return -1.0;
    }
    /* Fill with non-zero data so transparent compression / dedupe on
     * smarter filesystems can't cheat. */
    for (size_t i = 0; i < block_size; i++) {
        ((unsigned char *)buf)[i] = (unsigned char)(i ^ 0xA5);
    }

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        free(buf);
        return -1.0;
    }

    uint64_t written = 0;
    double t0 = now_seconds();
    while (written < total_bytes) {
        size_t to_write = block_size;
        if (total_bytes - written < (uint64_t)to_write) {
            to_write = (size_t)(total_bytes - written);
            /* O_DIRECT also requires the request to be aligned -- if we
             * had a non aligned tail we'd need to pad.  We keep things
             * simple by ensuring total_bytes is a multiple of block_size. */
        }
        ssize_t n = write(fd, buf, to_write);
        if (n < 0) {
            fprintf(stderr, "write() failed in mode %s, bs=%zu: %s\n",
                    MODE_NAMES[mode], block_size, strerror(errno));
            close(fd);
            unlink(path);
            free(buf);
            return -1.0;
        }
        written += (uint64_t)n;
    }

    if (mode == MODE_FSYNC) {
        if (fsync(fd) != 0) {
            fprintf(stderr, "fsync() failed: %s\n", strerror(errno));
            close(fd);
            unlink(path);
            free(buf);
            return -1.0;
        }
    }
    double t1 = now_seconds();

    close(fd);
    unlink(path);
    free(buf);

    double elapsed = t1 - t0;
    if (elapsed <= 0.0) elapsed = 1e-9;
    double mib_per_s = ((double)written / (1024.0 * 1024.0)) / elapsed;
    return mib_per_s;
}

static bool parse_modes(const char *arg, bool out_enabled[MODE_COUNT])
{
    for (int i = 0; i < MODE_COUNT; i++) out_enabled[i] = false;

    char *dup = strdup(arg);
    if (!dup) return false;
    char *save = NULL;
    bool ok = true;
    for (char *tok = strtok_r(dup, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        bool matched = false;
        for (int i = 0; i < MODE_COUNT; i++) {
            if (strcmp(tok, MODE_NAMES[i]) == 0) {
                out_enabled[i] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "unknown mode: %s\n", tok);
            ok = false;
            break;
        }
    }
    free(dup);
    return ok;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-f path] [-s total_MiB] [-m modes]\n"
        "  -f  test file path (default: %s)\n"
        "  -s  total bytes per run in MiB (default: %d)\n"
        "  -m  comma separated modes: buffered,fsync,osync,odirect\n",
        prog, DEFAULT_PATH, DEFAULT_TOTAL_MIB);
}

int main(int argc, char **argv)
{
    const char *path = DEFAULT_PATH;
    uint64_t total_mib = DEFAULT_TOTAL_MIB;
    bool enabled[MODE_COUNT] = {true, true, true, true};

    int opt;
    while ((opt = getopt(argc, argv, "f:s:m:h")) != -1) {
        switch (opt) {
        case 'f':
            path = optarg;
            break;
        case 's': {
            char *end = NULL;
            unsigned long long v = strtoull(optarg, &end, 10);
            if (!end || *end != '\0' || v == 0) {
                fprintf(stderr, "invalid -s value: %s\n", optarg);
                return 2;
            }
            total_mib = (uint64_t)v;
            break;
        }
        case 'm':
            if (!parse_modes(optarg, enabled)) return 2;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    uint64_t total_bytes = total_mib * 1024ull * 1024ull;

    char total_str[64];
    format_bytes((double)total_bytes, total_str, sizeof(total_str));

    printf("Disk write benchmark\n");
    printf("  test file       : %s\n", path);
    printf("  bytes per run   : %s (%" PRIu64 " MiB)\n", total_str, total_mib);
    printf("  page size       : %ld bytes\n", sysconf(_SC_PAGESIZE));
    printf("\n");

    printf("%-10s %-10s %12s %12s\n",
           "mode", "block", "throughput", "time(s)");
    printf("%-10s %-10s %12s %12s\n",
           "----", "-----", "----------", "-------");

    for (int m = 0; m < MODE_COUNT; m++) {
        if (!enabled[m]) continue;
        for (size_t b = 0; b < NUM_BLOCK_SIZES; b++) {
            size_t bs = BLOCK_SIZES[b];
            char bs_str[32];
            format_bytes((double)bs, bs_str, sizeof(bs_str));

            /* round total down to a multiple of bs to keep the math clean */
            uint64_t rounded = (total_bytes / bs) * bs;
            if (rounded == 0) rounded = bs;

            double t0 = now_seconds();
            double mib_per_s = run_one(path, (mode_t_test)m, bs, rounded);
            double dt = now_seconds() - t0;

            if (mib_per_s < 0.0) {
                printf("%-10s %-10s %12s %12s\n",
                       MODE_NAMES[m], bs_str, "ERROR", "-");
            } else if (mib_per_s == 0.0) {
                printf("%-10s %-10s %12s %12s\n",
                       MODE_NAMES[m], bs_str, "skipped", "-");
            } else {
                char tput[32];
                snprintf(tput, sizeof(tput), "%.2f MiB/s", mib_per_s);
                printf("%-10s %-10s %12s %12.3f\n",
                       MODE_NAMES[m], bs_str, tput, dt);
            }
            fflush(stdout);
        }
    }

    return 0;
}
