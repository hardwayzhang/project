/*
 * crc_hw.c - x86 CRC hardware-acceleration detection + implementation.
 *
 * This file is intentionally compiled with the default (baseline) x86 ISA.
 * No -msse4.2 / -mpclmul / -mavx flags are required on the command line:
 * the only function that uses SSE4.2 intrinsics is explicitly annotated
 * with __attribute__((target("sse4.2"))), which makes the compiler emit
 * the `crc32` instruction inside that one function only, while keeping
 * the rest of the translation unit (and the rest of the program) at the
 * baseline ISA so binaries remain runnable on older CPUs.
 */

#include "crc_hw.h"

#include <string.h>
#include <cpuid.h>      /* __get_cpuid, bit_SSE4_2 */
#include <nmmintrin.h>  /* _mm_crc32_u8/u32/u64 (intrinsics carry their own
                         * target("sse4.2") attribute in modern GCC/Clang,
                         * so they're usable without -msse4.2). */

/* ----- Detection --------------------------------------------------------- */

static bool detect_sse42_once(void) {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx & bit_SSE4_2) != 0;
}

bool crc_hw_supported(void) {
    /* Cache the result. -1 = unknown, 0 = no, 1 = yes. Initialisation of
     * function-local statics is thread-safe in C11. */
    static int cached = -1;
    if (cached < 0) cached = detect_sse42_once() ? 1 : 0;
    return cached == 1;
}

/* ----- Software reference (CRC32C, Castagnoli polynomial 0x1EDC6F41) ----- */

static uint32_t crc32c_sw_table[256];
static bool     crc32c_sw_table_inited;

static void crc32c_sw_table_init(void) {
    if (crc32c_sw_table_inited) return;
    /* Reflected polynomial. */
    const uint32_t poly = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ poly : (c >> 1);
        }
        crc32c_sw_table[i] = c;
    }
    crc32c_sw_table_inited = true;
}

uint32_t crc32c_sw(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc32c_sw_table_init();
    crc = ~crc;
    while (len--) {
        crc = crc32c_sw_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

/* ----- Hardware implementation ------------------------------------------- */

/* This is the *only* function in the program that needs SSE4.2 codegen.
 * The target attribute lets the compiler emit `crc32` here without affecting
 * any other function or requiring a global -msse4.2 build flag. */
__attribute__((target("sse4.2")))
uint32_t crc32c_hw(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
#if defined(__x86_64__)
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        crc = (uint32_t)_mm_crc32_u64(crc, v);
        p += 8;
        len -= 8;
    }
#endif
    while (len >= 4) {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        crc = _mm_crc32_u32(crc, v);
        p += 4;
        len -= 4;
    }
    while (len--) {
        crc = _mm_crc32_u8(crc, *p++);
    }
    return ~crc;
}
