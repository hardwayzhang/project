/*
 * bench.c -- micro-benchmark and correctness check comparing
 *
 *   (a) glibc's backtrace() from <execinfo.h>
 *       (uses libgcc_s _Unwind_Backtrace, walks .eh_frame DWARF tables)
 *
 *   (b) our fast_backtrace() (frame-pointer chain walk)
 *
 * Build with -fno-omit-frame-pointer so that (b) actually has a chain
 * to walk. (a) does not need frame pointers because it parses DWARF
 * unwind info, which gcc emits regardless of -fomit-frame-pointer.
 */

#define _GNU_SOURCE
#include "fast_backtrace.h"

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define MAX_FRAMES 256

/* Use volatile sinks so the optimiser does not delete the calls. */
static volatile int    g_sink_n;
static void * volatile g_sink_buf[MAX_FRAMES];
/* Reported separately from recurse()'s return value, because gcc's
 * tail-recursion-modulo-addition optimisation (see recurse() below)
 * makes the return value an unreliable proxy for the actual unwind
 * depth. The bench function writes here on every iteration. */
static volatile int    g_actual_frames;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ----- recursion to build a deep call stack ------------------------- */

typedef int (*bench_fn_t)(int iters);

/*
 * __attribute__((noinline)) keeps the function out-of-line, but it does
 * NOT prevent gcc from rewriting the body. With -O2, gcc applies
 * "tail-recursion modulo addition" and turns
 *
 *     return recurse(depth - 1, ...) + 1;
 *
 * into an iterative loop with an accumulator -- collapsing all N levels
 * into a single stack frame. We saw this empirically: objdump showed no
 * `call recurse` inside recurse(), and unwinders only saw 1 recurse frame.
 *
 * To force a real call per level, we put a memory-clobbering inline asm
 * between the recursive call and the post-call work. The "+r"(r) tells
 * gcc the asm reads AND writes `r`, and "memory" forbids reordering
 * memory accesses across it. That's an opaque side effect bound to the
 * call's return value, which defeats both plain TCO and the modulo-add
 * transformation.
 */
/*
 * The bench/correctness functions are intentionally NOT static and are
 * tagged externally_visible. backtrace_symbols() resolves names through
 * dladdr(3), which only sees the dynamic symbol table (.dynsym). With
 * -rdynamic on the link line, *global* symbols are exported into .dynsym
 * and become visible to dladdr. Static functions never make it there
 * (they only live as local entries in .symtab) and would resolve to
 * just "./bench_backtrace(+offset)". Making them extern + visible
 * fixes the symbolic backtrace output.
 */
#define BENCH_API __attribute__((noinline, visibility("default")))

int BENCH_API recurse(int depth, bench_fn_t fn, int iters);
int BENCH_API recurse(int depth, bench_fn_t fn, int iters)
{
    if (depth <= 0) {
        return fn(iters);
    }
    int r = recurse(depth - 1, fn, iters);
    __asm__ __volatile__("" : "+r"(r) : : "memory");
    return r + 1;
}

/* ----- benchmark bodies --------------------------------------------- */

int BENCH_API do_glibc_backtrace(int iters);
int BENCH_API do_glibc_backtrace(int iters)
{
    void *buf[MAX_FRAMES];
    int n = 0;
    for (int i = 0; i < iters; i++) {
        n = backtrace(buf, MAX_FRAMES);
    }
    /* publish to defeat DCE */
    g_sink_n = n;
    g_actual_frames = n;
    for (int i = 0; i < n; i++) g_sink_buf[i] = buf[i];
    return n;
}

int BENCH_API do_fast_backtrace(int iters);
int BENCH_API do_fast_backtrace(int iters)
{
    void *buf[MAX_FRAMES];
    int n = 0;
    for (int i = 0; i < iters; i++) {
        n = fast_backtrace(buf, MAX_FRAMES);
    }
    g_sink_n = n;
    g_actual_frames = n;
    for (int i = 0; i < n; i++) g_sink_buf[i] = buf[i];
    return n;
}

/* ----- correctness check (one shot, prints both stacks) ------------- */

int BENCH_API capture_glibc(int iters);
int BENCH_API capture_glibc(int iters)
{
    (void)iters;
    void *buf[MAX_FRAMES];
    int n = backtrace(buf, MAX_FRAMES);

    char **syms = backtrace_symbols(buf, n);
    printf("[glibc backtrace]      depth=%d\n", n);
    for (int i = 0; i < n; i++) {
        printf("  #%-2d %p  %s\n", i, buf[i], syms ? syms[i] : "(no symbols)");
    }
    free(syms);
    return n;
}

int BENCH_API capture_fast(int iters);
int BENCH_API capture_fast(int iters)
{
    (void)iters;
    void *buf[MAX_FRAMES];
    int n = fast_backtrace(buf, MAX_FRAMES);

    /* backtrace_symbols() works on any return-address array, so we can
     * reuse it to verify the addresses we collected resolve sensibly. */
    char **syms = backtrace_symbols(buf, n);
    printf("[fast_backtrace (FP)]  depth=%d\n", n);
    for (int i = 0; i < n; i++) {
        printf("  #%-2d %p  %s\n", i, buf[i], syms ? syms[i] : "(no symbols)");
    }
    free(syms);
    return n;
}

/* ----- timing harness ----------------------------------------------- */

static double __attribute__((noinline)) bench(const char *name, int depth, int iters, bench_fn_t fn)
{
    /* warm-up: pages, TLS init, dlopen of libgcc_s.so.1 for glibc path */
    recurse(depth, fn, 1000);

    uint64_t t0 = now_ns();
    (void)recurse(depth, fn, iters);
    uint64_t t1 = now_ns();

    double total_ns = (double)(t1 - t0);
    double per_call = total_ns / (double)iters;
    printf("  %-22s depth=%-3d iters=%-9d  %9.1f ns/call  (actual_frames=%d, total=%.2f ms)\n",
           name, depth, iters, per_call, g_actual_frames, total_ns / 1e6);
    return per_call;
}

int main(int argc, char **argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 200000;
    if (iters <= 0) iters = 200000;

    printf("==== correctness ====\n");
    recurse(8, capture_glibc, 0);
    putchar('\n');
    recurse(8, capture_fast,  0);
    putchar('\n');

    printf("==== performance ====\n");
    int depths[] = { 4, 16, 64, 128 };
    for (size_t i = 0; i < sizeof(depths)/sizeof(depths[0]); i++) {
        int d = depths[i];
        printf("-- depth ~%d --\n", d);
        double a = bench("glibc backtrace()",   d, iters, do_glibc_backtrace);
        double b = bench("fast_backtrace (FP)", d, iters, do_fast_backtrace);
        if (b > 0) {
            printf("  speedup: %.2fx\n\n", a / b);
        }
    }
    return 0;
}
