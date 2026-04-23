/*
 * timing.h — rdtsc cycle counter + serial-debug hooks.
 *
 * now_cycles() returns a free-running cycle counter:
 *   - On DJGPP (32-bit), via `rdtsc` asm (eax:edx into "=A").
 *   - On the Linux host, via clock_gettime(CLOCK_MONOTONIC) translated to a
 *     uint64_t "ticks" value (nanoseconds, not CPU cycles — callers should
 *     treat the return as an opaque monotonic counter).
 *
 * serial_debug_{init,write} are no-ops unless -DVELLM_SERIAL_DEBUG is set.
 * The real implementation (direct COM2/IRQ3 port writes) is a stretch goal.
 */
#ifndef VELLM_TIMING_H
#define VELLM_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t now_cycles(void);

void serial_debug_init(void);
void serial_debug_write(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* VELLM_TIMING_H */
