/*
 * crc_hw.h - x86 CRC hardware-acceleration detection API.
 *
 * This module targets the x86 / x86_64 platform only. It provides a small,
 * stable C API:
 *
 *   bool     crc_hw_supported(void);
 *   uint32_t crc32c_hw(uint32_t crc, const void *buf, size_t len);
 *   uint32_t crc32c_sw(uint32_t crc, const void *buf, size_t len);
 *
 * - crc_hw_supported() returns true if and only if the current CPU implements
 *   the SSE4.2 `crc32` instruction (i.e. CRC32C in hardware). The result is
 *   cached after the first call.
 * - crc32c_hw() MUST only be invoked when crc_hw_supported() returns true.
 *   Calling it otherwise yields undefined behaviour (illegal instruction).
 * - crc32c_sw() is a portable, table-driven CRC32C reference, suitable as a
 *   fallback or for verification.
 *
 * Building:
 *   The whole project is compiled with the *baseline* x86 ISA - no global
 *   -msse4.2 / -mpclmul flags are required. Only the hardware-accelerated
 *   function uses __attribute__((target("sse4.2"))) so the rest of the
 *   translation unit stays at the default ISA level.
 */

#ifndef CRC_HW_H
#define CRC_HW_H

#if !defined(__x86_64__) && !defined(__i386__)
#  error "crc_hw is x86-only; rebuild on an x86 / x86_64 target."
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the host CPU supports SSE4.2 CRC32 in hardware. */
bool crc_hw_supported(void);

/* Hardware CRC32C (Castagnoli) using the SSE4.2 `crc32` instruction.
 * Precondition: crc_hw_supported() == true. */
uint32_t crc32c_hw(uint32_t crc, const void *buf, size_t len);

/* Portable software CRC32C - identical result to crc32c_hw().
 * Safe to call on any CPU. */
uint32_t crc32c_sw(uint32_t crc, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC_HW_H */
