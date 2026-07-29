#ifndef _SIGNALS_H
#define _SIGNALS_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/libraries/std/time.h>
#include <krnl/process/sigstructs.h>

/* Blocks the thread until wakeup(condition) is called or a deliverable
   signal arrives. Returns 1 if woken normally (by wakeup()), 0 if woken
   early because a signal became pending — callers that can be interrupted
   (waitpid, blocking read on a tty/pipe, futex_wait, ...) should treat 0 as
   "return -EINTR now" rather than re-checking their wait condition and
   blocking again, since the actual signal handler only runs once the thread
   is next scheduled in user context, which won't happen while it keeps
   re-entering this kernel-side wait. */
int sleep(thread_t * process, int64_t condition);
void wakeup(int64_t condition);
int check_sleep_condition(int64_t condition);
/* Removes any pending sleep-list bookkeeping for `thread`. Needed when a
   thread's state is forced away from INTERRUPTIBLE_SLEEP without going
   through wakeup() (e.g. woken early to receive a signal, then re-stopped by
   SIGSTOP/SIGTSTP delivery) — otherwise the stale list node lingers and a
   later, unrelated wakeup() on the same condition id spuriously resumes it. */
void cancel_sleep(thread_t * thread);
status_t nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem);
void update_counters(void);
status_t sigprocmask(sigset_t * current_mask, int how, const sigset_t * set, sigset_t * oldset);
#endif