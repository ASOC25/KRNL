#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <krnl/arch/x86/cpu.h>
#include <krnl/mem/vmm.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/elf.h>
#include <krnl/mem/vmarea.h>
#include <krnl/mem/allocator.h>

#define PROCESS_STATUS_RUNABLE 0x1
#define PROCESS_STATUS_INTERRUPTIBLE_SLEEP 0x2
#define PROCESS_STATUS_UNINTERRUPTIBLE_SLEEP 0x3
#define PROCESS_STATUS_STOPPED 0x4
#define PROCESS_STATUS_ZOMBIE 0x5

#define NEW_PROCESS_STACK_SIZE 0x4000 //16KB
#define MAX_THREADS_PER_PROCESS 16
#define MAX_OPEN_FILES 32
#define INIT_PROCESS_PARENT_CODE (process_t *)0xFFFFFFFFFFFFFFFF

#define GET_THREAD_PROCESS(thread) ((process_t *)((thread)->process))

#define GET_PROC(thread) ((process_t *)((thread)->process))

//Type for pid
typedef int16_t pid_t;

typedef struct {
    cpu_context_t cpu_ctx;
    void * simd_ctx;
    uint64_t fs_base;
} context_t;

typedef struct thread_event_queue {
    int event;
    struct thread_event_queue * next;
} thread_event_queue_t;

typedef struct thread_t {
    context_t* context;
    void * entry;
    void * process;
    stack_t * kstack;
    farlands_stack_t * ustack;
    thread_event_queue_t * event_queue;
    uint64_t stack_size;
    uint8_t state;
    long prio;
} thread_t;

typedef struct process_t {
    vmm_root_t * vmm;
    vm_area_t *vm_areas;

    thread_t threads[MAX_THREADS_PER_PROCESS];
    int thread_count;
    thread_t * current_thread;
    thread_t * main_thread;

    pid_t pid;
    pid_t ppid;
    int16_t uid;
    int16_t gid;
    int exit_code;

    long nice;

    vfs_file_descriptor_t open_files[MAX_OPEN_FILES];
    int open_file_count;
    
    struct process_t * parent;
    void * binary_entry;

    char ** argv;
    char ** envp;
    struct auxv* auxv;
    uint64_t auxv_size;
} process_t;

void process_set_exit_code(process_t * process, int code);
vfs_file_descriptor_t * process_get_fd(process_t *proc, int fd);
int process_allocate_fd_slot(process_t *proc);
process_t * process_create(process_t * parent, const char * filename, const char * tty, const char ** argv, const char ** envp);
thread_t * process_create_thread(process_t * process, void * entry_point);
status_t process_init_thread_context(context_t * context, vmm_root_t* root, void * pc, void * stack_top, char ** args, thread_t * thread);
status_t process_destroy(process_t * process);

void context_save(context_t* ctx, cpu_context_t* cpu_ctx);
void context_restore(context_t* ctx, cpu_context_t* cpu_ctx);
status_t process_execve(process_t * process, const char * filename, const char ** argv, const char ** envp);
process_t * process_fork(process_t * parent, thread_t * forking_thread);
status_t process_waitpid(process_t * proc, int pid, int * status, int options);
status_t process_enqueue_event(thread_t * thread, int event);
status_t process_dequeue_event(thread_t * thread, int * out_event);
status_t process_exit(process_t * process, int code);
void process_init();
#endif