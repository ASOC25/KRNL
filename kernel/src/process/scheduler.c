#include <krnl/process/scheduler.h>
extern void kcontext_restore_trampoline(cpu_context_t *kctx);
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
    //return if any child process such that  gid = abs(pid) has exited
    return (iterated->ppid == caller->pid && iterated->gid == (pid_t)(-pid));
}
    //return if any child process has exited
uint8_t comparator_beta(process_t * caller, process_t * iterated, int pid) {
    (void)pid;
    return (iterated->ppid == caller->pid);
}
uint8_t comparator_gamma(process_t * caller, process_t * iterated, int pid) {
    (void)pid;
    //return if any child gid=gid of caller has exited
    return (iterated->ppid == caller->pid && iterated->gid == caller->gid);
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
            sleep(caller, SIGNAL_WAITPID);
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

    idle_thread->process = idle_proc;
    idle_thread->entry   = (void *)idle_thread_fn;
    idle_thread->state   = SCHEDULER_STATUS_RUNABLE;
    idle_thread->prio    = idle_proc->nice;
    idle_thread->tid     = 0;
    idle_thread->kcontext_pending = 0;

    idle_proc->threads[0]      = idle_thread;
    idle_proc->thread_count    = 1;
    idle_proc->main_thread     = idle_thread;
    idle_proc->current_thread  = idle_thread;

    scheduler_add(idle_thread);
}

thread_t * scheduler_get_next_thread() {
    thread_t * chosen_thread = NULL;
    long highest_priority = LONG_MAX;

    /* Single pass: deliver pending signals and find the highest-priority
       runnable thread. */
    for (scheduler_queue_t *n = sched_queue; n != NULL; n = n->next) {
        thread_t *t = n->thread;

        if (t->prio >= highest_priority)
            continue;

        signal_t *sig = process_get_signal(t->process);
        if (sig && t->scontext == NULL) {
            status_t st = process_create_scontext(t, sig);
            if (st != SUCCESS)
                panic("scheduler_handler: Failed to create signal context");
            if (t->state == SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP)
                t->state = SCHEDULER_STATUS_RUNABLE;
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

void scheduler_sigreturn(cpu_context_t* ctx, thread_t * thread) {
    //if (thread->scontext) panic("scheduler_sigreturn: thread still has a signal context");
    if (thread->kcontext_pending) {
        //kprintf("scheduler_sigreturn: Restoring KERNEL context for thread %p\n", thread);
        context_restore(thread->kcontext, ctx);
        thread->kcontext_pending = 0;
    } else {
        //kprintf("scheduler_sigreturn: Restoring USER context for thread %p\n", thread);
        context_restore(thread->context, ctx);
    }

    cpu_set_context_info(ctx->ctx_info);
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
    //process_t * ending_process = NULL;
    if (ending_thread) {
        if (ending_thread->scontext && ending_thread->scontext->in_progress) {
            if (save_current) {
                //kprintf("scheduler_handler: Saving SIGNAL context for thread %p\n", ending_thread);
                context_save(ending_thread->scontext->context, ctx);
            } else {
                //kprintf("scheduler_handler: Not saving SIGNAL context for thread %p\n", ending_thread);
            }
        } else {
            if (is_kernel_ctx) {
                if (save_current) {
                    //kprintf("scheduler_handler: Saving KERNEL context for thread %p\n", ending_thread);
                    context_save(ending_thread->kcontext, ctx);
                    /* Kernel-mode interrupts don't push RSP onto the frame.
                       Store the pre-interrupt RSP explicitly so the trampoline
                       can switch to the correct kstack on restore. */
                    ending_thread->kcontext->cpu_ctx.rsp =
                        (uint64_t)ctx + sizeof(cpu_context_t) - 2 * sizeof(uint64_t);
                } else {
                    //kprintf("scheduler_handler: Not saving KERNEL context for thread %p\n", ending_thread);
                }
                ending_thread->kcontext_pending = 1;
            } else {
                if (save_current) {
                    //kprintf("scheduler_handler: Saving USER context for thread %p\n", ending_thread);
                    context_save(ending_thread->context, ctx);
                } else {
                    //kprintf("scheduler_handler: Not saving USER context for thread %p\n", ending_thread);
                }
            }
        }
       //ending_process = (process_t*)ending_thread->process;
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

    if (next_thread->scontext) {
        //kprintf("scheduler_handler: Restoring SIGNAL context for thread %p\n", next_thread);
        next_thread->scontext->in_progress = 1;
        context_restore(next_thread->scontext->context, ctx);
    } else {
        if (next_thread->kcontext_pending) {
            //kprintf("scheduler_handler: Restoring KERNEL context for thread %p\n", next_thread);
            next_thread->kcontext_pending = 0;
            apic_arm_lapic_timer(cpu_id, SCHEDULER_TIMESLICE_MS);
            kcontext_restore_trampoline(&next_thread->kcontext->cpu_ctx);
            __builtin_unreachable();
        } else {
            //kprintf("scheduler_handler: Restoring USER context for thread %p\n", next_thread);
            context_restore(next_thread->context, ctx);
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