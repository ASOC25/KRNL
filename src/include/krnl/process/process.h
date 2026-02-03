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
#include <krnl/process/sigstructs.h>

#define NEW_PROCESS_STACK_SIZE 0x4000 //16KB
#define MAX_THREADS_PER_PROCESS 16
#define MAX_OPEN_FILES 32
#define INIT_PROCESS_PARENT_CODE (process_t *)0xFFFFFFFFFFFFFFFF

#define GET_THREAD_PROCESS(thread) ((process_t *)((thread)->process))

#define GET_PROC(thread) ((process_t *)((thread)->process))

//Type for pid
typedef int16_t pid_t;

//Type for gid
typedef int16_t gid_t;

typedef struct {
    cpu_context_t cpu_ctx;
    void * simd_ctx;
    uint64_t fs_base;
} context_t;

typedef struct sigctx {
    signal_t * signal;
    uint8_t in_progress;
    context_t* context;
    stack_t* stack;
    stack_t* kstack;
    struct sigctx * next;
} sigctx_t;

typedef struct thread_t {
    context_t* context;
    context_t* kcontext;
    sigctx_t* scontext;
    uint8_t kcontext_pending;
    void * entry;
    void * process;
    stack_t * kstack;
    stack_t * ustack;
    uint64_t stack_size;
    uint8_t state;
    long prio;
    pid_t tid;
} thread_t;

typedef struct process_t {
    vmm_root_t * vmm;
    vm_area_t *vm_areas;

    thread_t * threads[MAX_THREADS_PER_PROCESS];
    signal_t * signal_queue[NSIG];
    sigaction_t *signal_actions[NSIG];
    void * stramp_address;
    int thread_count;
    thread_t * current_thread;
    thread_t * main_thread;
    vfs_path_t cwd;
    vfs_path_t rootdir;
    pid_t pid;
    pid_t ppid;
    gid_t uid;
    gid_t gid;
    int exit_code;
    int state;
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

vfs_file_descriptor_t * process_get_fd(process_t *proc, int fd);
int process_allocate_fd_slot(process_t *proc);

void context_save(context_t* ctx, cpu_context_t* cpu_ctx);
void context_restore(context_t* ctx, cpu_context_t* cpu_ctx);

status_t process_execve(thread_t * thread, cpu_context_t * ctx, char * filename, char ** argv, char ** envp);
process_t * process_fork(process_t * parent, thread_t * forking_thread);
status_t process_exit(process_t * process, int code);
status_t process_destroy(process_t * process);
sigctx_t * process_get_scontext(thread_t * thread);
status_t process_thread_exit(thread_t * thread);
signal_t * process_get_signal(process_t * process);
status_t process_create_scontext(thread_t * thread, signal_t * signal);
int process_dup(process_t * process, int old_fd, int new_fd);
thread_t * process_get_current_thread(void);
void process_init(const char * INIT_PROCESS, const char * INIT_TTY, vfs_path_t INIT_CWD, vfs_path_t INIT_ROOT);
status_t process_kill(process_t * process, int code);
void process_sigret(thread_t * thread);
status_t process_sigaction(process_t * process, int signum, const struct sigaction * act, struct sigaction * oldact);
status_t process_destroy_thread(process_t * process, thread_t * thread);
#endif