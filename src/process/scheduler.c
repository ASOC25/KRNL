#include <krnl/process/scheduler.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/libraries/lock/spinlock.h>

scheduler_queue_t * sched_runable_queue_head = NULL;
scheduler_queue_t * sched_sleeping_queue_head = NULL;
scheduler_queue_t * sched_stopped_queue_head = NULL;
scheduler_queue_t * sched_zombie_queue_head = NULL;

process_t * current_process = NULL;
thread_t * current_thread = NULL;

static spinlock_t scheduler_spinlock = SPINLOCK_INIT;

// Scheduler spinlock helpers
#define SCHEDULER_LOCK()   \
    do { \
        if (spinlock_acquire(&scheduler_spinlock) != 0) { \
            panic("SCHEDULER_LOCK: Deadlock detected in scheduler"); \
        } \
        __asm__ volatile("cli"); \
    } while (0)

#define SCHEDULER_UNLOCK() \
    do { \
        spinlock_release(&scheduler_spinlock); \
        __asm__ volatile("sti"); \
    } while (0)

// Internal helper: must be called with scheduler_spinlock already held
static pid_t scheduler_get_free_pid_locked(void) {
    static pid_t last_pid = 100; // Start from 100 to avoid reserved PIDs
    scheduler_queue_t * current;
    pid_t candidate_pid = last_pid;
    int found;
    do {
        candidate_pid++;
        if (candidate_pid < 100) {
            candidate_pid = 100; // Wrap around but stay above reserved PIDs
        }
        found = 0;

        // Check all queues for PID collision
        scheduler_queue_t * queues[] = {
            sched_runable_queue_head,
            sched_sleeping_queue_head,
            sched_stopped_queue_head,
            sched_zombie_queue_head
        };

        for (int i = 0; i < 4; i++) {
            current = queues[i];
            while (current) {
                if (current->process->pid == candidate_pid) {
                    found = 1;
                    break;
                }
                current = current->next;
            }
            if (found) {
                break;
            }
        }
    } while (found);
    last_pid = candidate_pid;
    return candidate_pid;
}

pid_t scheduler_get_free_pid() {
    pid_t pid;
    SCHEDULER_LOCK();
    pid = scheduler_get_free_pid_locked();
    SCHEDULER_UNLOCK();
    return pid;
}

scheduler_queue_t * scheduler_get_process_queue(scheduler_queue_id_t queue) {
    switch (queue) {
        case SCHEDULER_QUEUE_RUNABLE:
            return sched_runable_queue_head;
        case SCHEDULER_QUEUE_SLEEPING:
            return sched_sleeping_queue_head;
        case SCHEDULER_QUEUE_STOPPED:
            return sched_stopped_queue_head;
        case SCHEDULER_QUEUE_ZOMBIE:
            return sched_zombie_queue_head;
        default:
            return NULL;
    }
}

process_t * scheduler_get_next_process() {
    //Iterate over the runable queue and return the process with
    //the highest priority (lowest numerical value of current_nice)
    //The chosen process will have its current_nice reset to its nice value
    //All other processes in the queue will have their current_nice decreased by 1
    SCHEDULER_LOCK();
    scheduler_queue_t * current = sched_runable_queue_head;
    process_t * chosen_process = NULL;
    long highest_priority = 0x7FFFFFFF;
    while (current != NULL) {
        if (current->process->current_nice < highest_priority) {
            highest_priority = current->process->current_nice;
            chosen_process = current->process;
        }
        current = current->next;
    }
    if (chosen_process) {
        //Adjust niceness values
        current = sched_runable_queue_head;
        while (current != NULL) {
            if (current->process == chosen_process) {
                current->process->current_nice = current->process->nice;
            } else {
                if (current->process->current_nice > 0) {
                    current->process->current_nice--;
                }
            }
            current = current->next;
        }
    } else {
        panic("scheduler_get_next_process: No process found in runable queue");
        SCHEDULER_UNLOCK();
        return NULL;
    }
    current_process = chosen_process;
    SCHEDULER_UNLOCK();
    return chosen_process;
}

thread_t * scheduler_get_current_thread() {
    return current_thread;
}

thread_t * scheduler_get_next_thread(process_t * process) {
    if (!process) {
        panic("scheduler_get_next_thread: process is NULL");
        return NULL;
    }

    if (process->thread_count == 0) {
        panic("scheduler_get_next_thread: process has no threads");
        return NULL;
    }

    //Simple round-robin scheduling
    static int last_thread_index = -1;
    last_thread_index = (last_thread_index + 1) % process->thread_count;
    current_thread = &process->threads[last_thread_index];
    return &process->threads[last_thread_index];
}

