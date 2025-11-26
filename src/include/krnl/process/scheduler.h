#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>

typedef enum {
    SCHEDULER_QUEUE_RUNABLE,
    SCHEDULER_QUEUE_SLEEPING,
    SCHEDULER_QUEUE_STOPPED,
    SCHEDULER_QUEUE_ZOMBIE,
} scheduler_queue_id_t;

typedef struct scheduler_queue {
    process_t * process;
    struct scheduler_queue * next;
} scheduler_queue_t;

process_t * scheduler_get_next_process();
thread_t * scheduler_get_next_thread(process_t * process);
void scheduler_handler(cpu_context_t* ctx, uint8_t cpu_id);

scheduler_queue_t * scheduler_get_process_queue(scheduler_queue_id_t queue);
status_t scheduler_add_process(process_t * process, scheduler_queue_id_t queue);
status_t scheduler_move_process(process_t * process, scheduler_queue_id_t queue);
status_t scheduler_remove_process(process_t * process);
status_t scheduler_flush_queue(scheduler_queue_id_t queue);

status_t scheduler_send_event_to_queue(scheduler_queue_id_t queue, int event);
status_t scheduler_send_event_to_process(process_t * process, int event);

#endif