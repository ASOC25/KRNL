#include <krnl/process/scheduler.h>
extern void kcontext_restore_trampoline(cpu_context_t *kctx);
extern void kcontext_simple_launch(cpu_context_t *kctx);
extern void set_cpu_fs_base(uint64_t base);
extern void simd_restore_context(void *ctx);
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/limits.h>
#include <krnl/process/process.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/process/signals.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/libraries/std/wait.h>
#include <krnl/mem/vmm.h>
#include <krnl/libraries/std/string.h>

typedef struct scheduler_queue {
    thread_t * thread;
    struct scheduler_queue * next;
} scheduler_queue_t;

scheduler_queue_t * sched_queue = NULL;
thread_t * current_thread = NULL;
static thread_t * g_idle_thread = NULL;

static pid_t scheduler_get_free_pid_locked(void) {
    static pid_t last_pid = 100; // Start from 100 to avoid reserved PIDs
    scheduler_queue_t * current = sched_queue;
    pid_t candidate_pid = last_pid;
    int found;
    do {
        found = 0;
        candidate_pid++;
        if (candidate_pid < 100) {
            candidate_pid = 100; // Wrap around to avoid reserved PIDs
        }
        current = sched_queue;
        while (current != NULL) {
            if (GET_PROC(current->thread)->pid == candidate_pid) {
                found = 1;
                break;
            }
            current = current->next;
        }
    } while (found);
    last_pid = candidate_pid;
    return candidate_pid;
}

static pid_t scheduler_get_free_tid_locked(void) {
    static pid_t last_tid = 0; // Start from 0
    scheduler_queue_t * current = sched_queue;
    pid_t candidate_tid = last_tid;
    int found;
    do {
        found = 0;
        candidate_tid++;
        if (candidate_tid < 0) {
            candidate_tid = 0; // Wrap around
        }
        current = sched_queue;
        while (current != NULL) {
            if (current->thread->tid == candidate_tid) {
                found = 1;
                break;
            }
            current = current->next;
        }
    } while (found);
    last_tid = candidate_tid;
    return candidate_tid;
}

pid_t scheduler_get_free_pid() {
    pid_t pid;

    pid = scheduler_get_free_pid_locked();

    return pid;
}

uint8_t comparator_alpha(process_t * caller, process_t * iterated, int pid) {
    //return if any child process such that  pgid = abs(pid) has exited
    return (iterated->ppid == caller->pid && iterated->pgid == (pid_t)(-pid));
}
    //return if any child process has exited
uint8_t comparator_beta(process_t * caller, process_t * iterated, int pid) {
    (void)pid;
    return (iterated->ppid == caller->pid);
}
uint8_t comparator_gamma(process_t * caller, process_t * iterated, int pid) {
    (void)pid;
    //return if any child pgid=pgid of caller has exited
    return (iterated->ppid == caller->pid && iterated->pgid == caller->pgid);
}
uint8_t comparator_delta(process_t * caller, process_t * iterated, int pid) {
    //return if specific pid has exited
    return (iterated->ppid == caller->pid && iterated->pid == (pid_t)(pid));
}

static int generate_status(int reason, int value) {
    if (reason & WREASON_CONT) {
        return _WCONTINUED;
    }
    if (reason & WREASON_STOP) {
        return W_STOPCODE(value & 0xff);
    }
    if (reason & WREASON_SIGNAL) {
        return W_EXITCODE(0, value & 0x7f);
    }
    if (reason & WREASON_EXIT) {
        return W_EXITCODE(value & 0xff, 0);
    }
    return 0;
}

