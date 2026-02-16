#ifndef _SIGNALS_H
#define _SIGNALS_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/libraries/std/time.h>
#include <krnl/process/sigstructs.h>

void sleep(thread_t * process, int condition);
void wakeup(int condition);
int check_sleep_condition(int condition);
status_t nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem);

void update_counters();
void __attribute__((__section__(".vdso"))) signal_trampoline(int signo, sigaction_t * sigact, cpu_context_t* ctx);
status_t dequeue_signal(sigaction_t ** squeue, signal_t * out_signal, int signo);
status_t sigaction(sigaction_t * shandlers, int signum, sigaction_t * act, sigaction_t * oldact);
status_t sigsuspend(sigset_t* sigsuspend_mask, const sigset_t *mask);
status_t sigprocmask(sigset_t * sigprocmask, int how, const sigset_t * set, sigset_t * oldset);
status_t sigpending(signal_t **squeue, sigset_t * set);
status_t kill(signal_t **squeue, int signal);
status_t sigaltstack(stack_t* stack, stack_t* ss, stack_t * old_ss);
#endif