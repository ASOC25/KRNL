#include <krnl/libraries/lock/spinlock.h>

int global_spinlock_counter = 0;

__attribute__((noinline)) int spinlock_acquire(spinlock_t *lock) {
    volatile size_t deadlock_counter = 0;
    for (;;) {
        /* BUG-55 fix: disable interrupts BEFORE test-and-set so no interrupt
         * can fire between acquiring the lock and masking interrupts. */
        __asm__("cli");
        if (spinlock_test_and_acq(lock)) {
            global_spinlock_counter++;
            break;
        }
        /* Failed to get the lock — re-enable interrupts while spinning */
        __asm__("sti");
        if (++deadlock_counter >= 10000000) {
            return -1;
        }
#if defined (__x86_64__)
        __asm__ volatile ("pause");
#endif
    }
    lock->last_acquirer = __builtin_return_address(0);
    return 0;
}