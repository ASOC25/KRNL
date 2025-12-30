#include <krnl/process/scheduler.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/arch/x86/apic.h>

#include <krnl/libraries/assert/assert.h>

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

pid_t scheduler_get_free_pid() {
    pid_t pid;

    pid = scheduler_get_free_pid_locked();

    return pid;
}

thread_t * scheduler_get_next_thread() {
    //Iterate over the runable queue and return the thread with
    //the highest priority (lowest numerical value of thread->prio)
    //The thread will have its prio reset to its thread->process->nice
    //All other threads in the queue will have their prio decreased by 1
    long highest_priority = 0x7FFFFFFF;
    long lowest_priority = -0x7FFFFFFF;
    scheduler_queue_t * current = sched_queue;
    thread_t * chosen_thread = NULL;
    while (current != NULL) {
        if (current->thread->prio < highest_priority && current->thread->state == SCHEDULER_STATUS_RUNABLE) {
            highest_priority = current->thread->prio;
            chosen_thread = current->thread;
        }
        current = current->next;
    }
    if (chosen_thread) {
        current = sched_queue;
        while (current != NULL) {
            if (current->thread == chosen_thread) {
                current->thread->prio = GET_PROC(current->thread)->nice;
            } else {
                if (current->thread->prio > lowest_priority) {
                    current->thread->prio--;
                }
            }
            current = current->next;
        }
    } else {
        panic("scheduler_get_next_thread: No runable threads found");
        return NULL;
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

status_t thread_send_event(thread_t * thread, int event) {
    if (!thread) {
        panic("thread_send_event: thread is NULL");
        return FAILURE;
    }
    //Add the event to the thread's event queue
    //For simplicity, we just set a flag here
    process_enqueue_event(thread, event);
    return SUCCESS;
}

status_t scheduler_send_event(int event, int who) {
    if (who <= 0) {
        // 0 and negative numbers: Send to all threads with the state equal to the absolute value of who

        scheduler_queue_t * current = sched_queue;
        while (current != NULL) {
            thread_t * thread = current->thread;
            if (thread->state == -who) {
                thread_send_event(thread, event);
                
            }
            current = current->next;
        }

        return SUCCESS;
    }
    if (who == 1) {

        // 1: Send to the current thread
        thread_t * current_thread = scheduler_get_current_thread();
        if (current_thread) {
            status_t res = thread_send_event(current_thread, event);

            return res;
        } else {
            panic("scheduler_send_event: No current thread");

            return FAILURE;
        }
    }
    if (who == 2) {

        //Send to all threads in the current process
        thread_t * current_thread = scheduler_get_current_thread();
        if (current_thread) {
            process_t * current_process = (process_t*)current_thread->process;
            if (current_process) {
                scheduler_queue_t * current = sched_queue;
                while (current != NULL) {
                    thread_t * thread = current->thread;
                    if (thread->process == current_process) {
                        thread_send_event(thread, event);
                    }
                    current = current->next;
                }

                return SUCCESS;
            }
        }
        panic("scheduler_send_event: No current thread or process");

        return FAILURE;
    }
    if (who == 3) {
        //Send to all threads

        scheduler_queue_t * current = sched_queue;
        while (current != NULL) {
            thread_t * thread = current->thread;
            thread_send_event(thread, event);
            current = current->next;
        }

        return SUCCESS;
    }
    if (who > 100) {
        // >100: Send to all threads in the process with the given PID
        pid_t target_pid = (pid_t)who;

        scheduler_queue_t * current = sched_queue;
        while (current != NULL) {
            thread_t * thread = current->thread;
            process_t * process = (process_t*)thread->process;
            if (process && process->pid == target_pid) {
                thread_send_event(thread, event);
            }
            current = current->next;
        }

        return SUCCESS;
    }
    panic("scheduler_send_event: Invalid 'who' parameter");
    return FAILURE;
}

void match() {
    kprintf("Scheduler match function called\n");
}

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id, uint8_t is_kernel_ctx) {
    if (ctx == NULL) {
        panic("scheduler_handler: ctx is NULL");
    }
    
    if (ctx->ctx_info == NULL) {
        panic("scheduler_handler: ctx->ctx_info is NULL");
    }

    thread_t * ending_thread = ctx->ctx_info->thread;
    //process_t * ending_process = NULL;
    if (ending_thread) {
        if (is_kernel_ctx) {
            context_save(ending_thread->kcontext, ctx);
            ending_thread->kcontext_pending = 1;
        } else {
            context_save(ending_thread->context, ctx);
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

    //if (next_process->pid == 101) match();

    //if (ending_process)
    //    kprintf("ROBERT, ITS PISSING ME OFF from %d to %d\n", ending_process->pid, next_process->pid);
    //else
    //    kprintf("ROBERT, ITS PISSING ME OFF from NULL to %d\n", next_process->pid);
    
    if (next_thread->kcontext_pending) {
        next_thread->kcontext_pending = 0;
        context_restore(next_thread->kcontext, ctx);
    } else {
        context_restore(next_thread->context, ctx);
    }
    
    apic_arm_lapic_timer(cpu_id, SCHEDULER_TIMESLICE_MS);

}