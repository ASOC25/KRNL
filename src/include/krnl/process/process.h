#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <krnl/arch/x86/cpu.h>
#include <krnl/mem/vmm.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>

#define NEW_PROCESS_STACK_SIZE 0x4000 //16KB
#define MAX_THREADS_PER_PROCESS 16
#define MAX_OPEN_FILES 32
#define FAKE_PROCESS (process_t *)0xFFFFFFFFFFFFFFFF

//Type for pid
typedef int16_t pid_t;

typedef struct {
    cpu_context_t cpu_ctx;
    void * simd_ctx;
    uint64_t fs_base;
} context_t;

typedef struct thread_t {
    context_t* context;
    void * entry;

    struct stack * stack;
    uint64_t stack_size;
} thread_t;

typedef struct process_t {
    vmm_root * vmm;
    
    thread_t threads[MAX_THREADS_PER_PROCESS];
    int thread_count;
    thread_t * current_thread;
    thread_t * main_thread;

    pid_t pid;
    pid_t ppid;
    int16_t uid;
    int16_t gid;

    int open_files[MAX_OPEN_FILES];
    int open_file_count;
    
    struct process_t * parent;

    char ** argv;
    char ** envp;
    //Auxv
} process_t;

process_t * process_create(process_t * parent, char ** argv, char ** envp);
thread_t * process_create_thread(process_t * process, void * entry_point);
status_t process_thread_context_init(context_t * context, vmm_root* root, void * pc, void * stack_top, char ** args, thread_t * thread);
status_t process_destroy(process_t * process);
status_t process_destroy_thread(thread_t * thread);

void context_save(context_t* ctx, cpu_context_t* cpu_ctx);
void context_restore(context_t* ctx, cpu_context_t* cpu_ctx);

void process_init();
#endif