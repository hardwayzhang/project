/*
 * fast_backtrace.c -- frame-pointer based unwinder.
 *
 * On x86_64 (System V ABI) with -fno-omit-frame-pointer, every frame
 * starts with:
 *
 *      [ rbp + 0  ]  -> saved rbp of caller (i.e. caller's frame)
 *      [ rbp + 8  ]  -> return address into caller
 *
 * On aarch64 (AAPCS64) with -fno-omit-frame-pointer, the layout is
 * symmetric using x29 as the frame pointer:
 *
 *      [ x29 + 0  ]  -> saved x29 of caller
 *      [ x29 + 8  ]  -> saved x30 (lr) -- return address into caller
 *
 * So unwinding is just:
 *      while (fp && depth < max) { ret = fp[1]; fp = fp[0]; }
 *
 * To stay safe even when the chain is broken (e.g. crossing into a
 * frame compiled with -fomit-frame-pointer, or a corrupted stack) we
 * apply some cheap sanity checks:
 *   - fp must be non-NULL and pointer-aligned
 *   - fp must be monotonically increasing (stacks grow downward, so
 *     parent frames live at higher addresses)
 *   - fp must lie within [stack_lo, stack_hi) for the current thread,
 *     which we obtain lazily via pthread_getattr_np()
 *
 * The stack-bounds lookup happens once per thread and is cached in a
 * thread-local; this keeps the hot path branch-light.
 */

#define _GNU_SOURCE
#include "fast_backtrace.h"

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/resource.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#warning "fast_backtrace: only x86_64 and aarch64 are supported; falling back to a no-op"
#endif

struct frame {
    struct frame *next;   /* saved frame pointer of caller */
    void         *ret;    /* return address into caller    */
};

/* Per-thread cached stack bounds. The pair (lo, hi) describes the
 * half-open interval of valid frame-pointer values. We initialise
 * lazily on first call. */
static __thread uintptr_t tls_stack_lo = 0;
static __thread uintptr_t tls_stack_hi = 0;
static __thread int       tls_stack_init = 0;

static void init_stack_bounds(void)
{
    pthread_attr_t attr;
    void *stack_addr = NULL;
    size_t stack_size = 0;

    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0) {
            tls_stack_lo = (uintptr_t)stack_addr;
            tls_stack_hi = (uintptr_t)stack_addr + stack_size;
        }
        pthread_attr_destroy(&attr);
    }

    /* If the query failed (e.g. main thread on some libcs), fall back
     * to a permissive range so we still walk something. The monotonic
     * check below will still cut runaway pointers. */
    if (tls_stack_hi == 0) {
        tls_stack_lo = 0;
        tls_stack_hi = UINTPTR_MAX;
    }
    tls_stack_init = 1;
}

static inline int fp_looks_sane(uintptr_t fp)
{
    if (fp == 0) return 0;
    if (fp & (sizeof(void *) - 1)) return 0;              /* misaligned */
    if (fp < tls_stack_lo || fp >= tls_stack_hi) return 0; /* out of stack */
    return 1;
}

int fast_backtrace_from(void *start_fp, void **buffer, int max_frames)
{
    if (max_frames <= 0 || buffer == NULL) return 0;

#if defined(__x86_64__) || defined(__aarch64__)
    if (!tls_stack_init) init_stack_bounds();

    struct frame *fp;
    if (start_fp != NULL) {
        fp = (struct frame *)start_fp;
    } else {
        fp = (struct frame *)__builtin_frame_address(0);
    }

    int n = 0;
    uintptr_t prev = 0;

    while (n < max_frames && fp_looks_sane((uintptr_t)fp)) {
        /* Frame pointers must be monotonically increasing as we walk
         * outwards; if not, the chain is broken or corrupted. */
        if ((uintptr_t)fp <= prev) break;
        prev = (uintptr_t)fp;

        void *ret = fp->ret;
        if (ret == NULL) break;

        buffer[n++] = ret;
        fp = fp->next;
    }
    return n;
#else
    (void)start_fp; (void)buffer; (void)max_frames;
    return 0;
#endif
}

int __attribute__((noinline)) fast_backtrace(void **buffer, int max_frames)
{
#if defined(__x86_64__) || defined(__aarch64__)
    /* Start from our own frame; the first return address we record is
     * fp->ret, which is the address inside *our caller* -- i.e. exactly
     * what backtrace() conventionally returns as frame #0. We deliberately
     * avoid __builtin_frame_address(1), which gcc warns about. */
    void *fp = __builtin_frame_address(0);
    if (fp == NULL) return 0;
    return fast_backtrace_from(fp, buffer, max_frames);
#else
    (void)buffer; (void)max_frames;
    return 0;
#endif
}
