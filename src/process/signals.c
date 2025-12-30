#include <krnl/process/signals.h>
#include <krnl/mem/allocator.h>
#include <krnl/process/process.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/time.h>
#include <krnl/arch/x86/hpet.h>

typedef struct snode {
    int id;
    thread_t * thread;
    struct timespec duration;
    struct timespec rem;

    uint64_t elapsed_femtos;

    struct snode * next;
    struct snode * prev;
} sleeping_thread_t;

sleeping_thread_t * sleeping_threads_head = NULL; //Sleeping for time
sleeping_thread_t * waiting_threads_head = NULL; //Sleeping indefinitely


void new(int id, thread_t * thread, struct timespec *duration, struct timespec *rem) {
    sleeping_thread_t * new_node = kmalloc(sizeof(sleeping_thread_t));
    if (!new_node) {
        panic("Failed to allocate memory for sleeping_thread_t");
        return;
    }
    new_node->id = id;
    new_node->thread = thread;
    new_node->elapsed_femtos = 0;
    if (duration) {
        new_node->duration.tv_sec = duration->tv_sec;
        new_node->duration.tv_nsec = duration->tv_nsec;
    } else {
        new_node->duration.tv_sec = 0;
        new_node->duration.tv_nsec = 0;
    }
    if (rem) {
        new_node->rem.tv_sec = rem->tv_sec;
        new_node->rem.tv_nsec = rem->tv_nsec;
    } else {
        new_node->rem.tv_sec = 0;
        new_node->rem.tv_nsec = 0;
    }
    new_node->next = NULL;
    new_node->prev = NULL;

    //If duration is not zero add to sleeping threads list
    if (new_node->duration.tv_sec != 0 || new_node->duration.tv_nsec != 0) {
        if (!sleeping_threads_head) {
            sleeping_threads_head = new_node;
        } else {
            sleeping_thread_t * current = sleeping_threads_head;
            while (current->next) {
                current = current->next;
            }
            current->next = new_node;
            new_node->prev = current;
        }
        hpet_enable();
    } else {
        if (!waiting_threads_head) {
            waiting_threads_head = new_node;
        } else {
            sleeping_thread_t * current = waiting_threads_head;
            while (current->next) {
                current = current->next;
            }
            current->next = new_node;
            new_node->prev = current;
        }
    }
}

void remove(sleeping_thread_t * node) {
    if (!node) return;

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        if (node->duration.tv_sec != 0 || node->duration.tv_nsec != 0)
            sleeping_threads_head = node->next;
        else
            waiting_threads_head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    kfree(node);
}

void update_counters() {
    //Iterate through sleeping threads and update their elapsed_femtos by HPET_SYSTEM_TASK_FEMTOS
    if (!sleeping_threads_head) {hpet_disable(); return;} //Disable HPET interrupts if no sleeping threads
    sleeping_thread_t * current = sleeping_threads_head;
    
    while (current) {
        current->elapsed_femtos += HPET_SYSTEM_TASK_FEMTOS;
        //Check if the thread has slept enough
        uint64_t total_femtos = (current->duration.tv_sec * 1000000000000ULL) + (current->duration.tv_nsec * 1000ULL);
        if (current->elapsed_femtos >= total_femtos) {
            //Wake up the thread
            current->thread->state = PROCESS_STATUS_RUNABLE;
            sleeping_thread_t * to_remove = current;
            current = current->next;
            remove(to_remove);
            kprintf("Woke up thread %d\n", to_remove->id);
        } else {
            current = current->next;
        }
    }
}

void sleep(thread_t * thread, int condition) {
    new(condition, thread, NULL, NULL);
    thread->state = PROCESS_STATUS_INTERRUPTIBLE_SLEEP;
    while (thread->state == PROCESS_STATUS_INTERRUPTIBLE_SLEEP) {
        __asm__ volatile("int $0x81");
    }
}

int generate_id(thread_t* thread) {
    int current_id = (int)(uintptr_t)thread;
    sleeping_thread_t * current = sleeping_threads_head;
    while (current) {
        if (current->id == current_id) {
            current_id++;
            current = sleeping_threads_head;;
        } else {
            current = current->next;
        }
    }
    return current_id;
}

status_t nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem) {
    new(generate_id(thread), thread, duration, rem);
    thread->state = PROCESS_STATUS_INTERRUPTIBLE_SLEEP;
    while (thread->state == PROCESS_STATUS_INTERRUPTIBLE_SLEEP) {
        __asm__ volatile("int $0x81");
    }
    return SUCCESS;
}

void wakeup(int condition) {
    //Wake up all threads waiting on the given condition
    sleeping_thread_t * current = waiting_threads_head;
    while (current) {
        if (current->id == condition) {
            current->thread->state = PROCESS_STATUS_RUNABLE;
            sleeping_thread_t * to_remove = current;
            current = current->next;
            remove(to_remove);
            kprintf("Woke up thread %d from condition %d\n", to_remove->id, condition);
        } else {
            current = current->next;
        }
    }
}