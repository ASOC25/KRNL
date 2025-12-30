#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>

#define SCHEDULER_TIMESLICE_MS 100

#define CONTEXT_SAVE_USPACE 0
#define CONTEXT_SAVE_KSPACE 1

#define SCHEDULER_SOURCE_TIMER_INTERRUPT 0
#define SCHEDULER_SOURCE_YIELD_SYSCALL 1
#define SCHEDULER_SOURCE_OTHER 2

#define SCHEDULER_STATUS_RUNABLE 0
#define SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP 1
#define SCHEDULER_STATUS_UNINTERRUPTIBLE_SLEEP 2
#define SCHEDULER_STATUS_STOPPED 3
#define SCHEDULER_STATUS_ZOMBIE 4

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id, uint8_t is_kernel_ctx);

status_t scheduler_add(thread_t * thread);
status_t scheduler_remove(thread_t * thread);

//who values:
// Positive numbers > 100: Send to all threads in the process with the given PID
// 3: Send to all threads in all processes
// 2: Send to all threads in the current process
// 1: Send to current thread
// 0 and negative numbers: Send to all threads with the state equal to the absolute value of who
status_t scheduler_send_event(int event, int who);

#endif