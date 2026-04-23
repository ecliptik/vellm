/*
 * timing.c — cycle counter + serial-debug stubs.
 *
 * DJGPP uses rdtsc directly (32-bit, "=A" pairs eax:edx into a u64).
 * Native host builds fall back to clock_gettime for a monotonic counter.
 *
 * The serial-debug hooks compile to no-ops unless -DVELLM_SERIAL_DEBUG
 * is defined. When enabled on DOS, they will poke COM2 (0x2F8) directly;
 * that code is deferred to the "stretch" phase in PLAN.md.
 */

#include "timing.h"

#ifdef __DJGPP__

uint64_t now_cycles(void) {
    uint64_t result;
    __asm__ volatile("rdtsc" : "=A"(result));
    return result;
}

#else /* !__DJGPP__ — native Linux host */

#define _POSIX_C_SOURCE 200809L
#include <time.h>

uint64_t now_cycles(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif /* __DJGPP__ */

#ifdef VELLM_SERIAL_DEBUG
/* Real impl deferred — see PLAN.md "Stretch". */
void serial_debug_init(void) { /* TODO: COM2 init */ }
void serial_debug_write(const char *s) { (void)s; /* TODO: byte-bang */ }
#else
void serial_debug_init(void) { }
void serial_debug_write(const char *s) { (void)s; }
#endif