//Used for waitpid
int scheduler_waitpid(thread_t * caller, int pid, int * status, int options) {
	//values for pid:
    //under -1 = return if any child process such that  gid = abs(pid) has exited
	//exactly -1 = return if any child process has exited
	//exactly 0 = return if any child gid=gid of caller has exited
	//over 0 = return if specific pid has exited
    uint8_t (*comparator)(process_t *, process_t *, int) = NULL;
    switch (pid) {
        case -1:
            comparator = comparator_beta;
            break;
        case 0:
            comparator = comparator_gamma;
            break;
        default:
            if (pid < -1) {
                comparator = comparator_alpha;
            } else {
                comparator = comparator_delta;
            }
            break;
    }
    
    int changed_pid = 0;
    while (!changed_pid) {
        uint8_t found_one = 0;
        //iterate over all threads in the scheduler queue
        scheduler_queue_t * current = sched_queue;
        while (current != NULL) {
            process_t * iterated_process = GET_PROC(current->thread);
            process_t * caller_process = GET_PROC(caller);
            
            //Make sure we don't check the caller process itself
            if (iterated_process == caller_process) {
                current = current->next;
                continue;
            }

            if (comparator(caller_process, iterated_process, pid)) {
                found_one = 1;
                //kprintf("scheduler_waitpid: Found matching process %d for caller %d\n", iterated_process->pid, caller_process->pid);
                //Check if the process has exited
                if (options & WNOHANG) {
                    //kprintf("WNOHANG option set\n");
                    if (iterated_process->state == SCHEDULER_STATUS_ZOMBIE) {
                        //kprintf("scheduler_waitpid WNOHANG: Reaping process %d for caller %d\n", iterated_process->pid, caller_process->pid);
                        //Reap process — save PID before destroy to avoid use-after-free (BUG-25)
                        pid_t reaped_pid = iterated_process->pid;
                        if (status) {
                            *status = generate_status(WREASON_EXIT, iterated_process->exit_code);
                        }
                        process_destroy(iterated_process);
                        changed_pid = reaped_pid;  // only set when a zombie was actually reaped (BUG-24)
                    }
                    // If not a zombie, changed_pid stays 0 — POSIX requires returning 0 for WNOHANG
                } else if ((options & WUNTRACED) && iterated_process->state == SCHEDULER_STATUS_STOPPED) {
                    //kprintf("scheduler_waitpid: Process %d is stopped for caller %d\n", iterated_process->pid, caller_process->pid);
                    if (status) {
                        *status = generate_status(WREASON_STOP, iterated_process->exit_code);
                    }
                    changed_pid = iterated_process->pid;
                } else if ((options & WCONTINUED) && iterated_process->state == SCHEDULER_STATUS_CONTINUED) {
                    //kprintf("scheduler_waitpid: Process %d is continued for caller %d\n", iterated_process->pid, caller_process->pid);
                    if (status) {
                        *status = generate_status(WREASON_CONT, 0);
                    }
                    changed_pid = iterated_process->pid;
                } else {
                    //kprintf("ELSE BRANCH\n");
                    if (iterated_process->state == SCHEDULER_STATUS_ZOMBIE) {
                        //kprintf("scheduler_waitpid: Reaping process %d for caller %d\n", iterated_process->pid, caller_process->pid);
                        //Reap process
                        if (status) {
                            *status = generate_status(WREASON_EXIT, iterated_process->exit_code);
                        }
                        pid_t reaped_pid = iterated_process->pid;
                        process_destroy(iterated_process);
                        changed_pid = reaped_pid;
                    }
                }
            }
            current = current->next;
            //kprintf("scheduler_waitpid: Moving to next process in scheduler queue\n");
        }
        if (!found_one) return -ECHILD;
        //No matching exited process found
        if (!changed_pid) {
            //WNOHANG: POSIX requires returning 0 when no child has changed state (BUG-24)
            if (options & WNOHANG) return 0;

            /* sleep() returns 0 if we were woken early because a signal
             * became pending rather than because of an actual child state
             * change. Kernel-context blocking calls like this one only get
             * their thread flipped back to RUNABLE when woken — the signal
             * itself isn't delivered until the thread is next scheduled in
             * *user* context. Looping back around without checking this
             * would let waitpid silently swallow that wakeup and keep
             * blocking, so the signal ends up applied arbitrarily far into
             * whatever the caller runs once this syscall eventually returns,
             * instead of right at this syscall boundary as expected. */
            if (!sleep(caller, SIGNAL_WAITPID)) return -EINTR;
        }
        //kprintf("Moving on to next iteration of waitpid loop\n");
    }
    //kprintf("scheduler_waitpid: Returning changed_pid %d\n", changed_pid);
    return changed_pid;
}

