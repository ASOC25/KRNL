#ifndef _KRNL_PERF_H_
#define _KRNL_PERF_H_

#include <krnl/libraries/std/stdint.h>

/*
 * Lightweight kernel performance measurement.
 *
 * Quick usage:
 *
 *   PERF_BEGIN(t);
 *   ... code to measure ...
 *   PERF_END(t, "label");
 *
 * Prints via kprintf:  [PERF] label: 42 us  (1234567 cycles)
 *
 * For manual use:
 *
 *   uint64_t t0 = perf_rdtsc();   // cheapest, cycle-accurate
 *   uint64_t t0 = perf_now_us();  // HPET microseconds
 */

typedef struct {
    uint64_t tsc;
    uint64_t us;
} perf_timer_t;

/* Read the TSC directly — one instruction, no locks, no MMIO. */
static inline uint64_t perf_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Current time in microseconds from the HPET. */
uint64_t perf_now_us(void);

/* Start a timer. */
static inline void perf_start(perf_timer_t *t) {
    t->us  = perf_now_us();
    t->tsc = perf_rdtsc();
}

/* Stop a timer and print elapsed time tagged with label. */
void perf_stop(perf_timer_t *t, const char *label);

/* Convenience macros — var must be a valid C identifier. */
#define PERF_BEGIN(var)         perf_timer_t var; perf_start(&(var))
#define PERF_END(var, label)    perf_stop(&(var), (label))

#endif
