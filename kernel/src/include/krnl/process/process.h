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

#define NEW_PROCESS_STACK_SIZE 0x800000 //8MB
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

typedef struct thread_t {
    context_t* context;
    context_t* kcontext;
    uint8_t kcontext_pending;
    uint8_t kcontext_first_run;
    void * entry;
    void * process;
    stack_t * kstack;
    stack_t * ustack;
    uint64_t stack_size;
    uint8_t state;
    long prio;
    pid_t tid;
    /* Set by scheduler_get_next_thread() when it wakes this thread early
       because a deliverable signal is pending, as opposed to a normal
       wakeup() call for the condition it was sleeping on. sleep() clears it
       before blocking and reports it back to the caller so blocking
       syscalls (waitpid, read on a tty/pipe, futex_wait, ...) can return
       -EINTR promptly instead of silently going back to sleep. */
    uint8_t woken_by_signal;
} thread_t;

typedef struct proc_symtab {
    Elf64_Sym * syms;
    char      * strtab;
    uint64_t    count;
    uint64_t    load_base;   /* 0 for ET_EXEC, actual load addr for PIE/SO */
    struct proc_symtab * next;
} proc_symtab_t;

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
    pid_t pgid;
    pid_t sid;
    gid_t uid;
    gid_t gid;
    sigset_t sig_mask;  /* blocked signal mask */
    int exit_code;
    int state;
    long nice;

    vfs_file_descriptor_t open_files[MAX_OPEN_FILES];
    int open_file_count;

    void * binary_entry;

    char ** argv;
    char ** envp;
    struct auxv* auxv;
    uint64_t auxv_size;

    struct proc_symtab * symtab_list;  /* linked list: main exe + shared libs */
    uint64_t r_debug_va;               /* VA of _r_debug in process (set by loader) */
    uint8_t  shlib_syms_loaded;        /* lazy-load flag */
} process_t;

vfs_file_descriptor_t * process_get_fd(process_t *proc, int fd);
int process_allocate_fd_slot(process_t *proc);

void context_save(context_t* ctx, cpu_context_t* cpu_ctx);
void context_restore(context_t* ctx, cpu_context_t* cpu_ctx);

status_t process_execve(thread_t * thread, cpu_context_t * ctx, char * filename, char ** argv, char ** envp);
process_t * process_fork(process_t * parent, thread_t * forking_thread);
status_t process_exit(process_t * process, int code);
status_t process_destroy(process_t * process);
status_t process_thread_exit(thread_t * thread);
signal_t * process_get_signal(process_t * process);
status_t process_handle_default_signal(thread_t * thread, signal_t * signal);
status_t signal_deliver(thread_t * thread, signal_t * signal, cpu_context_t * ctx);
int process_dup(process_t * process, int old_fd, int new_fd);
thread_t * process_get_current_thread(void);
void process_init(const char * INIT_PROCESS, const char * INIT_TTY, vfs_path_t INIT_CWD, vfs_path_t INIT_ROOT);
status_t process_kill(process_t * process, int code);
status_t process_sigaction(process_t * process, int signum, const struct sigaction * act, struct sigaction * oldact);
status_t process_destroy_thread(process_t * process, thread_t * thread);
void process_load_shlib_symtabs(process_t * proc);
const char * process_resolve_symbol(process_t * proc, uint64_t addr);
#endif