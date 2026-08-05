#include <krnl/arch/x86/syscall.h>
#include <krnl/process/scheduler.h>
#include <krnl/process/process.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/debug/debug.h>
#include <krnl/debug/perf.h>
#include <krnl/mem/mmap.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/process/signals.h>
#include <krnl/process/pipe.h>
#include <krnl/libraries/std/time.h>
#include <krnl/libraries/std/string.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/fs/x1fs/x1fs.h>
#include <krnl/drivers/ramdisk/ramdisk.h>

#define SYSCALL_NUMBER(context) ((context)->rax)
#define SYSCALL_ARG0(context)   ((context)->rdi)
#define SYSCALL_ARG1(context)   ((context)->rsi)
#define SYSCALL_ARG2(context)   ((context)->rdx)
#define SYSCALL_ARG3(context)   ((context)->r10)
#define SYSCALL_ARG4(context)   ((context)->r8)
#define SYSCALL_ARG5(context)   ((context)->r9)
#define SYSCALL_RET(context)    ((context)->rax)

typedef int64_t (*syscall_handler_t)(thread_t * thread, cpu_context_t * context);

#define AT_FDCWD      (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR  0x200
#define AT_EMPTY_PATH 0x1000

#define MAX_SYMLINK_DEPTH 40

#define R_OK 4
#define W_OK 2
#define X_OK 1

/* Collapses "." and ".." components in an absolute path, in place.
 * "/a/./b/../c" -> "/a/c". Never ascends above root. The backends
 * (e.g. x1fs) match paths as literal strings, so without this a
 * trailing "." (as produced for bare `ls`/`opendir(".")`) or a ".."
 * component never resolves to anything. */
static void normalize_path(char *path) {
    char tmp[VFS_PATH_MAX];
    size_t out_len = 0;
    size_t seg_starts[256];
    int nseg = 0;

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg_start);

        if (seg_len == 1 && seg_start[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            if (nseg > 0) {
                nseg--;
                out_len = seg_starts[nseg];
            }
            continue;
        }
        if (nseg < 256 && out_len + 1 + seg_len < VFS_PATH_MAX) {
            seg_starts[nseg++] = out_len;
            tmp[out_len++] = '/';
            memcpy(tmp + out_len, seg_start, seg_len);
            out_len += seg_len;
        }
    }
    if (out_len == 0) {
        tmp[out_len++] = '/';
    }
    tmp[out_len] = '\0';
    strcpy(path, tmp);
}

/* path is absolute + already lexically normalized. Walks it left to right;
 * whenever a prefix up to a component boundary is itself a symlink, splices
 * in its target (absolute target replaces the whole prefix; relative target
 * is relative to the symlink's own containing directory) and restarts from
 * the beginning of the (new) string. If follow_final==0, the last component
 * is left alone even if it is itself a symlink (lstat/unlink/rename/readlink/
 * O_NOFOLLOW). Bounded by MAX_SYMLINK_DEPTH -> -ELOOP. */
static int follow_symlinks(char *path, size_t sz, int follow_final) {
    int budget = MAX_SYMLINK_DEPTH;
restart:
    {
        size_t len = strlen(path);
        size_t i = 1; /* skip leading '/' */
        while (i <= len) {
            size_t comp_end = i;
            while (comp_end < len && path[comp_end] != '/') comp_end++;
            int is_last = (comp_end >= len);
            if (is_last && !follow_final) break;

            char saved = path[comp_end];
            path[comp_end] = '\0';
            vfs_stat_t st;
            status_t r = vfs_lstat_path(path, &st);
            int is_link = (r == SUCCESS) && S_ISLNK(st.st_mode);
            path[comp_end] = saved;

            if (is_link) {
                if (--budget <= 0) return -ELOOP;
                char target[VFS_PATH_MAX];
                path[comp_end] = '\0';
                ssize_t tlen = vfs_readlink_path(path, target, sizeof(target) - 1);
                path[comp_end] = saved;
                if (tlen < 0) return (int)tlen;
                target[tlen] = '\0';

                /* '/' that starts this component -- the symlink's own
                 * containing directory, for a relative target */
                size_t dir_end = i - 1;
                char newpath[VFS_PATH_MAX];
                size_t np = 0;
                if (target[0] == '/') {
                    np = strlen(target);
                    if (np >= sz) return -ENAMETOOLONG;
                    memcpy(newpath, target, np);
                } else {
                    if (dir_end >= sz) return -ENAMETOOLONG;
                    memcpy(newpath, path, dir_end);
                    np = dir_end;
                    if (np == 0 || newpath[np - 1] != '/') newpath[np++] = '/';
                    size_t tl = strlen(target);
                    if (np + tl >= sz) return -ENAMETOOLONG;
                    memcpy(newpath + np, target, tl);
                    np += tl;
                }
                size_t suffix_len = len - comp_end; /* trailing "/rest...", if any */
                if (np + suffix_len >= sz) return -ENAMETOOLONG;
                memcpy(newpath + np, path + comp_end, suffix_len);
                newpath[np + suffix_len] = '\0';

                strcpy(path, newpath);
                normalize_path(path);
                goto restart;
            }
            i = comp_end + 1;
        }
    }
    return 0;
}

/* Reconstructs the full VFS path an already-open fd refers to, from its
 * mount's mount_point plus its native_path within that mount (the inverse of
 * vfs_get_native_path). Shared by resolve_at (dirfd-relative *at() syscalls)
 * and fchdir. */
static int fd_full_path(process_t *proc, int fd, char *out, size_t sz) {
    vfs_file_descriptor_t *desc = process_get_fd(proc, fd);
    if (!desc) return -EBADF;
    if (!desc->mount || !desc->native_path) return -EBADF;

    if (strcmp(desc->mount->mount_point, "/") == 0) {
        if (strlen(desc->native_path) >= sz) return -ENAMETOOLONG;
        strcpy(out, desc->native_path);
    } else {
        size_t mlen = strlen(desc->mount->mount_point);
        size_t nlen = strlen(desc->native_path);
        if (mlen + nlen >= sz) return -ENAMETOOLONG;
        strcpy(out, desc->mount->mount_point);
        strcat(out, desc->native_path);
    }
    return 0;
}

/* Standard owner/group/other permission check against `st`, for `proc`.
 * Root (uid 0) always passes -- this kernel has no distinct real/effective/
 * saved id split yet (see syscall_setuid), so "uid" is the only identity
 * a process has. */
static int check_access(process_t *proc, vfs_stat_t *st, uint32_t want) {
    if (proc->uid == 0) return 0;
    uint32_t bits;
    if (st->st_uid == (uint32_t)(uint16_t)proc->uid) {
        bits = (st->st_mode >> 6) & 07;
    } else if (st->st_gid == (uint32_t)(uint16_t)proc->gid) {
        bits = (st->st_mode >> 3) & 07;
    } else {
        bits = st->st_mode & 07;
    }
    return ((bits & want) == want) ? 0 : -EACCES;
}

/* Search (x) permission on every directory component of `path` except the
 * last (the caller checks the final component itself, against whatever
 * access the specific operation needs). Mirrors follow_symlinks' own
 * prefix-walk since the VFS has no per-component vnode walk of its own. */
