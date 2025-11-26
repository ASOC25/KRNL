#include <krnl/process/scheduler.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>

scheduler_queue_t * sched_runable_queue_head = NULL;
scheduler_queue_t * sched_sleeping_queue_head = NULL;
scheduler_queue_t * sched_stopped_queue_head = NULL;
scheduler_queue_t * sched_zombie_queue_head = NULL;

process_t * current_process = NULL;

pid_t scheduler_get_free_pid() {
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
    //Round robin through runable processes, if reached end, start from beginning, if no processes, panic
    //Take into account current_process to continue from there
    if (!sched_runable_queue_head) {
        panic("scheduler_get_next_process: No runable processes");
        return NULL;
    }

    if (current_process == NULL) {
        return sched_runable_queue_head->process;
    }

    //Search current_process in the queue
    scheduler_queue_t * current = sched_runable_queue_head;
    while (current) {
        if (current->process == current_process) {
            break;
        }
        current = current->next;
    }

    if (!current) {
        panic("scheduler_get_next_process: current_process not found in runable queue");
    }

    if (current->next) {
        return current->next->process;
    } else {
        return sched_runable_queue_head->process;
    }
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
    return &process->threads[last_thread_index];
}

status_t scheduler_add_process(process_t * process, scheduler_queue_id_t queue) {
    if (!process) {
        panic("scheduler_add_process: process is NULL");
        return FAILURE;
    }

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
            return FAILURE;
    }

    process->pid = scheduler_get_free_pid();
    scheduler_queue_t * new_node = kmalloc(sizeof(scheduler_queue_t));
    if (!new_node) {
        panic("scheduler_add_process: Failed to allocate memory for scheduler queue node");
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
                return SUCCESS;
            }
            previous = current;
            current = current->next;
        }
    }

    panic("scheduler_remove_process: Process not found in any queue");
    return FAILURE;
}
status_t scheduler_flush_queue(scheduler_queue_id_t queue) {
    //Remove all processes from the specified queue
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
            panic("scheduler_flush_queue: Invalid queue type");
            return FAILURE;
    }

    scheduler_queue_t * current = *head;
    while (current != NULL) {
        scheduler_queue_t * to_free = current;
        current = current->next;
        kfree(to_free);
    }
    *head = NULL;
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

void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id){
    (void)cpu_id;

    thread_t * ending_thread = ctx->ctx_info->thread;
    if (ending_thread) {
        context_save(ending_thread->context, ctx);
    }

    process_t * next_process = scheduler_get_next_process();
    if (!next_process) {
        panic("scheduler_handler: No next process found");
    }
    thread_t * next_thread = scheduler_get_next_thread(next_process);
    if (!next_thread) {
        panic("scheduler_handler: No next thread found");
    }
    context_restore(next_thread->context, ctx);
}
