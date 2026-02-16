#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include <krnl/libraries/std/stdbool.h>
#include <krnl/libraries/std/stddef.h>

#define SPINLOCK_INIT {0, NULL}

extern int global_spinlock_counter;

typedef struct {
    int lock;
    void *last_acquirer;
} spinlock_t;

static inline bool spinlock_test_and_acq(spinlock_t *lock) {
    return __sync_bool_compare_and_swap(&lock->lock, 0, 1);
}

int spinlock_acquire(spinlock_t *lock);
static inline void spinlock_release(spinlock_t *lock);

static inline void spinlock_release(spinlock_t *lock) {
    lock->last_acquirer = NULL;
    __atomic_store_n(&lock->lock, 0, __ATOMIC_SEQ_CST);
    global_spinlock_counter--;
    if (global_spinlock_counter <= 0) {
        global_spinlock_counter = 0;
        __asm__ volatile ("sti");
    }
}

#endif