static void __attribute__((noreturn)) idle_thread_fn(void) {
    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

void scheduler_create_idle_thread(void) {
    process_t * idle_proc = kmalloc(sizeof(process_t));
    if (!idle_proc) panic("idle: Failed to allocate idle process");
    memset(idle_proc, 0, sizeof(process_t));

    idle_proc->vmm = vmm_duplicate_kspace();
    if (!idle_proc->vmm) panic("idle: Failed to duplicate kspace for idle process");

    idle_proc->pid = 0;
    idle_proc->ppid = 0;
    idle_proc->nice = LONG_MAX - 1;
    idle_proc->state = SCHEDULER_STATUS_RUNABLE;

    thread_t * idle_thread = kmalloc(sizeof(thread_t));
    if (!idle_thread) panic("idle: Failed to allocate idle thread");
    memset(idle_thread, 0, sizeof(thread_t));

    idle_thread->context = kmalloc(sizeof(context_t));
    if (!idle_thread->context) panic("idle: Failed to allocate idle context");
    memset(idle_thread->context, 0, sizeof(context_t));

    idle_thread->kcontext = kmalloc(sizeof(context_t));
    if (!idle_thread->kcontext) panic("idle: Failed to allocate idle kcontext");
    memset(idle_thread->kcontext, 0, sizeof(context_t));

    idle_thread->context->simd_ctx = kmalloc(512);
    if (!idle_thread->context->simd_ctx) panic("idle: Failed to allocate idle SIMD context");
    memset(idle_thread->context->simd_ctx, 0, 512);

    idle_thread->kcontext->simd_ctx = kmalloc(512);
    if (!idle_thread->kcontext->simd_ctx) panic("idle: Failed to allocate idle kernel SIMD context");
    memset(idle_thread->kcontext->simd_ctx, 0, 512);

    idle_thread->kstack = kstackalloc(idle_proc->vmm, KERNEL_STACK_SIZE);
    if (!idle_thread->kstack) panic("idle: Failed to allocate kernel stack");

    context_info_t * ctx_info = kmalloc(sizeof(context_info_t));
    if (!ctx_info) panic("idle: Failed to allocate idle context_info");
    memset(ctx_info, 0, sizeof(context_info_t));
    ctx_info->thread = idle_thread;
    ctx_info->cs = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    ctx_info->ss = GDT_KERNEL_DATA * sizeof(gdt_entry_t);
    ctx_info->kernel_stack = idle_thread->kstack->top;

    context_info_t * kctx_info = kmalloc(sizeof(context_info_t));
    if (!kctx_info) panic("idle: Failed to allocate idle kernel context_info");
    memset(kctx_info, 0, sizeof(context_info_t));
    kctx_info->thread = idle_thread;
    kctx_info->cs = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    kctx_info->ss = GDT_KERNEL_DATA * sizeof(gdt_entry_t);
    kctx_info->kernel_stack = idle_thread->kstack->top;

    idle_thread->context->cpu_ctx.rip    = (uint64_t)idle_thread_fn;
    idle_thread->context->cpu_ctx.cs     = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    idle_thread->context->cpu_ctx.ss     = GDT_KERNEL_DATA * sizeof(gdt_entry_t);
    idle_thread->context->cpu_ctx.rflags = RFLAGS_INTERRUPT_ENABLE | RFLAGS_ONE;
    idle_thread->context->cpu_ctx.rsp    = (uint64_t)idle_thread->kstack->top;
    idle_thread->context->cpu_ctx.cr3    = (uint64_t)vmm_from_identity_map((uint64_t)idle_proc->vmm);
    idle_thread->context->cpu_ctx.ctx_info = ctx_info;

    idle_thread->kcontext->cpu_ctx.ctx_info = kctx_info;
    /* Pre-populate kcontext with idle's initial state so the trampoline
       path is taken on first scheduling, which correctly switches RSP to
       idle's own kstack.  Without this, iretq for cs=0x8 would not switch
       RSP and idle would run on the interrupted thread's kstack. */
    idle_thread->kcontext->cpu_ctx.rip    = (uint64_t)idle_thread_fn;
    idle_thread->kcontext->cpu_ctx.cs     = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    idle_thread->kcontext->cpu_ctx.rflags = RFLAGS_INTERRUPT_ENABLE | RFLAGS_ONE;
    idle_thread->kcontext->cpu_ctx.rsp    = (uint64_t)idle_thread->kstack->top;
    idle_thread->kcontext->cpu_ctx.cr3    = (uint64_t)vmm_from_identity_map((uint64_t)idle_proc->vmm);

    idle_thread->process = idle_proc;
    idle_thread->entry   = (void *)idle_thread_fn;
    idle_thread->state   = SCHEDULER_STATUS_RUNABLE;
    idle_thread->prio    = idle_proc->nice;
    idle_thread->tid     = 0;
    idle_thread->kcontext_pending = 1;
    idle_thread->kcontext_first_run = 1;

    idle_proc->threads[0]      = idle_thread;
    idle_proc->thread_count    = 1;
    idle_proc->main_thread     = idle_thread;
    idle_proc->current_thread  = idle_thread;

    g_idle_thread = idle_thread;

    scheduler_add(idle_thread);
}

thread_t * scheduler_get_next_thread() {
    thread_t * chosen_thread = NULL;
    long highest_priority = LONG_MAX;

    /* Single pass: wake sleeping threads with deliverable signals, then pick
       the highest-priority runnable thread. */
    for (scheduler_queue_t *n = sched_queue; n != NULL; n = n->next) {
        thread_t *t = n->thread;

        if (t->prio >= highest_priority)
            continue;

        /* Wake interruptible sleepers that have a deliverable signal */
        if (t->state == SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP) {
            process_t *p = GET_PROC(t);
            for (int s = 1; s < NSIG; s++) {
                if (!p->signal_queue[s]) continue;
                if (s != SIGKILL && s != SIGSTOP &&
                    (p->sig_mask & (1UL << (s - 1)))) continue;

                sigaction_t *action = p->signal_actions[s];
                uint8_t has_custom_handler = action &&
                    (uintptr_t)action->sa_handler != (uintptr_t)SIG_DFL &&
                    (uintptr_t)action->sa_handler != (uintptr_t)SIG_IGN;

                if (has_custom_handler) {
                    /* Redirecting into a handler needs a valid *user*
                       ctx, which this thread doesn't have right now (it's
                       mid-kernel-context block) — just wake it so the
                       normal signal_deliver() path in scheduler_handler
                       applies it once the thread genuinely returns to user
                       context on its own (see sleep()'s doc comment: the
                       caller sees this as an interrupted wait and unwinds
                       back up through its syscall). */
                    t->state = SCHEDULER_STATUS_RUNABLE;
                    t->woken_by_signal = 1;
                    cancel_sleep(t);
                } else {
                    /* Default or ignored disposition doesn't need a valid
                       user ctx to apply: terminate/stop just change
                       process/thread state directly, and ignore is a
                       no-op. Apply it right here instead of merely waking
                       the thread — a thread that immediately re-enters
                       another kernel-context blocking call (e.g. a plain
                       read() loop, which never does anything else in
                       between) would otherwise never actually receive the
                       signal, since that only ever happens on the
                       user-context path. */
                    signal_t *sig = process_get_signal(p);
                    if (sig) {
                        uint8_t was_sleeping =
                            (t->state == SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP);
                        if (!action || (uintptr_t)action->sa_handler == (uintptr_t)SIG_DFL)
                            process_handle_default_signal(t, sig);
                        kfree(sig);
                        /* Only a signal that actually moved this thread out
                           of INTERRUPTIBLE_SLEEP (stop/terminate) needs its
                           sleep-list bookkeeping dropped. A signal whose
                           default action is "ignore" (SIGCHLD, SIGWINCH,
                           ...) must leave the thread's original wait alone. */
                        if (was_sleeping && t->state != SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP)
                            cancel_sleep(t);
                    }
                }
                break;
            }
        }

        if (t->state == SCHEDULER_STATUS_RUNABLE) {
            highest_priority = t->prio;
            chosen_thread = t;
        }
    }

    if (!chosen_thread)
        panic("scheduler_get_next_thread: No runnable thread found (idle process missing?)");

    /* Second pass: reset winner's priority, age all others. */
    for (scheduler_queue_t *n = sched_queue; n != NULL; n = n->next) {
        thread_t *t = n->thread;
        if (t == chosen_thread)
            t->prio = GET_PROC(t)->nice;
        else if (t->prio > LONG_MIN)
            t->prio--;
    }

    current_thread = chosen_thread;
    return chosen_thread;
}

thread_t * scheduler_get_current_thread() {
    return current_thread;
}

status_t scheduler_add(thread_t * thread) {
    if (!thread) {
        panic("scheduler_add: thread is NULL");
        return FAILURE;
    }


    //If parent process's pid is -1, assign a new pid
    process_t * process = (process_t *)thread->process;
    if (process->pid == -1) {
        process->pid = scheduler_get_free_pid_locked();
    }

    //Assign a new tid to the thread
    if (thread->tid == -1) {
        thread->tid = scheduler_get_free_tid_locked();
    }

    scheduler_queue_t * new_node = kmalloc(sizeof(scheduler_queue_t));
    if (!new_node) {    
        panic("scheduler_add: Failed to allocate memory for scheduler queue node");

        return FAILURE;
    }
    new_node->thread = thread;
    new_node->next = NULL;
    //Add to the end of the sched_queue
    if (sched_queue == NULL) {
        sched_queue = new_node;
    } else {
        scheduler_queue_t * current = sched_queue;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }

    return SUCCESS;
}

status_t scheduler_remove(thread_t * thread) {
    if (!thread) {
        panic("scheduler_remove: thread is NULL");
        return FAILURE;
    }

    scheduler_queue_t * current = sched_queue;
    scheduler_queue_t * prev = NULL;
    while (current != NULL) {
        if (current->thread == thread) {
            //Remove this node
            if (prev == NULL) {
                sched_queue = current->next;
            } else {
                prev->next = current->next;
            }
            kfree(current);

            return SUCCESS;
        }
        prev = current;
        current = current->next;
    }
    panic("scheduler_remove: Thread not found in scheduler queue");
    

    return FAILURE;
}

void match() {
    kprintf("Scheduler match function called\n");
}

void dump_scheduler_status() {
    kprintf("Scheduler Queue Dump:\n");
    scheduler_queue_t * current = sched_queue;
    while (current != NULL) {
        process_t * process = GET_PROC(current->thread);
        kprintf("Thread TID: %d, Process PID: %d, State: %d, Prio: %d\n",
            current->thread->tid,
            process->pid,
            current->thread->state,
            current->thread->prio
        );
        current = current->next;
    }
}

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id, uint8_t is_kernel_ctx, uint8_t save_current) {
    //kprintf("scheduler_handler invoked on CPU %d | is_kernel_ctx: %d | save_current: %d\n", cpu_id, is_kernel_ctx, save_current);
    if (ctx == NULL) {
        panic("scheduler_handler: ctx is NULL");
    }
    
    if (ctx->ctx_info == NULL) {
        panic("scheduler_handler: ctx->ctx_info is NULL");
    }

    thread_t * ending_thread = ctx->ctx_info->thread;
    if (ending_thread) {
        if (is_kernel_ctx) {
            if (save_current) {
                context_save(ending_thread->kcontext, ctx);
                /* Kernel-mode interrupts (CS=0x8) don't push RSP/SS onto
                   the frame; store the pre-interrupt kstack RSP explicitly
                   so the trampoline can switch to it on restore.
                   Syscall-blocked contexts (CS=0x2b) already have the
                   correct RSP/SS in the frame — don't overwrite. */
                if (ctx->cs == 0x8) {
                    ending_thread->kcontext->cpu_ctx.rsp =
                        (uint64_t)ctx + sizeof(cpu_context_t) - 2 * sizeof(uint64_t);
                }
            }
            ending_thread->kcontext_pending = 1;
        } else {
            if (save_current) {
                context_save(ending_thread->context, ctx);
            }
        }
    }

    thread_t * next_thread = scheduler_get_next_thread();
    if (!next_thread) {
        panic("scheduler_handler: No next thread found");
    }
    process_t * next_process = (next_thread) ? (process_t*)next_thread->process : NULL;
    if (!next_process) {
        panic("scheduler_handler: No next process found");
    }

    //if (next_process->pid == 103) match();
    if (next_thread->state != SCHEDULER_STATUS_RUNABLE) {
        panic("scheduler_handler: Next thread is not runable");
    }

    if (next_thread->kcontext_pending) {
        next_thread->kcontext_pending = 0;
        apic_arm_lapic_timer(cpu_id, SCHEDULER_TIMESLICE_MS);
        simd_restore_context(next_thread->kcontext->simd_ctx);
        set_cpu_fs_base(next_thread->kcontext->fs_base);
        if (next_thread->kcontext->cpu_ctx.cs == 0x8) {
            /* Interrupted from kernel: trampoline rebuilds the 3-item
               iretq frame (RIP/CS/RFLAGS), switches to the saved kstack, and
               installs next_thread->kcontext->cpu_ctx.ctx_info into [gs:0x8].
               That ctx_info is next_thread's own persistent, per-thread
               record (allocated once in duplicate_thread()/thread creation
               and never reassigned), so it already matches whatever value
               next_thread pushed at its own syscall entry — no fixup needed.
               (A prior version of this code overwrote *that* struct's
               contents with the *other*, unrelated ending_thread's ctx_info
               fields, corrupting ending_thread's saved cs/ss for its next
               syscall and causing #GP on a later iretq — see git history.) */
            if (next_thread == g_idle_thread) {
                /* idle_thread_fn is a stateless `while(1) sti;hlt` loop: it
                   never needs its previously-saved registers or stack
                   contents back, only to keep running the same loop. Always
                   relaunching it fresh at its kstack top (instead of trying
                   to faithfully resume wherever it last got interrupted)
                   sidesteps a cumulative few-bytes-per-cycle kstack drift
                   that eventually corrupts its saved context after enough
                   scheduling rounds. */
                next_thread->kcontext->cpu_ctx.rip    = (uint64_t)idle_thread_fn;
                next_thread->kcontext->cpu_ctx.rflags = RFLAGS_INTERRUPT_ENABLE | RFLAGS_ONE;
                next_thread->kcontext->cpu_ctx.rsp    = (uint64_t)next_thread->kstack->top;
                kcontext_simple_launch(&next_thread->kcontext->cpu_ctx);
            } else if (next_thread->kcontext_first_run) {
                next_thread->kcontext_first_run = 0;
                kcontext_simple_launch(&next_thread->kcontext->cpu_ctx);
            } else {
                kcontext_restore_trampoline(&next_thread->kcontext->cpu_ctx);
            }
            __builtin_unreachable();
        }
        /* Syscall-blocked context (CS=0x2b): the saved context already
           has the full 5-item user frame (RSP/SS present).  Fall through
           to context_restore so iretq uses that frame normally. */
        context_restore(next_thread->kcontext, ctx);
    } else {
        context_restore(next_thread->context, ctx);
        /* Deliver a pending signal now that we have the user context in ctx */
        signal_t *sig = process_get_signal(next_thread->process);
        if (sig) {
            signal_deliver(next_thread, sig, ctx);
        }
    }
    cpu_set_context_info(ctx->ctx_info);

    //if (ending_process) {
    //    kprintf("ROBERT, ITS PISSING ME OFF from %d to %d\n", ending_process->pid, next_process->pid);
    //    kprintf("Setting cpu kstack to 0x%llx\n", (uint64_t)next_thread->kstack->top);
    //} else {
    //    kprintf("ROBERT, ITS PISSING ME OFF from NULL to %d\n", next_process->pid);
    //    kprintf("Setting cpu kstack to 0x%llx\n", (uint64_t)next_thread->kstack->top);
    //}
    
    apic_arm_lapic_timer(cpu_id, SCHEDULER_TIMESLICE_MS);
    //kprintf("scheduler_handler exiting\n");
}

