#include <krnl/arch/x86/syscall.h>
#include <krnl/process/scheduler.h>
#include <krnl/process/process.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/debug/debug.h>

#define SYSCALL_NUMBER(context) ((context)->rax)
#define SYSCALL_ARG0(context)   ((context)->rdi)
#define SYSCALL_ARG1(context)   ((context)->rsi)
#define SYSCALL_ARG2(context)   ((context)->rdx)
#define SYSCALL_ARG3(context)   ((context)->rcx)
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
    vfs_file_descriptor_t *newfd = vfs_open(path, flags);
    if (!newfd) {
        // Unknown reason; default to ENOENT per open(2) common case
        return -ENOENT;
    }
    // Copy into process table slot
    proc->open_files[slot] = *newfd;
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
    vfs_file_descriptor_t *tmp = vfs_open(path, /*O_RDONLY*/ 0);
    if (!tmp) {
        return -ENOENT;
    }
    status_t st = vfs_fstat(tmp, buf);
    vfs_close(tmp);
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


static syscall_handler_t handlers[SYS_COUNT] = { 
    syscall_read,
    syscall_write,
    syscall_open,
    syscall_close,
    syscall_stat,
    syscall_fstat,
    syscall_seek,
    syscall_exit
/*    syscall_mmap,
    syscall_munmap,
    syscall_ioctl,
    syscall_pread,
    syscall_schedule_yield,
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