status_t scheduler_add_process(process_t * process, scheduler_queue_id_t queue) {
    if (!process) {
        panic("scheduler_add_process: process is NULL");
        return FAILURE;
    }
    SCHEDULER_LOCK();

    scheduler_queue_t ** head;
    switch (queue) {
        case SCHEDULER_QUEUE_RUNABLE:
            head = &sched_runable_queue_head;
            break;
        case SCHEDULER_QUEUE_SLEEPING:
            head = &sched_sleeping_queue_head;
            break;
        case SCHEDULER_QUEUE_STOPPED:
            head = &sched_stopped_queue_head;
            break;
        case SCHEDULER_QUEUE_ZOMBIE:
            head = &sched_zombie_queue_head;
            break;
        default:
            panic("scheduler_add_process: Invalid queue type");
            SCHEDULER_UNLOCK();
            return FAILURE;
    }

    // We already hold the scheduler lock here, so call locked helper
    process->pid = scheduler_get_free_pid_locked();
    scheduler_queue_t * new_node = kmalloc(sizeof(scheduler_queue_t));
    if (!new_node) {
        panic("scheduler_add_process: Failed to allocate memory for scheduler queue node");
        SCHEDULER_UNLOCK();
        return FAILURE;
    }

    new_node->process = process;
    new_node->next = NULL;
    if (*head == NULL) {
        *head = new_node;
    } else {
        scheduler_queue_t * current = *head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }

    SCHEDULER_UNLOCK();
    return SUCCESS;
}

status_t scheduler_move_process(process_t * process, scheduler_queue_id_t queue) {
    //Move process from its current queue to new_queue
    status_t status = scheduler_remove_process(process);
    if (status != SUCCESS) {
        panic("scheduler_move_process: Failed to remove process from current queue");
        return status;
    }
    return scheduler_add_process(process, queue);
}

status_t scheduler_remove_process(process_t * process) {
    if (!process) {
        panic("scheduler_remove_process: process is NULL");
        return FAILURE;
    }
    SCHEDULER_LOCK();
    scheduler_queue_t ** queues[] = {
        &sched_runable_queue_head,
        &sched_sleeping_queue_head,
        &sched_stopped_queue_head,
        &sched_zombie_queue_head
    };

    for (int i = 0; i < 4; i++) {
        scheduler_queue_t ** head = queues[i];
        scheduler_queue_t * current = *head;
        scheduler_queue_t * previous = NULL;

        while (current != NULL) {
            if (current->process == process) {
                if (previous == NULL) {
                    *head = current->next;
                } else {
                    previous->next = current->next;
                }
                kfree(current);
                SCHEDULER_UNLOCK();
                return SUCCESS;
            }
            previous = current;
            current = current->next;
        }
    }
    SCHEDULER_UNLOCK();
    panic("scheduler_remove_process: Process not found in any queue");
    return FAILURE;
}
status_t scheduler_flush_queue(scheduler_queue_id_t queue) {
    //Remove all processes from the specified queue
    scheduler_queue_t ** head;
    SCHEDULER_LOCK();
    switch (queue) {
        case SCHEDULER_QUEUE_RUNABLE:
            head = &sched_runable_queue_head;
            break;
        case SCHEDULER_QUEUE_SLEEPING:
            head = &sched_sleeping_queue_head;
            break;
        case SCHEDULER_QUEUE_STOPPED:
            head = &sched_stopped_queue_head;
            break;
        case SCHEDULER_QUEUE_ZOMBIE:
            head = &sched_zombie_queue_head;
            break;
        default:
            panic("scheduler_flush_queue: Invalid queue type");
            SCHEDULER_UNLOCK();
            return FAILURE;
    }

    scheduler_queue_t * current = *head;
    while (current != NULL) {
        scheduler_queue_t * to_free = current;
        current = current->next;
        kfree(to_free);
    }
    *head = NULL;
    SCHEDULER_UNLOCK();
    return SUCCESS;
}

status_t scheduler_send_event_to_queue(scheduler_queue_id_t queue, int event) {
    (void)queue;
    (void)event;
    panic("scheduler_send_event_to_queue: Not implemented yet");
    return SUCCESS;
}
status_t scheduler_send_event_to_process(process_t * process, int event) {
    (void)process;
    (void)event;
    panic("scheduler_send_event_to_process: Not implemented yet");
    return SUCCESS;
}

void scheduler_exit_process(process_t * process, cpu_context_t* ctx, uint8_t cpu_id) {
    //First move the process to the zombie queue
    //Then switch to the next process by calling scheduler_handler
    status_t st = scheduler_move_process(process, SCHEDULER_QUEUE_ZOMBIE);
    if (st != SUCCESS) {
        panic("scheduler_exit_process: Failed to move process to zombie queue");
    }
    scheduler_handler(ctx, cpu_id);
}

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id){
    (void)cpu_id;
    
    thread_t * ending_thread = ctx->ctx_info->thread;
    if (ending_thread) {
        context_save(ending_thread->context, ctx);
    }
    process_t * ending_process = current_process;
    process_t * next_process = scheduler_get_next_process();
    if (!next_process) {
        panic("scheduler_handler: No next process found");
    }
    thread_t * next_thread = scheduler_get_next_thread(next_process);
    if (!next_thread) {
        panic("scheduler_handler: No next thread found");
    }

    kprintf("Scheduler switching from %d to %d\n",
        ending_process ? ending_process->pid : -1,
        next_process->pid
    );

    context_restore(next_thread->context, ctx);
}
