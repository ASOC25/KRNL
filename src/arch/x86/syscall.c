#include <krnl/arch/x86/syscall.h>
#include <krnl/process/scheduler.h>
#include <krnl/process/process.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/mmap.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/process/signals.h>
#include <krnl/libraries/std/time.h>
#include <krnl/libraries/std/string.h>

#define SYSCALL_NUMBER(context) ((context)->rax)
#define SYSCALL_ARG0(context)   ((context)->rdi)
#define SYSCALL_ARG1(context)   ((context)->rsi)
#define SYSCALL_ARG2(context)   ((context)->rdx)
#define SYSCALL_ARG3(context)   ((context)->r10)
#define SYSCALL_ARG4(context)   ((context)->r8)
#define SYSCALL_ARG5(context)   ((context)->r9)
#define SYSCALL_RET(context)    ((context)->rax)

typedef int64_t (*syscall_handler_t)(thread_t * thread, cpu_context_t * context);

int64_t syscall_read(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    void *buf = (void *)SYSCALL_ARG1(context);
    size_t count = (size_t)SYSCALL_ARG2(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    ssize_t ret = vfs_read(desc, buf, count);
    return ret;
}

int64_t syscall_write(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    const void *buf = (const void *)SYSCALL_ARG1(context);
    size_t count = (size_t)SYSCALL_ARG2(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    ssize_t ret = vfs_write(desc, buf, count);
    return ret;
}

int64_t syscall_open(thread_t * thread, cpu_context_t * context) {
    const char *path = (const char *)SYSCALL_ARG0(context);
    int flags = (int)SYSCALL_ARG1(context);
    process_t *proc = (process_t *)thread->process;
    int slot = process_allocate_fd_slot(proc);
    if (slot < 0) {
        return -EMFILE;
    }
    vfs_file_descriptor_t newfd;
    status_t st = vfs_open(path, flags, &newfd);
    if (st != SUCCESS || !newfd.valid) {
        // Unknown reason; default to ENOENT per open(2) common case
        return -ENOENT;
    }
    // Copy into process table slot
    proc->open_files[slot] = newfd;
    proc->open_file_count++;
    return slot;
}

int64_t syscall_close(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    process_t *proc = (process_t *)thread->process;
    vfs_file_descriptor_t *desc = process_get_fd(proc, fd);
    if (!desc) {
        return -EBADF;
    }
    ssize_t r = vfs_close(desc);
    // Clear slot on success
    if (r == 0) {
        desc->mount = NULL;
        desc->position = 0;
        desc->flags = 0;
        desc->native_path = NULL;
        if (proc->open_file_count > 0) proc->open_file_count--;
    }
    return r;
}

int64_t syscall_stat(thread_t * thread, cpu_context_t * context) {
    (void)thread; // Unused
    const char *path = (const char *)SYSCALL_ARG0(context);
    vfs_stat_t *buf = (vfs_stat_t *)SYSCALL_ARG1(context);
    // Open read-only, then fstat and close
    vfs_file_descriptor_t tmp;
    status_t st = vfs_open(path, /*O_RDONLY*/ 0, &tmp);
    if (st != SUCCESS || !tmp.valid) {
        return -ENOENT;
    }
    st = vfs_fstat(&tmp, buf);
    vfs_close(&tmp);
    if (st != 0) return -EIO;
    return 0;
}

int64_t syscall_fstat(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    vfs_stat_t *buf = (vfs_stat_t *)SYSCALL_ARG1(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    status_t st = vfs_fstat(desc, buf);
    return (st == 0) ? 0 : -EIO;
}

int64_t syscall_seek(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    int64_t offset = (int64_t)SYSCALL_ARG1(context);
    int whence = (int)SYSCALL_ARG2(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    // Determine new position
    int64_t newpos = 0;
    if (whence == SEEK_SET) {
        newpos = offset;
    } else if (whence == SEEK_CUR) {
        newpos = (int64_t)desc->position + offset;
    } else if (whence == SEEK_END) {
        vfs_stat_t s;
        if (vfs_fstat(desc, &s) != 0) return -EIO;
        newpos = (int64_t)s.st_size + offset;
    } else {
        return -EINVAL;
    }
    if (newpos < 0) {
        return -EINVAL;
    }
    desc->position = (size_t)newpos;
    return newpos;
}

extern uint8_t getApicId(void);
int64_t syscall_exit(thread_t * thread, cpu_context_t * context) {
    int code = (int)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;
    process_exit(proc, code);
    __asm__ volatile("int $0x40"); // Trigger scheduler to switch process
    panic("syscall_exit: Returned from scheduler_exit_process");
    return 0;
}

int64_t syscall_ioctl(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    unsigned long request = (unsigned long)SYSCALL_ARG1(context);
    void *argp = (void *)SYSCALL_ARG2(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    ssize_t ret = vfs_ioctl(desc, request, argp);
    return ret;
}

int64_t syscall_mmap(thread_t * thread, cpu_context_t * context) {
    void * addr = (void *)SYSCALL_ARG0(context);
    uint64_t length = SYSCALL_ARG1(context);
    int prot = SYSCALL_ARG2(context);
    int flags = SYSCALL_ARG3(context);
    int fd = SYSCALL_ARG4(context);
    off_t offset = SYSCALL_ARG5(context);
    
    if ((uint64_t)addr & 0xFFF) return -EINVAL;
    if (length == 0)return -EINVAL;
    if (length & 0xFFF) length = (length + 0xFFF) & ~0xFFF;
    if (prot == 0x0) return -EINVAL;
    if (prot != PROT_NONE && (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) return -EINVAL;
    if (!((flags & MAP_SHARED) ^ (flags & MAP_PRIVATE))) return -EINVAL;

    if ((uint64_t)addr > VMM_REGION_U_SPACE_END) return -ENOMEM;
    
    //Check fd if not anonymous
    vfs_file_descriptor_t *desc = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        desc = process_get_fd((process_t *)thread->process, fd);
        if (!desc) {
            return -EBADF;
        }
    }

    //Check that the file permissions (desc->flags) match the requested protections
    if (desc) {
        if ((prot & PROT_WRITE) && !(desc->flags & O_WRONLY) && !(desc->flags & O_RDWR)) {
            return -EACCES;
        }
        if ((prot & PROT_READ) && !(desc->flags & O_RDONLY) && !(desc->flags & O_RDWR)) {
            return -EACCES;
        }
    }

    //Check unimplemented flags and protections
    if (flags & MAP_SHARED) panic("syscall_mmap: MAP_SHARED not implemented yet");
    if (prot & PROT_NONE) panic("syscall_mmap: PROT_NONE not implemented yet");
    if (!(prot & PROT_READ)) panic("syscall_mmap: PROT_READ must be set, guard pages not implemented yet");
    uint8_t vmm_flags = VMM_USER_BIT;
    if (prot & PROT_WRITE) vmm_flags |= VMM_WRITE_BIT;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_NX_BIT;

    process_t * proc = (process_t *)thread->process;
    void * mapped_addr = vmarea_mmap(
        proc,
        addr,
        length,
        prot,
        flags,
        fd,
        offset,
        0
    );
    if (mapped_addr == MAP_FAILED) {
        return -EIO;
    }
    return (int64_t)mapped_addr;
}

int64_t syscall_munmap(thread_t * thread, cpu_context_t * context) {
    void * addr = (void *)SYSCALL_ARG0(context);
    uint64_t length = SYSCALL_ARG1(context);

    if ((uint64_t)addr & 0xFFF) return -EINVAL;
    if (length == 0)return -EINVAL;
    if (length & 0xFFF) length = (length + 0xFFF) & ~0xFFF;

    process_t * proc = (process_t *)thread->process;
    status_t st = vmarea_munmap(proc, addr);
    if (st != SUCCESS) {
        return -EIO;
    }
    return 0;
}

int64_t syscall_mprotect(thread_t * thread, cpu_context_t * context) {
    void * addr = (void *)SYSCALL_ARG0(context);
    uint64_t length = SYSCALL_ARG1(context);
    int prot = SYSCALL_ARG2(context);

    if ((uint64_t)addr & 0xFFF) return -EINVAL;
    if (length == 0)return -EINVAL;
    if (length & 0xFFF) return -EINVAL;
    if (prot != PROT_NONE && (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) return -EINVAL;

    process_t * proc = (process_t *)thread->process;
    status_t st = vmarea_mprotect(proc, addr, length, prot);
    if (st != SUCCESS) {
        return -EIO;
    }
    return 0;
}

int64_t syscall_pread(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    void *buf = (void *)SYSCALL_ARG1(context);
    size_t count = (size_t)SYSCALL_ARG2(context);
    off_t offset = (off_t)SYSCALL_ARG3(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    size_t original_position = desc->position;
    desc->position = offset;
    ssize_t ret = vfs_read(desc, buf, count);
    desc->position = original_position;
    return ret;
}

int64_t syscall_tell(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) {
        return -EBADF;
    }
    return (int64_t)desc->position;
}

int64_t syscall_schedule_yield(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    uint8_t cpu_id = getApicId();
    scheduler_handler(context, cpu_id, SCHEDULER_USER_CONTEXT, 1);
    return context->rax;
}

int64_t syscall_fork(thread_t * thread, cpu_context_t * context) {
    process_t * parent_proc = (process_t *)thread->process;
    
    context_save(thread->context, context);
    process_t * child_proc = process_fork(parent_proc, thread);
    if (!child_proc) {
        return -EAGAIN;
    }
    status_t std = scheduler_add(child_proc->main_thread);
    if (std != SUCCESS) {
        process_destroy(child_proc);
        return -EAGAIN;
    }
    return (int64_t)child_proc->pid;
}

char ** duplicate_argv(char ** argv) {
    if (!argv) return NULL;
    size_t count = 0;
    while (argv[count]) count++;
    char ** new_argv = kmalloc((count + 1) * sizeof(char *));
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(argv[i]);
        new_argv[i] = kmalloc(len + 1);
        strncpy(new_argv[i], argv[i], len + 1);
    }
    new_argv[count] = NULL;
    return new_argv;
}

char ** duplicate_envp(char ** envp) {
    if (!envp) return NULL;
    size_t count = 0;
    while (envp[count]) count++;
    char ** new_envp = kmalloc((count + 1) * sizeof(char *));
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(envp[i]);
        new_envp[i] = kmalloc(len + 1);
        strncpy(new_envp[i], envp[i], len + 1);
    }
    new_envp[count] = NULL;
    return new_envp;
}

int64_t syscall_execve(thread_t * thread, cpu_context_t * context) {
    const char *filename = (const char *)SYSCALL_ARG0(context);
    const char ** argv = (const char **)SYSCALL_ARG1(context);
    const char ** envp = (const char **)SYSCALL_ARG2(context);

    //Copy filename to kernel space
    size_t fname_len = strlen(filename);
    char * kfilename = kmalloc(fname_len + 1);
    strncpy(kfilename, filename, fname_len + 1);

    //Copy argv to kernel space
    char ** kargv = duplicate_argv((char **)argv);
    //Copy envp to kernel space
    char ** kenvp = duplicate_envp((char **)envp);

    status_t st = process_execve(thread, context, kfilename, kargv, kenvp);
    if (st != SUCCESS) {
        return -EIO;
    }
    return 0; //Return from this should go to new program
}

int64_t syscall_waitpid(thread_t * thread, cpu_context_t * context) {
    int pid = (int)SYSCALL_ARG0(context);
    int * status = (int *)SYSCALL_ARG1(context);
    int options = (int)SYSCALL_ARG2(context);
    return scheduler_waitpid(thread, pid, status, options);
}

int64_t syscall_nanosleep(thread_t * thread, cpu_context_t * context) {
    struct timespec *duration = (struct timespec *)SYSCALL_ARG0(context);
    struct timespec *rem = (struct timespec *)SYSCALL_ARG1(context);
    if (!duration) {
        return -EINVAL;
    }
    if (duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000) {
        return -EINVAL;
    }

    status_t st = nanosleep(thread, (struct timespec *)duration, rem);
    if (st != SUCCESS) {
        return -EINTR;
    }
    return 0;
}

int64_t syscall_kill(thread_t * thread, cpu_context_t * context) {
    int pid = (int)SYSCALL_ARG0(context);
    int sig = (int)SYSCALL_ARG1(context);
    process_t * proc = (process_t *)thread->process;
    if (pid == 0) {
        pid = proc->pid;
    }
    process_t * target_proc = scheduler_get_process_by_pid(pid);
    if (!target_proc) {
        return -ESRCH;
    }
    status_t st = process_kill(target_proc, sig);
    if (st != SUCCESS) {
        return -ESRCH;
    }
    return 0;
}

int64_t syscall_getpid(thread_t * thread, cpu_context_t * context) {
    (void)context; // Unused
    process_t * proc = (process_t *)thread->process;
    return (int64_t)proc->pid;
}

int64_t syscall_setgid(thread_t * thread, cpu_context_t * context) {
    gid_t gid = (gid_t)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;
    proc->gid = gid;
    return 0;
}

int64_t syscall_dup(thread_t * thread, cpu_context_t * context) {
    //use vfs_file_descriptor_t * process_dup(process_t * process, int old_fd, int new_fd);
    int old_fd = (int)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;
    int new_desc = process_dup(proc, old_fd, -1);
    if (new_desc < 0) {
        return -EBADF;
    }
    return (int64_t)new_desc;
}

int64_t syscall_dup2(thread_t * thread, cpu_context_t * context) {
    int old_fd = (int)SYSCALL_ARG0(context);
    int new_fd = (int)SYSCALL_ARG1(context);
    process_t * proc = (process_t *)thread->process;
    int new_desc = process_dup(proc, old_fd, new_fd);
    if (new_desc < 0) {
        return -EBADF;
    }
    return (int64_t)new_desc;
}

int64_t syscall_chdir(thread_t * thread, cpu_context_t * context) {
    const char * path = (const char *)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;
    memset(proc->cwd.internal_path, 0, VFS_PATH_MAX);
    int len = strlen(path);
    if (len >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    strncpy(proc->cwd.internal_path, path, len);
    return 0;
}

int64_t syscall_getcwd(thread_t * thread, cpu_context_t * context) {
    char * buf = (char *)SYSCALL_ARG0(context);
    size_t size = (size_t)SYSCALL_ARG1(context);
    process_t * proc = (process_t *)thread->process;
    size_t cwd_len = strlen(proc->cwd.internal_path);
    if (size == 0 || cwd_len + 1 > size) {
        return -ERANGE;
    }
    strncpy(buf, proc->cwd.internal_path, size);
    return (int64_t)buf;
}

int64_t syscall_getppid(thread_t * thread, cpu_context_t * context) {
    (void)context; // Unused
    process_t * proc = (process_t *)thread->process;
    if (proc->parent) {
        return (int64_t)proc->parent->pid;
    } else {
        return -1;
    }
}

int64_t syscall_get_tid(thread_t * thread, cpu_context_t * context) {
    (void)context; // Unused
    return (int64_t)thread->tid;
}

int64_t syscall_thread_exit(thread_t * thread, cpu_context_t * context) {
    (void)context; // Unused
    process_thread_exit(thread);
    __asm__ volatile("int $0x40"); // Trigger scheduler to switch process
    panic("syscall_thread_exit: Returned from process_thread_exit");
    return 0;
}

int64_t syscall_futex_wait(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_futex_wait\n");
    return -ENOSYS;
}

int64_t syscall_futex_wake(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_futex_wake\n");
    return -ENOSYS;
}

int64_t syscall_clock_gettime(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    uint64_t clk_id = (uint64_t)SYSCALL_ARG0(context);
    struct timespec *tp = (struct timespec *)SYSCALL_ARG1(context);

    if (clk_id != CLOCK_MONOTONIC) {
        return -EINVAL;
    }
    if (!tp) {
        return -EFAULT;
    }

    return (int64_t)timespec_now(tp);
}

int64_t syscall_clock_getres(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    uint64_t clk_id = (uint64_t)SYSCALL_ARG0(context);
    struct timespec *res = (struct timespec *)SYSCALL_ARG1(context);

    if (clk_id != CLOCK_MONOTONIC) {
        return -EINVAL;
    }
    if (!res) {
        return -EFAULT;
    }

    return (int64_t)clock_res(res);
}

int64_t syscall_clock_settime(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_clock_settime\n");
    return -ENOSYS;
}

int64_t syscall_dir_open(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_dir_open\n");
    return -ENOSYS;
}

int64_t syscall_readdir(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_readdir\n");
    return -ENOSYS;
}

int64_t syscall_gettimeofday(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    struct timeval *tv = (struct timeval *)SYSCALL_ARG0(context);
    struct timezone *tz = (struct timezone *)SYSCALL_ARG1(context);

    if (tz != NULL) {
        return -EINVAL;
    }
    if (!tv) {
        return -EFAULT;
    }

    return (int64_t)timeval_now(tv);
}

int64_t syscall_fcntl(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_fcntl\n");
    return -ENOSYS;
}

int64_t syscall_rename(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_rename\n");
    return -ENOSYS;
}

int64_t syscall_mkdir(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_mkdir\n");
    return -ENOSYS;
}

int64_t syscall_creat(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_creat\n");
    return -ENOSYS;
}

extern void set_cpu_fs_base(uint64_t base);
extern void set_cpu_gs_base(uint64_t base);
extern void set_cpu_gs_base(uint64_t addr);
extern uint64_t get_cpu_gs_base(void);
int64_t syscall_arch_prctl(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    uint64_t option = SYSCALL_ARG0(context);
    

    switch(option) {
        case ARCH_SET_CPUID:
            return -ENODEV;
        case ARCH_GET_CPUID:
            return -ENODEV;
        case ARCH_SET_FS:
            uint64_t new_fs = SYSCALL_ARG1(context);
            thread->context->fs_base = new_fs;
            set_cpu_fs_base(new_fs);
            return 0;
        case ARCH_GET_FS:
            int64_t* addr = (int64_t*)SYSCALL_ARG1(context);
            *addr = thread->context->fs_base;
            return 0;
        case ARCH_SET_GS:
            uint64_t new_gs = SYSCALL_ARG1(context);
            set_cpu_gs_base(new_gs); //I can't see where this could go wrong...
            return 0;
        case ARCH_GET_GS:
            int64_t* gaddr = (int64_t*)SYSCALL_ARG1(context);
            uint64_t gs_base = get_cpu_gs_base();
            *gaddr = gs_base;
            return 0;
        default:
            return -EINVAL;
    }
}

int64_t syscall_fchownat(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_fchownat\n");
    return -ENOSYS;
}

int64_t syscall_unlinkat(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_unlinkat\n");
    return -ENOSYS;
}

int64_t syscall_renameat(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_renameat\n");
    return -ENOSYS;
}

int64_t syscall_pselect(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_pselect\n");
    return -ENOSYS;
}

int64_t syscall_statx(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    (void)context;
    kprintf("UNIMPLEMENTED: syscall_statx\n");
    return -ENOSYS;
}

int64_t syscall_debug(thread_t * thread, cpu_context_t * context) {
    kprintf("syscall_debug invoked by thread %lu\n", thread->tid);
    (void)context;
    dump_scheduler_status();
    return 0;
}

int64_t syscall_sigret(thread_t * thread, cpu_context_t * context) {
    (void)context;
    kprintf("syscall_sigret invoked by thread %lu\n", thread->tid);
    process_sigret(thread);
    scheduler_handler(context, getApicId(), SCHEDULER_USER_CONTEXT, 0);
    return 0;
}

int64_t syscall_sigaction(thread_t * thread, cpu_context_t * context) {
    int signum = (int)SYSCALL_ARG0(context);
    const struct sigaction * act = (const struct sigaction *)SYSCALL_ARG1(context);
    struct sigaction * oldact = (struct sigaction *)SYSCALL_ARG2(context);
    process_t * proc = (process_t *)thread->process;
    
    return (process_sigaction(proc, signum, act, oldact) == SUCCESS) ? 0 : -EINVAL;
}

static syscall_handler_t handlers[SYS_COUNT] = { 
    syscall_read, //0
    syscall_write,
    syscall_open,
    syscall_close,
    syscall_stat,
    syscall_fstat, //5
    syscall_seek,
    syscall_ioctl,
    syscall_exit, //8
    syscall_mmap,
    syscall_munmap, //10
    syscall_mprotect,
    syscall_schedule_yield,
    syscall_pread, //13
    syscall_tell,
    syscall_fork, //15
    syscall_execve,
    syscall_waitpid,
    syscall_getpid,
    syscall_nanosleep,
    syscall_setgid, //20
    syscall_dup,
    syscall_dup2,
    syscall_chdir,
    syscall_getcwd,
    syscall_getppid, //25
    syscall_get_tid,
    syscall_thread_exit,
    syscall_futex_wait,
    syscall_futex_wake,
    syscall_dir_open, //30
    syscall_readdir,
    syscall_clock_settime,
    syscall_clock_gettime,
    syscall_clock_getres,
    syscall_gettimeofday, //35
    syscall_kill,
    syscall_fcntl,
    syscall_rename,
    syscall_mkdir,
    syscall_creat, //40
    syscall_arch_prctl,
    syscall_fchownat,
    syscall_unlinkat,
    syscall_renameat,
    syscall_pselect, //45
    syscall_statx,
    syscall_debug,
    syscall_kill,
    syscall_sigret, //49
    syscall_sigaction //50
};

void syscall_handler(cpu_context_t * context) {
    uint64_t syscall_number = SYSCALL_NUMBER(context);
    if (syscall_number >= SYS_COUNT) {
        SYSCALL_RET(context) = -1; // Invalid syscall number
        return;
    }

    thread_t * current_thread = context->ctx_info->thread;
    if (!current_thread) {
        SYSCALL_RET(context) = -1; // No current thread
        return;
    }

    syscall_handler_t handler = handlers[syscall_number];
    if (handler) {
        SYSCALL_RET(context) = handler(
            current_thread,
            context
        );
    } else {
        SYSCALL_RET(context) = -1; // Unimplemented syscall
    }
}