#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>

#define SCHEDULER_TIMESLICE_MS 100

#define SCHEDULER_SOURCE_TIMER_INTERRUPT 0
#define SCHEDULER_SOURCE_YIELD_SYSCALL 1
#define SCHEDULER_SOURCE_OTHER 2

#define SCHEDULER_STATUS_RUNABLE 0
#define SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP 1
#define SCHEDULER_STATUS_UNINTERRUPTIBLE_SLEEP 2
#define SCHEDULER_STATUS_STOPPED 3
#define SCHEDULER_STATUS_CONTINUED 4
#define SCHEDULER_STATUS_ZOMBIE 5

#define SCHEDULER_USER_CONTEXT 0
#define SCHEDULER_KERNEL_CONTEXT 2

#define _WSTATUS(x)                         ((x) & 0177)
#define _WSTOPPED                           0177
#define _WCONTINUED                         0177777
#define WIFSTOPPED(x)                       (((x) & 0xff) == _WSTOPPED)
#define WSTOPSIG(x)                         (int)(((unsigned)(x) >> 8) & 0xff)
#define WIFSIGNALED(x)                      (_WSTATUS(x) != _WSTOPPED && _WSTATUS(x) != 0)
#define WTERMSIG(x)                         (_WSTATUS(x))
#define WIFEXITED(x)                        (_WSTATUS(x) == 0)
#define WEXITSTATUS(x)                      (int)(((unsigned)(x) >> 8) & 0xff)
#define WIFCONTINUED(x)                     (((x) & _WCONTINUED) == _WCONTINUED)
#define WCOREFLAG                           0200
#define WCOREDUMP(x)                        ((x) & WCOREFLAG)
#define W_EXITCODE(ret, sig)                ((ret) << 8 | (sig))
#define W_STOPCODE(sig)                     ((sig) << 8 | _WSTOPPED)

#define WREASON_EXIT                        0x01
#define WREASON_STOP                        0x02
#define WREASON_CONT                        0x04
#define WREASON_SIGNAL                      0x08

#define WNOHANG                             0x01
#define WUNTRACED                           0x02
#define WCONTINUED                          0x08
#define WEXITED                             0x04
#define WSTOPPED                            WUNTRACED
#define WNOWAIT                             0x10
#define WTRAPPED                            0x20

#define WAIT_ANY                            (-1)
#define WAIT_MYGRP                          0

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id, uint8_t source, uint8_t save_current);
status_t scheduler_add(thread_t * thread);
status_t scheduler_remove(thread_t * thread);
int scheduler_waitpid(thread_t * caller, int pid, int * status, int options);
void dump_scheduler_status();
process_t * scheduler_get_process_by_pid(pid_t pid);
void scheduler_sigreturn(cpu_context_t* ctx, thread_t * thread);
#endif