process_t * scheduler_get_process_by_pid(pid_t pid) {
    scheduler_queue_t * current = sched_queue;
    while (current != NULL) {
        process_t * iterated_process = GET_PROC(current->thread);
        if (iterated_process->pid == pid) {
            return iterated_process;
        }
        current = current->next;
    }
    return NULL;
}

/* Hands every direct child of `old_ppid` off to `new_ppid` (init) so it can
   still be reaped after its real parent exits — otherwise a child's ppid
   points at a pid nobody will ever match again in scheduler_waitpid's
   comparators, and it stays a zombie (or unreaped orphan) forever. */
void scheduler_reparent_children(pid_t old_ppid, pid_t new_ppid) {
    for (scheduler_queue_t * current = sched_queue; current != NULL; current = current->next) {
        process_t * proc = GET_PROC(current->thread);
        if (proc->ppid == old_ppid) proc->ppid = new_ppid;
    }
}

/* Delivers `signo` to every process in process group `pgid`. sched_queue
   holds one node per thread, so a multi-threaded process would otherwise be
   signalled once per thread; dedupe on the process pointer instead. */
void scheduler_signal_pgrp(pid_t pgid, int signo) {
    if (pgid <= 0) return;

    process_t * signalled[64];
    int signalled_count = 0;

    for (scheduler_queue_t * current = sched_queue; current != NULL; current = current->next) {
        process_t * proc = GET_PROC(current->thread);
        if (proc->pgid != pgid) continue;

        uint8_t already = 0;
        for (int i = 0; i < signalled_count; i++) {
            if (signalled[i] == proc) { already = 1; break; }
        }
        if (already) continue;

        if (signalled_count < 64) signalled[signalled_count++] = proc;
        process_kill(proc, signo);
    }
}