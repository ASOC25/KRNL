#include <krnl/debug/perf.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/hpet.h>

uint64_t perf_now_us(void) {
    return hpet_get_current_time();
}

void perf_stop(perf_timer_t *t, const char *label) {
    uint64_t cycles = perf_rdtsc() - t->tsc;
    uint64_t us     = perf_now_us() - t->us;
    kprintf("[PERF] %s: %llu us  (%llu cycles)\n", label, us, cycles);
}
