#include <krnl/process/signals.h>
#include <krnl/mem/allocator.h>
#include <krnl/process/process.h>
#include <krnl/debug/debug.h>

typedef struct snode {
    int id;
    thread_t * thread;
    struct timespec duration;
    struct timespec rem;

    struct snode * next;
    struct snode * prev;
} sleeping_thread_t;

sleeping_thread_t * sleeping_threads_head = NULL;

void new(int id, thread_t * thread, struct timespec *duration, struct timespec *rem) {
    sleeping_thread_t * new_node = kmalloc(sizeof(sleeping_thread_t));
    if (!new_node) {
        panic("Failed to allocate memory for sleeping_thread_t");
        return;
    }
    new_node->id = id;
    new_node->thread = thread;
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
}

void remove(sleeping_thread_t * node) {
    if (!node) return;

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        sleeping_threads_head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    kfree(node);
}

void sleep(thread_t * thread, int condition) {
    new(condition, thread, NULL, NULL);
    thread->state = THREAD_STATE_SLEEPING;
}

void wakeup(int condition) {

}