static int check_path_traversal(process_t *proc, char *path) {
    if (proc->uid == 0) return 0;
    size_t len = strlen(path);
    size_t i = 1; /* skip leading '/' */
    while (i <= len) {
        size_t comp_end = i;
        while (comp_end < len && path[comp_end] != '/') comp_end++;
        if (comp_end >= len) break; /* final component -- not ours to check */

        char saved = path[comp_end];
        path[comp_end] = '\0';
        vfs_stat_t st;
        status_t r = vfs_lstat_path(path, &st);
        int rc = (r == SUCCESS) ? check_access(proc, &st, X_OK) : 0;
        path[comp_end] = saved;

        if (r == SUCCESS && rc < 0) return rc;
        i = comp_end + 1;
    }
    return 0;
}

/* Splits an absolute, normalized path into its parent directory. "/" for a
 * top-level entry. Used by callers that need to check write permission on
 * the directory a new/removed/renamed entry lives in. */
static void parent_dir_of(const char *path, char *out, size_t sz) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t i = len;
    while (i > 0 && path[i - 1] != '/') i--;
    size_t plen = (i > 1) ? i - 1 : 1;
    if (plen >= sz) plen = sz - 1;
    memcpy(out, path, plen);
    out[plen] = '\0';
}

/* Resolves `path` against `dirfd` into `out`. Absolute paths are copied
 * through unchanged (besides normalization); relative paths are resolved
 * against AT_FDCWD (the caller's cwd) or against an arbitrary already-open
 * directory fd (via fd_full_path). Checks search (x) permission on every
 * directory component traversed along the way. */
static int resolve_at(thread_t *thread, int dirfd, const char *path,
                      char *out, size_t sz, int follow_final) {
    if (!path || !path[0]) return -ENOENT;
    process_t *proc = (process_t *)thread->process;
    if (path[0] == '/') {
        if (strlen(path) >= sz) return -ENAMETOOLONG;
        strncpy(out, path, sz);
    } else {
        char base_buf[VFS_PATH_MAX];
        const char *base;
        if (dirfd == AT_FDCWD) {
            base = proc->cwd.internal_path;
        } else {
            int rr = fd_full_path(proc, dirfd, base_buf, sizeof(base_buf));
            if (rr < 0) return rr;
            base = base_buf;
        }
        size_t blen = strlen(base), plen = strlen(path);
        if (blen + 1 + plen + 1 > sz) return -ENAMETOOLONG;
        memcpy(out, base, blen);
        if (blen && base[blen - 1] != '/') out[blen++] = '/';
        memcpy(out + blen, path, plen + 1);
    }
    normalize_path(out);
    int fr = follow_symlinks(out, sz, follow_final);
    if (fr < 0) return fr;
    return check_path_traversal(proc, out);
}

/* R_OK/W_OK check for open()/openat(), based on O_ACCMODE and whether the
 * target already exists. If it doesn't exist and O_CREAT is set, checks
 * W_OK on the parent directory instead (X_OK on the parent was already
 * checked by resolve_at's traversal). *existed_out reports whether the
 * target was found, so the caller can tell a fresh O_CREAT apart from
 * opening something that was already there (only the former should take
 * new ownership -- see chown_new_entry). O_PATH opens need no access check
 * at all (only path resolution rights, already covered by traversal). */
static int check_open_access(process_t *proc, const char *resolved, int flags, int *existed_out) {
    vfs_stat_t st;
    status_t r = vfs_lstat_path(resolved, &st);
    if (existed_out) *existed_out = (r == SUCCESS);
    if (flags & O_PATH) return 0;

    int want = 0;
    switch (flags & 03) {
        case O_RDONLY: want = R_OK; break;
        case O_WRONLY: want = W_OK; break;
        case O_RDWR:   want = R_OK | W_OK; break;
    }

    if (r == SUCCESS) return check_access(proc, &st, want);
    if (flags & O_CREAT) {
        char parent[VFS_PATH_MAX];
        parent_dir_of(resolved, parent, sizeof(parent));
        vfs_stat_t pst;
        if (vfs_lstat_path(parent, &pst) == SUCCESS) return check_access(proc, &pst, W_OK);
    }
    return 0; /* doesn't exist, no O_CREAT -- vfs_open() reports ENOENT itself */
}

/* W_OK on the directory containing `path` -- mkdir/unlink/rmdir/rename/
 * symlink all create or remove a directory entry, which POSIX gates on
 * write permission to the containing directory itself, not the entry
 * being touched. X_OK on the parent was already checked by resolve_at's
 * traversal, since the parent is always an ancestor of `path`. */
static int check_parent_write(process_t *proc, const char *path) {
    char parent[VFS_PATH_MAX];
    parent_dir_of(path, parent, sizeof(parent));
    vfs_stat_t st;
    if (vfs_lstat_path(parent, &st) != SUCCESS) return 0; /* let the real op report the error */
    return check_access(proc, &st, W_OK);
}

/* New files/dirs/symlinks are created as though by root (uid 0, gid 0) --
 * ext2/x1fs's create()/mkdir()/symlink() ops take no uid/gid of their own.
 * Give them real ownership immediately after a non-root creator makes them,
 * rather than threading uid/gid through every filesystem backend's create
 * path. Only call this for entries that were *just* created (see callers). */
static void chown_new_entry(process_t *proc, const char *path) {
    if (proc->uid == 0) return;
    vfs_chown(path, (uint32_t)(uint16_t)proc->uid, (uint32_t)(uint16_t)proc->gid);
}

int64_t syscall_log(thread_t*thread, cpu_context_t* ctx) {
    (void)thread; // Unused
    char * message = (char *)SYSCALL_ARG0(ctx);
    kprintf("[SYSCALL LOG] %s\n", message);
    return 0;
}

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
    uint32_t mode = (uint32_t)SYSCALL_ARG2(context);
    process_t *proc = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), !(flags & O_NOFOLLOW));
    if (rr < 0) return rr;
    int existed = 0;
    int acc = check_open_access(proc, resolved, flags, &existed);
    if (acc < 0) return acc;

    int slot = process_allocate_fd_slot(proc);
    if (slot < 0) {
        return -EMFILE;
    }
    vfs_file_descriptor_t newfd;
    status_t st = vfs_open(resolved, flags, mode, &newfd);
    if (st == ALREADY_EXISTS) return -EEXIST;
    if (st != SUCCESS || !newfd.valid) {
        return -ENOENT;
    }
    // Copy into process table slot
    proc->open_files[slot] = newfd;
    proc->open_file_count++;
    if (!existed && (flags & O_CREAT)) chown_new_entry(proc, resolved);
    return slot;
}

/* openat: like syscall_open, but resolves relative to an arbitrary directory
 * fd instead of always AT_FDCWD. Needed by any modern *at()-based caller
 * (e.g. GNU findutils' safe directory traversal) — without this, mlibc's
 * openat() had no sysdep at all and unconditionally failed. */
