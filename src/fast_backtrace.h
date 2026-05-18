/*
 * fast_backtrace.h
 *
 * A frame-pointer based stack unwinder. Compared to glibc's backtrace()
 * (which on Linux relies on libgcc_s/_Unwind_Backtrace + DWARF .eh_frame
 * tables), this implementation simply walks the saved frame-pointer chain
 * (rbp on x86_64, x29 on aarch64).
 *
 * Requirements:
 *   - The whole program (and the libraries whose frames you want to walk)
 *     must be compiled with -fno-omit-frame-pointer.
 *   - Currently x86_64 and aarch64 are supported.
 *
 * Trade-offs vs. backtrace():
 *   + 1 to 2 orders of magnitude faster: no DWARF parsing, no locks,
 *     no _Unwind_RaiseException machinery, just pointer chasing.
 *   + Async-signal-safe (no malloc, no dlopen, no global locks).
 *   - Will produce wrong/short results in any frame whose function was
 *     compiled with -fomit-frame-pointer (very common in distro libc/.so).
 *   - Cannot unwind through hand-written assembly that does not maintain
 *     a frame-pointer chain.
 */

#ifndef FAST_BACKTRACE_H
#define FAST_BACKTRACE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Capture up to `max_frames` return addresses of the current call chain
 * into `buffer`, starting from the caller of fast_backtrace() itself.
 *
 * Returns the number of frames actually captured (0..max_frames).
 *
 * Async-signal-safe and lock-free.
 */
int fast_backtrace(void **buffer, int max_frames);

/*
 * Same as fast_backtrace() but starts walking from `start_fp`. Useful
 * when called from a signal handler where you have the interrupted
 * context's frame pointer.
 *
 * If `start_fp` is NULL, the current frame pointer is used.
 */
int fast_backtrace_from(void *start_fp, void **buffer, int max_frames);

#ifdef __cplusplus
}
#endif

#endif /* FAST_BACKTRACE_H */
