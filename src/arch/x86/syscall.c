#include <krnl/arch/x86/syscall.h>
#include <krnl/process/scheduler.h>
#include <krnl/process/process.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/mmap.h>
#include <krnl/mem/allocator.h>

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
    uint8_t cpu_id = getApicId();
    process_set_exit_code(proc, code);
    scheduler_exit_process(proc, context, cpu_id);
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
        offset
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
    scheduler_handler(context, cpu_id);
    return 0;
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
    syscall_tell
/*
    syscall_dup,
    syscall_dup2,
    syscall_nanosleep,
    syscall_getpid,
    syscall_fork,
    syscall_execve,
    syscall_exit,
    syscall_waitpid,
    syscall_kill,
    syscall_fcntl,
    syscall_chdir,
    syscall_rename,
    syscall_mkdir,
    syscall_creat,
    syscall_gettimeofday,
    syscall_getppid,
    syscall_arch_prctl,
    syscall_get_tid,
    syscall_clock_settime,
    syscall_clock_gettime,
    syscall_clock_getres,
    syscall_fchownat,
    syscall_unlinkat,
    syscall_renameat,
    syscall_pselect,
    syscall_statx,
    syscall_thread_exit,
    syscall_futex_wait,
    syscall_futex_wake,
    syscall_dir_open,
    syscall_readdir
*/
};

void syscall_handler(cpu_context_t * context) {
    uint64_t syscall_number = SYSCALL_NUMBER(context);
    if (syscall_number >= SYS_COUNT) {
        SYSCALL_RET(context) = -1; // Invalid syscall number
        return;
    }

    thread_t * current_thread = scheduler_get_current_thread();
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