int64_t syscall_openat(thread_t * thread, cpu_context_t * context) {
    int dirfd = (int)SYSCALL_ARG0(context);
    const char *path = (const char *)SYSCALL_ARG1(context);
    int flags = (int)SYSCALL_ARG2(context);
    uint32_t mode = (uint32_t)SYSCALL_ARG3(context);
    process_t *proc = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), !(flags & O_NOFOLLOW));
    if (rr < 0) return rr;
    int existed = 0;
    int acc = check_open_access(proc, resolved, flags, &existed);
    if (acc < 0) return acc;

    int slot = process_allocate_fd_slot(proc);
    if (slot < 0) {
        return -EMFILE;
    }
    vfs_file_descriptor_t newfd;
    status_t st = vfs_open(resolved, flags, mode, &newfd);
    if (st == ALREADY_EXISTS) return -EEXIST;
    if (st != SUCCESS || !newfd.valid) {
        return -ENOENT;
    }
    proc->open_files[slot] = newfd;
    proc->open_file_count++;
    if (!existed && (flags & O_CREAT)) chown_new_entry(proc, resolved);
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
    const char *path = (const char *)SYSCALL_ARG0(context);
    /* mlibc calls: SYS_PATH_STAT(path, strlen(path), flags, statbuf, dirfd)
     * — dirfd is AT_FDCWD for plain stat()/lstat(), or a real directory fd
     * for fstatat(dirfd, path, ...) (mlibc's fsfd_target::fd_path case). */
    int flags = (int)SYSCALL_ARG2(context);
    vfs_stat_t *buf = (vfs_stat_t *)SYSCALL_ARG3(context);
    int dirfd = (int)SYSCALL_ARG4(context);

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), !(flags & AT_SYMLINK_NOFOLLOW));
    if (rr < 0) return rr;

    // Open read-only (O_PATH: metadata only -- must not block/connect on a
    // FIFO), then fstat and close
    vfs_file_descriptor_t tmp;
    status_t st = vfs_open(resolved, O_RDONLY | O_PATH, 0, &tmp);
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
    /* mlibc calls: SYS_FD_STAT(fd, flags, statbuf) */
    vfs_stat_t *buf = (vfs_stat_t *)SYSCALL_ARG2(context);
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
    if (desc->pipe) {
        return -ESPIPE;
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
    /* BUG-35: PROT_NONE (0x0) is valid — do not reject it here */
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
        if (desc->pipe) {
            return -ENODEV;
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
    /* BUG-36: return ENOSYS instead of panicking for unimplemented features */
    if (flags & MAP_SHARED) return -ENOSYS;
    if (prot == PROT_NONE) return -ENOSYS; /* guard pages not yet implemented */
    if (!(prot & PROT_READ)) return -ENOSYS; /* non-readable mappings not yet implemented */
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
    if (desc->pipe) {
        return -ESPIPE;
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

    PERF_BEGIN(t_total);

    context_save(thread->context, context);

    PERF_BEGIN(t_fork);
    process_t * child_proc = process_fork(parent_proc, thread);
    PERF_END(t_fork, "fork/process_fork");

    if (!child_proc) {
        return -EAGAIN;
    }

    PERF_BEGIN(t_sched);
    status_t std = scheduler_add(child_proc->main_thread);
    PERF_END(t_sched, "fork/scheduler_add");

    if (std != SUCCESS) {
        process_destroy(child_proc);
        return -EAGAIN;
    }

    PERF_END(t_total, "fork/total");
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

    char resolved_filename[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, filename, resolved_filename, sizeof(resolved_filename), 1);
    if (rr < 0) return rr;
    {
        vfs_stat_t xst;
        if (vfs_lstat_path(resolved_filename, &xst) != SUCCESS) return -ENOENT;
        int acc = check_access((process_t *)thread->process, &xst, X_OK);
        if (acc < 0) return acc;
    }

    PERF_BEGIN(t_total);

    PERF_BEGIN(t_copy);
    size_t fname_len = strlen(resolved_filename);
    char * kfilename = kmalloc(fname_len + 1);
    strncpy(kfilename, resolved_filename, fname_len + 1);
    char ** kargv = duplicate_argv((char **)argv);
    char ** kenvp = duplicate_envp((char **)envp);
    PERF_END(t_copy, "execve/copy-args");

    PERF_BEGIN(t_exec);
    status_t st = process_execve(thread, context, kfilename, kargv, kenvp);
    PERF_END(t_exec, "execve/process_execve");

    /* BUG-37: free temporary kernel copies; process_execve made its own internal copies */
    kfree(kfilename);
    if (kargv) {
        for (size_t i = 0; kargv[i] != NULL; i++) kfree(kargv[i]);
        kfree(kargv);
    }
    if (kenvp) {
        for (size_t i = 0; kenvp[i] != NULL; i++) kfree(kenvp[i]);
        kfree(kenvp);
    }

    PERF_END(t_total, "execve/total");

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

/* Unrestricted, like setgid() above -- this kernel has no distinct real/
 * effective/saved id triple (process_t has a single `uid` field), so it
 * can't enforce POSIX's "non-root may only switch to its saved/real uid"
 * rule. Good enough to let a process drop privilege for testing EACCES;
 * revisit once real multi-user/login support tracks more than one id. */
int64_t syscall_setuid(thread_t * thread, cpu_context_t * context) {
    gid_t uid = (gid_t)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;
    proc->uid = uid;
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

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;

    /* Verify the target actually exists before committing to it */
    vfs_file_descriptor_t tmp;
    if (vfs_open_dir(resolved, &tmp) != SUCCESS) {
        return -ENOENT;
    }
    vfs_close(&tmp);

    size_t len = strlen(resolved);
    memset(proc->cwd.internal_path, 0, VFS_PATH_MAX);
    strncpy(proc->cwd.internal_path, resolved, len);
    return 0;
}

/* fchdir: reconstruct the full VFS path an already-open fd refers to (its
 * mount's mount_point + its native_path within that mount — see
 * vfs_get_native_path for the inverse operation) and chdir to that. Needed
 * for mlibc's openat()/fdopendir() family, which emulate directory-relative
 * opens via fchdir() since this kernel has no real *at() syscalls — without
 * this, any dirfd-relative traversal (e.g. GNU findutils' safe directory
 * walk) fails outright. */
int64_t syscall_fchdir(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    process_t * proc = (process_t *)thread->process;

    char full_path[VFS_PATH_MAX];
    int rr = fd_full_path(proc, fd, full_path, sizeof(full_path));
    if (rr < 0) return rr;

    /* Verify it's still a valid directory before committing to it */
    vfs_file_descriptor_t tmp;
    if (vfs_open_dir(full_path, &tmp) != SUCCESS) {
        return -ENOTDIR;
    }
    vfs_close(&tmp);

    memset(proc->cwd.internal_path, 0, VFS_PATH_MAX);
    strncpy(proc->cwd.internal_path, full_path, strlen(full_path));
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
    return (int64_t)proc->ppid;
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
    int *pointer  = (int *)SYSCALL_ARG0(context);
    int  expected = (int)SYSCALL_ARG1(context);
    /* ARG2 = timeout (struct timespec *) — not yet implemented */

    if (!pointer) return -EFAULT;
    /* Atomic check: if value has already changed there is nothing to wait for */
    if (*pointer != expected) return -EAGAIN;

    /* Sleep until a futex_wake on the same address */
    if (!sleep(thread, (int64_t)(uintptr_t)pointer)) return -EINTR;
    return 0;
}

int64_t syscall_futex_wake(thread_t * thread, cpu_context_t * context) {
    (void)thread;
    int *pointer = (int *)SYSCALL_ARG0(context);
    /* ARG1 = max threads to wake — wake all for simplicity */

    if (!pointer) return -EFAULT;
    wakeup((int64_t)(uintptr_t)pointer);
    return 0;
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
    const char *path = (const char *)SYSCALL_ARG0(context);
    process_t *proc = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;

    int slot = process_allocate_fd_slot(proc);
    if (slot < 0) return -EMFILE;

    vfs_file_descriptor_t newfd;
    status_t st = vfs_open_dir(resolved, &newfd);
    if (st != SUCCESS) return -ENOENT;

    proc->open_files[slot] = newfd;
    proc->open_file_count++;
    return slot;
}

int64_t syscall_readdir(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    void *buf = (void *)SYSCALL_ARG1(context);
    size_t count = (size_t)SYSCALL_ARG2(context);
    vfs_file_descriptor_t *desc = process_get_fd((process_t *)thread->process, fd);
    if (!desc) return -EBADF;
    return vfs_readdir(desc, buf, count);
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

/* fcntl constants from mlibc abi-bits/fcntl.h */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

int64_t syscall_fcntl(thread_t * thread, cpu_context_t * context) {
    int fd = (int)SYSCALL_ARG0(context);
    int request = (int)SYSCALL_ARG1(context);
    uint64_t arg = SYSCALL_ARG2(context);
    process_t *proc = (process_t *)thread->process;
    vfs_file_descriptor_t *desc = process_get_fd(proc, fd);
    if (!desc) return -EBADF;

    switch (request) {
        case F_DUPFD: {
            int new_fd = process_dup(proc, fd, -1);
            return (new_fd < 0) ? -EBADF : new_fd;
        }
        case F_GETFD:
            return (desc->flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
        case F_SETFD:
            if (arg & FD_CLOEXEC) desc->flags |= O_CLOEXEC;
            else                  desc->flags &= ~O_CLOEXEC;
            return 0;
        case F_GETFL:
            return desc->flags & ~O_CLOEXEC;
        case F_SETFL:
            /* Only allow changing O_NONBLOCK and O_APPEND */
            desc->flags = (desc->flags & ~(O_NONBLOCK | O_APPEND))
                        | ((int)arg & (O_NONBLOCK | O_APPEND));
            return 0;
        default:
            return -EINVAL;
    }
}

int64_t syscall_rename(thread_t * thread, cpu_context_t * context) {
    const char *oldpath = (const char *)SYSCALL_ARG0(context);
    const char *newpath = (const char *)SYSCALL_ARG1(context);
    process_t *proc = (process_t *)thread->process;

    char old_res[VFS_PATH_MAX], new_res[VFS_PATH_MAX];
    int r = resolve_at(thread, AT_FDCWD, oldpath, old_res, sizeof(old_res), 0);
    if (r < 0) return r;
    r = resolve_at(thread, AT_FDCWD, newpath, new_res, sizeof(new_res), 0);
    if (r < 0) return r;
    r = check_parent_write(proc, old_res);
    if (r < 0) return r;
    r = check_parent_write(proc, new_res);
    if (r < 0) return r;

    status_t st = vfs_rename(old_res, new_res);
    if (st == NOT_FOUND)      return -ENOENT;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    return (st == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_mkdir(thread_t * thread, cpu_context_t * context) {
    const char *path = (const char *)SYSCALL_ARG0(context);
    uint32_t mode    = (uint32_t)SYSCALL_ARG1(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;
    rr = check_parent_write(proc, resolved);
    if (rr < 0) return rr;

    status_t st = vfs_mkdir(resolved, mode ? mode : 0755);
    if (st == ALREADY_EXISTS)  return -EEXIST;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == SUCCESS) chown_new_entry(proc, resolved);
    return (st == SUCCESS) ? 0 : -EIO;
}

/* mkfifoat: creates a FIFO node at `path` resolved against `dirfd`. Only the
 * on-disk node is created here -- actually connecting readers and writers
 * happens later, in vfs_open()'s S_ISFIFO special case. */
int64_t syscall_mkfifoat(thread_t * thread, cpu_context_t * context) {
    int dirfd        = (int)SYSCALL_ARG0(context);
    const char *path = (const char *)SYSCALL_ARG1(context);
    uint32_t mode    = (uint32_t)SYSCALL_ARG2(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;
    rr = check_parent_write(proc, resolved);
    if (rr < 0) return rr;

    status_t st = vfs_mkfifo(resolved, mode ? (mode & 07777) : 0644);
    if (st == ALREADY_EXISTS)  return -EEXIST;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == SUCCESS) chown_new_entry(proc, resolved);
    return (st == SUCCESS) ? 0 : -EIO;
}

/* mount(source, target, fstype, flags, data): `source`/`flags`/`data` are
 * currently unused -- matches real Linux, which ignores `source` for
 * pseudo-filesystems too. "tmpfs" is the only fstype implemented so far,
 * backed by a fresh, deviceless x1fs instance (x1fs_create_tmpfs()); real
 * block devices aren't nameable from userspace yet (no /dev block-device-
 * node infrastructure exists), so mounting an actual disk isn't supported
 * here -- that needs its own follow-up once such device nodes exist. */
int64_t syscall_mount(thread_t * thread, cpu_context_t * context) {
    const char *target = (const char *)SYSCALL_ARG1(context);
    const char *fstype = (const char *)SYSCALL_ARG2(context);
    process_t *proc = (process_t *)thread->process;

    if (!target || !fstype) return -EFAULT;
    if (proc->uid != 0) return -EPERM; /* mount is a privileged operation */

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, target, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;

    vfs_stat_t st;
    if (vfs_lstat_path(resolved, &st) != SUCCESS) return -ENOENT;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;

    /* Refuse an exact-path duplicate here rather than letting
     * vfs_find_mount()'s own same-length-match panic fire on the next
     * lookup through this path. */
    if (vfs_find_exact_mount(resolved)) return -EBUSY;

    if (strcmp(fstype, "tmpfs") != 0) return -ENODEV;

    device_minor_t minor;
    if (x1fs_create_tmpfs(&minor) != SUCCESS) return -ENOMEM;

    if (!vfs_new_mount_with_ops(x1fs_get_ops(), RAMDISK_DRIVER_MAJOR, minor, resolved)) {
        x1fs_destroy_tmpfs(minor);
        return -EIO;
    }
    return 0;
}

/* umount2(target, flags) -- `flags` (e.g. MNT_FORCE/MNT_DETACH) aren't
 * meaningful yet (there's no notion of a busy mount to force through), so
 * it's accepted but ignored. Refuses to unmount "/" and refuses any path
 * that isn't itself exactly a mount point (mirrors real umount(2), which
 * doesn't let you unmount a plain subdirectory). */
int64_t syscall_umount(thread_t * thread, cpu_context_t * context) {
    const char *target = (const char *)SYSCALL_ARG0(context);
    process_t *proc = (process_t *)thread->process;

    if (!target) return -EFAULT;
    if (proc->uid != 0) return -EPERM;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, target, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;

    if (strcmp(resolved, "/") == 0) return -EINVAL;

    vfs_mount_t *m = vfs_find_exact_mount(resolved);
    if (!m) return -EINVAL;

    uint8_t was_tmpfs = (m->ops == x1fs_get_ops() && m->minor != 0);
    device_minor_t minor = m->minor;

    if (vfs_remove_mount(resolved) != SUCCESS) return -EINVAL;

    if (was_tmpfs) x1fs_destroy_tmpfs(minor);
    return 0;
}

int64_t syscall_creat(thread_t * thread, cpu_context_t * context) {
    const char *path = (const char *)SYSCALL_ARG0(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 1);
    if (rr < 0) return rr;

    int slot = process_allocate_fd_slot(proc);
    if (slot < 0) return -EMFILE;

    vfs_file_descriptor_t newfd;
    /* creat = open(path, O_WRONLY|O_CREAT|O_TRUNC) */
    status_t st = vfs_open(resolved, O_WRONLY | O_CREAT, 0644, &newfd);
    if (st == ALREADY_EXISTS) {
        /* file exists — open it for writing */
        st = vfs_open(resolved, O_WRONLY, 0, &newfd);
    }
    if (st != SUCCESS || !newfd.valid) return -EIO;
    proc->open_files[slot] = newfd;
    proc->open_file_count++;
    return slot;
}

extern void set_cpu_fs_base(uint64_t base);
extern void set_cpu_gs_base(uint64_t base);
extern void set_cpu_gs_base(uint64_t addr);
extern uint64_t get_cpu_gs_base(void);
int64_t syscall_arch_prctl(thread_t * thread, cpu_context_t * context) {
    uint64_t option = SYSCALL_ARG0(context);
    

    switch(option) {
        case ARCH_SET_CPUID:
            return -ENODEV;
        case ARCH_GET_CPUID:
            return -ENODEV;
        case ARCH_SET_FS:
            uint64_t new_fs = SYSCALL_ARG1(context);
            thread->context->fs_base = new_fs;
            if (thread->kcontext) thread->kcontext->fs_base = new_fs;
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

/* Only root may change ownership -- POSIX lets an owner give a file's group
 * away to a group they belong to, but this kernel doesn't track supplementary
 * groups, so that half of the real rule can't be checked; simplify to
 * root-only, which is always a safe subset of the real POSIX behaviour. */
int64_t syscall_fchownat(thread_t * thread, cpu_context_t * context) {
    int dirfd        = (int)SYSCALL_ARG0(context);
    const char *path = (const char *)SYSCALL_ARG1(context);
    uint32_t uid     = (uint32_t)SYSCALL_ARG2(context);
    uint32_t gid     = (uint32_t)SYSCALL_ARG3(context);
    int flags        = (int)SYSCALL_ARG4(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    if ((flags & AT_EMPTY_PATH) && (!path || !path[0])) {
        int rr = fd_full_path(proc, dirfd, resolved, sizeof(resolved));
        if (rr < 0) return rr;
    } else {
        int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), !(flags & AT_SYMLINK_NOFOLLOW));
        if (rr < 0) return rr;
    }

    if (proc->uid != 0) return -EPERM;

    vfs_stat_t st;
    if (vfs_lstat_path(resolved, &st) != SUCCESS) return -ENOENT;
    if (uid == (uint32_t)-1) uid = st.st_uid;
    if (gid == (uint32_t)-1) gid = st.st_gid;

    status_t r = vfs_chown(resolved, uid, gid);
    if (r == NOT_IMPLEMENTED) return -EROFS;
    return (r == SUCCESS) ? 0 : -EIO;
}

/* chmod may be done by root or by the file's own owner (POSIX). */
int64_t syscall_fchmodat(thread_t * thread, cpu_context_t * context) {
    int dirfd        = (int)SYSCALL_ARG0(context);
    const char *path = (const char *)SYSCALL_ARG1(context);
    uint32_t mode    = (uint32_t)SYSCALL_ARG2(context);
    int flags        = (int)SYSCALL_ARG3(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    if ((flags & AT_EMPTY_PATH) && (!path || !path[0])) {
        int rr = fd_full_path(proc, dirfd, resolved, sizeof(resolved));
        if (rr < 0) return rr;
    } else {
        int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), !(flags & AT_SYMLINK_NOFOLLOW));
        if (rr < 0) return rr;
    }

    vfs_stat_t st;
    if (vfs_lstat_path(resolved, &st) != SUCCESS) return -ENOENT;
    if (proc->uid != 0 && st.st_uid != (uint32_t)(uint16_t)proc->uid) return -EPERM;

    status_t r = vfs_chmod(resolved, mode);
    if (r == NOT_IMPLEMENTED) return -EROFS;
    return (r == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_unlinkat(thread_t * thread, cpu_context_t * context) {
    int         dirfd = (int)SYSCALL_ARG0(context);
    const char *path  = (const char *)SYSCALL_ARG1(context);
    int         flags = (int)SYSCALL_ARG2(context);
    process_t *proc   = (process_t *)thread->process;
    char resolved[VFS_PATH_MAX];
    int r = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), 0);
    if (r < 0) return r;
    r = check_parent_write(proc, resolved);
    if (r < 0) return r;
    status_t st;
    if (flags & AT_REMOVEDIR) {
        st = vfs_rmdir(resolved);
        if (st == FAILURE) return -ENOTEMPTY;
    } else {
        st = vfs_unlink(resolved);
    }
    if (st == NOT_FOUND)      return -ENOENT;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    return (st == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_symlink(thread_t * thread, cpu_context_t * context) {
    const char *target   = (const char *)SYSCALL_ARG0(context);
    const char *linkpath = (const char *)SYSCALL_ARG1(context);
    process_t *proc      = (process_t *)thread->process;
    if (!target || !linkpath) return -EINVAL;
    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, linkpath, resolved, sizeof(resolved), 0);
    if (rr < 0) return rr;
    rr = check_parent_write(proc, resolved);
    if (rr < 0) return rr;
    status_t st = vfs_symlink(target, resolved);
    if (st == ALREADY_EXISTS)  return -EEXIST;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == SUCCESS) chown_new_entry(proc, resolved);
    return (st == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_symlinkat(thread_t * thread, cpu_context_t * context) {
    const char *target   = (const char *)SYSCALL_ARG0(context);
    int dirfd             = (int)SYSCALL_ARG1(context);
    const char *linkpath  = (const char *)SYSCALL_ARG2(context);
    process_t *proc       = (process_t *)thread->process;
    if (!target || !linkpath) return -EINVAL;
    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, linkpath, resolved, sizeof(resolved), 0);
    if (rr < 0) return rr;
    rr = check_parent_write(proc, resolved);
    if (rr < 0) return rr;
    status_t st = vfs_symlink(target, resolved);
    if (st == ALREADY_EXISTS)  return -EEXIST;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == SUCCESS) chown_new_entry(proc, resolved);
    return (st == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_readlink(thread_t * thread, cpu_context_t * context) {
    const char *path = (const char *)SYSCALL_ARG0(context);
    char *buf         = (char *)SYSCALL_ARG1(context);
    size_t bufsz      = (size_t)SYSCALL_ARG2(context);
    if (!path || !buf) return -EINVAL;
    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 0);
    if (rr < 0) return rr;
    return (int64_t)vfs_readlink_path(resolved, buf, bufsz);
}

int64_t syscall_readlinkat(thread_t * thread, cpu_context_t * context) {
    int dirfd         = (int)SYSCALL_ARG0(context);
    const char *path  = (const char *)SYSCALL_ARG1(context);
    char *buf          = (char *)SYSCALL_ARG2(context);
    size_t bufsz       = (size_t)SYSCALL_ARG3(context);
    if (!path || !buf) return -EINVAL;
    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), 0);
    if (rr < 0) return rr;
    return (int64_t)vfs_readlink_path(resolved, buf, bufsz);
}

int64_t syscall_renameat(thread_t * thread, cpu_context_t * context) {
    int         olddirfd = (int)SYSCALL_ARG0(context);
    const char *oldpath  = (const char *)SYSCALL_ARG1(context);
    int         newdirfd = (int)SYSCALL_ARG2(context);
    const char *newpath  = (const char *)SYSCALL_ARG3(context);
    process_t  *proc     = (process_t *)thread->process;
    char old_res[VFS_PATH_MAX], new_res[VFS_PATH_MAX];
    int r = resolve_at(thread, olddirfd, oldpath, old_res, sizeof(old_res), 0);
    if (r < 0) return r;
    r = resolve_at(thread, newdirfd, newpath, new_res, sizeof(new_res), 0);
    if (r < 0) return r;
    r = check_parent_write(proc, old_res);
    if (r < 0) return r;
    r = check_parent_write(proc, new_res);
    if (r < 0) return r;
    status_t st = vfs_rename(old_res, new_res);
    if (st == NOT_FOUND)      return -ENOENT;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == FAILURE)         return -EXDEV;
    return (st == SUCCESS) ? 0 : -EIO;
}

#define PSELECT_FDSET_BYTES 128 /* kernel fd_set is 128 bytes (1024 bits) */

/* Readiness checks mirror each fd type's own blocking condition exactly
   (pipe_read/pipe_write's loop conditions, tty_read's), so a "ready" fd is
   guaranteed not to block the following read()/write(). Anything without
   a real backing wait channel (regular files, memdev, ...) is always
   ready — its I/O is synchronous and never blocks anyway. */
static int fd_ready_for_read(vfs_file_descriptor_t *desc) {
    if (desc->pipe) {
        pipe_t *p = desc->pipe;
        return p->count > 0 || p->writers == 0;
    }
    if (desc->mount && desc->mount->ops && desc->mount->ops->poll)
        return desc->mount->ops->poll(desc->mount->major, desc->mount->minor, desc->native_path, 0);
    return 1;
}

static int fd_ready_for_write(vfs_file_descriptor_t *desc) {
    if (desc->pipe) {
        pipe_t *p = desc->pipe;
        return (p->capacity - p->count) > 0 || p->readers == 0;
    }
    if (desc->mount && desc->mount->ops && desc->mount->ops->poll)
        return desc->mount->ops->poll(desc->mount->major, desc->mount->minor, desc->native_path, 1);
    return 1;
}

/* pselect: real fd readiness for pipes and the tty (see fd_ready_for_*),
   everything else always ready. There's no event-driven multi-fd wait in
   this kernel (sleep()/wakeup() only support one wait channel per call),
   so an unready set is polled at the scheduler's HPET tick granularity
   until something is ready or the timeout (tracked in "remaining",
   decremented by the actual elapsed time per tick) runs out. */
int64_t syscall_pselect(thread_t * thread, cpu_context_t * context) {
    int nfds = (int)SYSCALL_ARG0(context);
    uint8_t *readfds   = (uint8_t *)SYSCALL_ARG1(context);
    uint8_t *writefds  = (uint8_t *)SYSCALL_ARG2(context);
    uint8_t *exceptfds = (uint8_t *)SYSCALL_ARG3(context);
    struct timespec *timeout = (struct timespec *)SYSCALL_ARG4(context);
    /* arg5 is NOT sigmask (mlibc's sys_pselect never passes that through to
       the syscall — it's accepted but unused C++-side). It's `num_events`:
       mlibc's wrapper treats the raw syscall return as a plain 0/-errno
       code and expects the actual ready-fd count written through this
       out-pointer instead — every prior version of this function ignored
       that and just returned the count directly, leaving the caller's
       num_events uninitialized on every call. Concretely: bash/readline
       calling pselect() got back "success" with a garbage ready-count,
       then used that garbage count downstream (e.g. deciding how many
       fd_set bits to inspect), producing memory corruption whose exact
       shape depended on whatever was on the caller's stack — the "crashes
       instantly near fileno/abstract_file, differently each time" pattern
       this was hunted down from. */
    int *num_events = (int *)SYSCALL_ARG5(context);

    process_t *proc = (process_t *)thread->process;

    if (exceptfds) memset(exceptfds, 0, PSELECT_FDSET_BYTES);

    uint8_t req_read[PSELECT_FDSET_BYTES], req_write[PSELECT_FDSET_BYTES];
    memset(req_read, 0, sizeof(req_read));
    memset(req_write, 0, sizeof(req_write));
    if (readfds)  memcpy(req_read, readfds, PSELECT_FDSET_BYTES);
    if (writefds) memcpy(req_write, writefds, PSELECT_FDSET_BYTES);

    struct timespec remaining = {0, 0};
    if (timeout) remaining = *timeout;

    for (;;) {
        uint8_t out_read[PSELECT_FDSET_BYTES], out_write[PSELECT_FDSET_BYTES];
        memset(out_read, 0, sizeof(out_read));
        memset(out_write, 0, sizeof(out_write));
        int ready = 0;

        for (int fd = 0; fd < nfds; fd++) {
            int byte = fd / 8, bit = fd % 8;
            int in_read  = (req_read[byte]  >> bit) & 1;
            int in_write = (req_write[byte] >> bit) & 1;
            if (!in_read && !in_write) continue;

            vfs_file_descriptor_t *desc = process_get_fd(proc, fd);
            if (!desc || !desc->valid) continue; /* closed fd: never becomes ready */

            if (in_read && fd_ready_for_read(desc))   { out_read[byte]  |= (1 << bit); ready++; }
            if (in_write && fd_ready_for_write(desc)) { out_write[byte] |= (1 << bit); ready++; }
        }

        if (ready > 0) {
            if (readfds)  memcpy(readfds, out_read, PSELECT_FDSET_BYTES);
            if (writefds) memcpy(writefds, out_write, PSELECT_FDSET_BYTES);
            if (num_events) *num_events = ready;
            return 0;
        }

        if (timeout && remaining.tv_sec == 0 && remaining.tv_nsec == 0) {
            if (readfds)  memset(readfds, 0, PSELECT_FDSET_BYTES);
            if (writefds) memset(writefds, 0, PSELECT_FDSET_BYTES);
            if (num_events) *num_events = 0;
            return 0;
        }

        struct timespec tick = { .tv_sec = 0, .tv_nsec = HPET_SYSTEM_TASK_NANO };
        struct timespec rem;
        status_t nsres = nanosleep(thread, &tick, &rem);
        if (nsres != SUCCESS) {
            return -EINTR;
        }

        if (timeout) {
            uint64_t rem_ns = (uint64_t)remaining.tv_sec * 1000000000ULL + (uint64_t)remaining.tv_nsec;
            rem_ns = (rem_ns > HPET_SYSTEM_TASK_NANO) ? (rem_ns - HPET_SYSTEM_TASK_NANO) : 0;
            remaining.tv_sec  = (long)(rem_ns / 1000000000ULL);
            remaining.tv_nsec = (long)(rem_ns % 1000000000ULL);
        }
    }
}

int64_t syscall_statx(thread_t * thread, cpu_context_t * context) {
    /* mlibc calls: SYS_STATX(dirfd, path, flags, mask, statxbuf) */
    int dirfd             = (int)SYSCALL_ARG0(context);
    const char *path    = (const char *)SYSCALL_ARG1(context);
    int flags             = (int)SYSCALL_ARG2(context);
    vfs_statx_t *statxbuf = (vfs_statx_t *)SYSCALL_ARG4(context);

    if (!path || !statxbuf) return -EINVAL;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, dirfd, path, resolved, sizeof(resolved), !(flags & AT_SYMLINK_NOFOLLOW));
    if (rr < 0) return rr;

    // O_PATH: metadata only -- must not block/connect on a FIFO
    vfs_file_descriptor_t tmp;
    status_t st = vfs_open(resolved, O_PATH, 0, &tmp);
    if (st != SUCCESS || !tmp.valid) return -ENOENT;

    vfs_stat_t s;
    st = vfs_fstat(&tmp, &s);
    vfs_close(&tmp);
    if (st != SUCCESS) return -EIO;

    memset(statxbuf, 0, sizeof(*statxbuf));
    statxbuf->stx_mask       = STATX_BASIC_STATS;
    statxbuf->stx_blksize    = (uint32_t)s.st_blksize;
    statxbuf->stx_nlink      = (uint32_t)s.st_nlink;
    statxbuf->stx_uid        = s.st_uid;
    statxbuf->stx_gid        = s.st_gid;
    statxbuf->stx_mode       = (uint16_t)s.st_mode;
    statxbuf->stx_ino        = s.st_ino;
    statxbuf->stx_size       = (uint64_t)s.st_size;
    statxbuf->stx_blocks     = (uint64_t)s.st_blocks;
    statxbuf->stx_atime.tv_sec  = s.st_atim.tv_sec;
    statxbuf->stx_atime.tv_nsec = (uint32_t)s.st_atim.tv_nsec;
    statxbuf->stx_btime.tv_sec  = s.st_mtim.tv_sec;
    statxbuf->stx_btime.tv_nsec = (uint32_t)s.st_mtim.tv_nsec;
    statxbuf->stx_ctime.tv_sec  = s.st_ctim.tv_sec;
    statxbuf->stx_ctime.tv_nsec = (uint32_t)s.st_ctim.tv_nsec;
    statxbuf->stx_mtime.tv_sec  = s.st_mtim.tv_sec;
    statxbuf->stx_mtime.tv_nsec = (uint32_t)s.st_mtim.tv_nsec;
    statxbuf->stx_dev_major  = (uint32_t)(s.st_dev >> 8) & 0xfff;
    statxbuf->stx_dev_minor  = (uint32_t)(s.st_dev & 0xff);
    statxbuf->stx_rdev_major = (uint32_t)(s.st_rdev >> 8) & 0xfff;
    statxbuf->stx_rdev_minor = (uint32_t)(s.st_rdev & 0xff);
    return 0;
}

int64_t syscall_debug(thread_t * thread, cpu_context_t * context) {
    kprintf("syscall_debug invoked by thread %lu\n", thread->tid);
    (void)context;
    dump_scheduler_status();
    return 0;
}

int64_t syscall_sigret(thread_t * thread, cpu_context_t * ctx) {
    process_t *proc = GET_PROC(thread);

    /* ctx->rsp is the user RSP when the restorer called syscall.
       After the handler ret'd to the restorer, RSP = frame_addr (start of rt_sigframe).
       The restorer does mov eax,49; syscall so user RSP = frame_addr at syscall entry. */
    uint64_t frame_addr = ctx->rsp;
    struct rt_sigframe *kframe = to_kident(proc->vmm, (void *)frame_addr);
    if (!kframe) {
        return -EFAULT;
    }

    k_mcontext_t *mc = &kframe->uc.uc_mcontext;

    /* Restore general-purpose registers from the saved mcontext.
       Note: the syscall epilogue overwrites rcx←ctx->rip and r11←ctx->rflags
       before sysret, so the user sees rip/rflags correctly but rcx/r11
       from mcontext are clobbered — this is the expected sysret trade-off. */
    ctx->r8     = mc->gregs[MC_R8];
    ctx->r9     = mc->gregs[MC_R9];
    ctx->r10    = mc->gregs[MC_R10];
    ctx->r11    = mc->gregs[MC_R11];
    ctx->r12    = mc->gregs[MC_R12];
    ctx->r13    = mc->gregs[MC_R13];
    ctx->r14    = mc->gregs[MC_R14];
    ctx->r15    = mc->gregs[MC_R15];
    ctx->rdi    = mc->gregs[MC_RDI];
    ctx->rsi    = mc->gregs[MC_RSI];
    ctx->rbp    = mc->gregs[MC_RBP];
    ctx->rbx    = mc->gregs[MC_RBX];
    ctx->rdx    = mc->gregs[MC_RDX];
    ctx->rax    = mc->gregs[MC_RAX];
    ctx->rcx    = mc->gregs[MC_RCX];
    ctx->rsp    = mc->gregs[MC_RSP];
    ctx->rip    = mc->gregs[MC_RIP];
    ctx->rflags = mc->gregs[MC_EFL];

    /* Restore signal mask (SIGKILL/SIGSTOP cannot be unblocked) */
    proc->sig_mask = kframe->uc.uc_sigmask;
    proc->sig_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

    thread->kcontext_pending = 0;

    /* Return 0; the syscall epilogue will apply rip/rsp/rflags from ctx
       via sysret, effectively resuming where the signal interrupted. */
    return ctx->rax; /* preserve user rax from the interrupted context */
}

int64_t syscall_sigaction(thread_t * thread, cpu_context_t * context) {
    int signum = (int)SYSCALL_ARG0(context);
    const struct sigaction * act = (const struct sigaction *)SYSCALL_ARG1(context);
    struct sigaction * oldact = (struct sigaction *)SYSCALL_ARG2(context);
    process_t * proc = (process_t *)thread->process;
    
    return (process_sigaction(proc, signum, act, oldact) == SUCCESS) ? 0 : -EINVAL;
}

int64_t syscall_sigprocmask(thread_t * thread, cpu_context_t * context) {
    int             how    = (int)SYSCALL_ARG0(context);
    const sigset_t *set    = (const sigset_t *)SYSCALL_ARG1(context);
    sigset_t       *oldset = (sigset_t *)SYSCALL_ARG2(context);
    process_t *proc = (process_t *)thread->process;
    status_t st = sigprocmask(&proc->sig_mask, how, set, oldset);
    return (st == SUCCESS) ? 0 : -EINVAL;
}

int64_t syscall_rmdir(thread_t * thread, cpu_context_t * context) {
    const char *path = (const char *)SYSCALL_ARG0(context);
    process_t *proc  = (process_t *)thread->process;

    char resolved[VFS_PATH_MAX];
    int rr = resolve_at(thread, AT_FDCWD, path, resolved, sizeof(resolved), 0);
    if (rr < 0) return rr;
    rr = check_parent_write(proc, resolved);
    if (rr < 0) return rr;

    status_t st = vfs_rmdir(resolved);
    if (st == NOT_FOUND)      return -ENOENT;
    if (st == NOT_IMPLEMENTED) return -EROFS;
    if (st == FAILURE)         return -ENOTEMPTY;
    return (st == SUCCESS) ? 0 : -EIO;
}

int64_t syscall_getuid(thread_t * thread, cpu_context_t * context) {
    (void)context;
    return (int64_t)((process_t *)thread->process)->uid;
}

int64_t syscall_getgid(thread_t * thread, cpu_context_t * context) {
    (void)context;
    return (int64_t)((process_t *)thread->process)->gid;
}

int64_t syscall_geteuid(thread_t * thread, cpu_context_t * context) {
    (void)context;
    return (int64_t)((process_t *)thread->process)->uid;
}

int64_t syscall_getegid(thread_t * thread, cpu_context_t * context) {
    (void)context;
    return (int64_t)((process_t *)thread->process)->gid;
}

int64_t syscall_pipe(thread_t * thread, cpu_context_t * context) {
    int *fds = (int *)SYSCALL_ARG0(context);
    int flags = (int)SYSCALL_ARG1(context);
    if (!fds) return -EFAULT;

    process_t *proc = (process_t *)thread->process;

    int read_slot = process_allocate_fd_slot(proc);
    if (read_slot < 0) return -EMFILE;
    /* Reserve the slot immediately so the second allocate call can't reuse it */
    proc->open_files[read_slot].native_path = (char *)(uintptr_t)1;

    int write_slot = process_allocate_fd_slot(proc);
    if (write_slot < 0) {
        proc->open_files[read_slot].native_path = NULL;
        return -EMFILE;
    }

    status_t st = pipe_create(&proc->open_files[read_slot], &proc->open_files[write_slot]);
    if (st != SUCCESS) {
        proc->open_files[read_slot].native_path = NULL;
        return -ENFILE;
    }
    proc->open_files[read_slot].flags  |= (flags & (O_NONBLOCK | O_CLOEXEC));
    proc->open_files[write_slot].flags |= (flags & (O_NONBLOCK | O_CLOEXEC));
    proc->open_file_count += 2;

    fds[0] = read_slot;
    fds[1] = write_slot;
    return 0;
}

int64_t syscall_setpgid(thread_t * thread, cpu_context_t * context) {
    pid_t pid  = (pid_t)(int)SYSCALL_ARG0(context);
    pid_t pgid = (pid_t)(int)SYSCALL_ARG1(context);
    process_t *proc = (process_t *)thread->process;

    if (pgid < 0) return -EINVAL;

    process_t *target = (pid == 0) ? proc : scheduler_get_process_by_pid(pid);
    if (!target) return -ESRCH;

    /* pgid == 0 means "make it its own group leader". */
    target->pgid = (pgid == 0) ? target->pid : pgid;
    return 0;
}

int64_t syscall_getpgid(thread_t * thread, cpu_context_t * context) {
    pid_t pid = (pid_t)(int)SYSCALL_ARG0(context);
    process_t *proc = (process_t *)thread->process;

    process_t *target = (pid == 0) ? proc : scheduler_get_process_by_pid(pid);
    if (!target) return -ESRCH;

    return (int64_t)target->pgid;
}

int64_t syscall_setsid(thread_t * thread, cpu_context_t * context) {
    (void)context;
    process_t *proc = (process_t *)thread->process;

    /* POSIX: a session leader must not already be a process-group leader. */
    if (proc->pgid == proc->pid) return -EPERM;

    proc->sid  = proc->pid;
    proc->pgid = proc->pid;
    return (int64_t)proc->pid;
}

int64_t syscall_getsid(thread_t * thread, cpu_context_t * context) {
    pid_t pid = (pid_t)(int)SYSCALL_ARG0(context);
    process_t *proc = (process_t *)thread->process;

    process_t *target = (pid == 0) ? proc : scheduler_get_process_by_pid(pid);
    if (!target) return -ESRCH;

    return (int64_t)target->sid;
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
    syscall_sigaction, //50
    syscall_sigprocmask,
    syscall_rmdir,
    syscall_log, //53
    syscall_getuid,  //54
    syscall_getgid,  //55
    syscall_geteuid, //56
    syscall_getegid, //57
    syscall_pipe,    //58
    syscall_setpgid, //59
    syscall_getpgid, //60
    syscall_setsid,  //61
    syscall_getsid,  //62
    syscall_fchdir,  //63
    syscall_openat,  //64
    syscall_symlink,     //65
    syscall_symlinkat,   //66
    syscall_readlink,    //67
    syscall_readlinkat,  //68
    syscall_fchmodat,    //69
    syscall_setuid,      //70
    syscall_mkfifoat,    //71
    syscall_mount,       //72
    syscall_umount,      //73
};

void syscall_handler(cpu_context_t * context) {
    //kprintf("syscall_handler: syscall number %lu\n", SYSCALL_NUMBER(context));
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

    /* Deliver a pending signal before returning to userspace.
     *
     * scheduler_get_next_thread() can wake an INTERRUPTIBLE_SLEEP thread that
     * has a deliverable signal with a custom handler *without* dequeuing it
     * (it only unblocks the sleep -- it can't build a handler frame there
     * since it doesn't have a valid *user* ctx to redirect, only whatever
     * kernel-context state the thread was sleeping in). The signal is left
     * queued on the assumption the thread will soon reach a point where a
     * real user ctx is available and the normal preemption-driven delivery
     * in scheduler_handler() (context_restore + process_get_signal +
     * signal_deliver) applies it.
     *
     * That assumption breaks for a thread whose usermode window between
     * syscalls is too short to ever coincide with a timer tick -- e.g. a
     * blocking read() that keeps returning -EINTR and getting retried
     * immediately (exactly what an interactive readline loop does). Such a
     * thread can cycle through kernel-context sleep/wake indefinitely
     * without ever being *caught* in usermode by the timer, so the signal
     * never gets dequeued: scheduler_get_next_thread() sees the same still-
     * queued signal on every pass and re-wakes the thread over and over,
     * an infinite spurious-EINTR loop that never lets the handler run.
     *
     * Checking here closes that gap: it runs on every syscall return
     * (not just preemption), which the retry loop above hits constantly
     * regardless of how brief its usermode window is. process_get_signal()
     * dequeues at most once, so this is safe to run alongside the
     * scheduler_handler() path -- whichever gets there first consumes it. */
    process_t *current_proc = GET_PROC(current_thread);
    signal_t *pending_sig = process_get_signal(current_proc);
    if (pending_sig) {
        signal_deliver(current_thread, pending_sig, context);
    }
}