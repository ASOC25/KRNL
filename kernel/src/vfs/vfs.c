#include <krnl/vfs/vfs.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/process/pipe.h>

vfs_mount_t * vfs_mounts = NULL;
vfs_fs_t * ops = NULL;
status_t vfs_register_fs(vfs_fs_t *new_fs) {
    if (new_fs == NULL) {
        panic("vfs_register_fs: new_fs is NULL");
    }

    //Check if filesystem with same name already exists
    vfs_fs_t *current = ops;
    while (current != NULL) {
        if (strcmp(current->name, new_fs->name) == 0) {
            return ALREADY_EXISTS;
        }
        current = current->next;
    }

    //Add new filesystem to the front of the linked list
    new_fs->next = ops;
    ops = new_fs;
    return SUCCESS;
}

status_t vfs_unregister_fs(char *fs_name) {
    if (fs_name == NULL) {
        panic("vfs_unregister_fs: fs_name is NULL");
    }

    vfs_fs_t *current = ops;
    vfs_fs_t *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->name, fs_name) == 0) {
            if (previous == NULL) {
                ops = current->next;
            } else {
                previous->next = current->next;
            }
            kfree(current);
            return SUCCESS;
        }
        previous = current;
        current = current->next;
    }
    return NOT_FOUND;
}

vfs_fs_t * detect_fs(device_major_t major, device_minor_t minor) {
    vfs_fs_t *current = ops;
    while (current != NULL) {
        if (current->detect != NULL) {
            status_t result = current->detect(major, minor);
            if (result == SUCCESS) {
                return current;
            }
        }
        current = current->next;
    }
    panic("detect_fs: No suitable filesystem found for device");
    return NULL;
}

/* Shared by vfs_new_mount() (which resolves `ops` itself via detect_fs(),
   probing the backing device for a filesystem signature) and
   vfs_new_mount_with_ops() (used by mount() for filesystems like tmpfs
   that have no backing device to probe -- the caller already knows
   exactly which vfs_fs_t to attach). */
static vfs_mount_t * vfs_new_mount_internal(vfs_fs_t *ops, device_major_t major, device_minor_t minor, const char *mount_point) {
    if (mount_point == NULL) {
        panic("vfs_new_mount: mount_point or ops is NULL");
    }
    if (strlen(mount_point) >= VFS_PATH_MAX) {
        panic("vfs_new_mount: Mount point path too long");
    }

    vfs_mount_t *new_mount = (vfs_mount_t *)kmalloc(sizeof(vfs_mount_t));
    if (!new_mount) {
        panic("vfs_new_mount: Unable to allocate memory for new mount");
    }

    new_mount->major = major;
    new_mount->minor = minor;
    new_mount->ops = ops;
    new_mount->mount_point = kmalloc(strlen(mount_point) + 1);
    if (!new_mount->mount_point) {
        panic("vfs_new_mount: Unable to allocate memory for mount point");
    }
    strcpy(new_mount->mount_point, mount_point);
    new_mount->next = vfs_mounts;
    vfs_mounts = new_mount;

    return new_mount;
}

vfs_mount_t * vfs_new_mount(device_major_t major, device_minor_t minor, const char *mount_point) {
    vfs_fs_t *ops = detect_fs(major, minor);
    return vfs_new_mount_internal(ops, major, minor, mount_point);
}

vfs_mount_t * vfs_new_mount_with_ops(vfs_fs_t *ops, device_major_t major, device_minor_t minor, const char *mount_point) {
    if (ops == NULL) {
        panic("vfs_new_mount_with_ops: ops is NULL");
    }
    return vfs_new_mount_internal(ops, major, minor, mount_point);
}

/* Exact-string match against an existing mount point -- unlike
   vfs_find_mount()'s longest-prefix match (used for ordinary path
   resolution), mount()/umount() need to know whether a path IS a mount
   point, not merely falls under one. */
vfs_mount_t * vfs_find_exact_mount(const char *path) {
    for (vfs_mount_t *m = vfs_mounts; m != NULL; m = m->next) {
        if (strcmp(m->mount_point, path) == 0) return m;
    }
    return NULL;
}

status_t vfs_remove_mount(const char *mount_point) {
    if (mount_point == NULL) {
        panic("vfs_remove_mount: mount_point is NULL");
    }
    /* BUG-48: removed duplicate NULL check that was dead code */

    vfs_mount_t *current = vfs_mounts;
    vfs_mount_t *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->mount_point, mount_point) == 0) {
            if (previous == NULL) {
                vfs_mounts = current->next;
            } else {
                previous->next = current->next;
            }
            if (current->mount_point == NULL) {
                panic("vfs_remove_mount: current->mount_point is NULL");
            }
            kfree(current->mount_point);
            kfree(current);
            return SUCCESS;
        }
        previous = current;
        current = current->next;
    }

    return NOT_FOUND;
}

struct find_mount_candidate {
    vfs_mount_t *mount;
    size_t mount_point_len;
};

//Find the most specific mount point for the given path
//If multiple mount points match, return the one with the longest mount point
vfs_mount_t * vfs_find_mount(const char *path) {
    if (path == NULL) {
        panic("vfs_find_mount: path is NULL");
    }
    vfs_mount_t *current = vfs_mounts;
    struct find_mount_candidate best_candidate = {NULL, 0};
    while (current != NULL) {
        size_t mount_point_len = strlen(current->mount_point);
        if (strncmp(path, current->mount_point, mount_point_len) == 0) {
            //Check if this is a better candidate
            if (mount_point_len > best_candidate.mount_point_len) {
                best_candidate.mount = current;
                best_candidate.mount_point_len = mount_point_len;
            } else if (mount_point_len == best_candidate.mount_point_len) {
                panic("vfs_find_mount: Multiple mount points with same length match path");
            }
        }
        current = current->next;
    }

    return best_candidate.mount;
}

char * vfs_get_native_path(const char *path, vfs_mount_t *mount) {
    if (path == NULL)
        panic("vfs_get_native_path: path is NULL");
    if (mount == NULL)
        panic("vfs_get_native_path: mount is NULL");

    size_t mount_point_len = strlen(mount->mount_point);
    if (strncmp(path, mount->mount_point, mount_point_len) != 0)
        panic("vfs_get_native_path: Path does not start with mount point");

    const char *suffix = path + mount_point_len;
    char *native_path_copy;

    if (*suffix == '\0') {
        /* path equals mount point exactly (e.g. path="/" mount="/") */
        native_path_copy = kmalloc(2);
        if (!native_path_copy)
            panic("vfs_get_native_path: Unable to allocate memory for native path");
        native_path_copy[0] = '/';
        native_path_copy[1] = '\0';
    } else if (*suffix == '/') {
        /* Non-root mount: mount="/usr", path="/usr/bin" → suffix="/bin" */
        native_path_copy = kmalloc(strlen(suffix) + 1);
        if (!native_path_copy)
            panic("vfs_get_native_path: Unable to allocate memory for native path");
        strcpy(native_path_copy, suffix);
    } else {
        /* Root mount: mount="/", path="/init.elf" → suffix="init.elf" → prepend '/' */
        size_t len = strlen(suffix);
        native_path_copy = kmalloc(len + 2);
        if (!native_path_copy)
            panic("vfs_get_native_path: Unable to allocate memory for native path");
        native_path_copy[0] = '/';
        strcpy(native_path_copy + 1, suffix);
    }
    return native_path_copy;
}

/* Defined below; forward-declared so vfs_fstat (which comes first) can use it. */
static char * vfs_reconstruct_abs_path(vfs_file_descriptor_t *fd);

/* True if some mount lives at or below `abs_path` (e.g. abs_path="/dev",
   mount_point="/dev/null") -- makes `abs_path` an implicit directory even
   though the underlying filesystem has no matching entry for it. Mirrors
   the mount overlay in vfs_readdir, so stat()/opendir() on a directory
   like /dev (which holds only device mounts and no real files/dirents)
   agree with what `ls` actually shows for it. */
static int vfs_path_has_mount_inside(const char *abs_path) {
    size_t plen = strlen(abs_path);
    int root = (plen == 1 && abs_path[0] == '/');
    for (vfs_mount_t *m = vfs_mounts; m != NULL; m = m->next) {
        size_t mlen = strlen(m->mount_point);
        if (mlen <= plen) continue;
        if (strncmp(m->mount_point, abs_path, plen) != 0) continue;
        if (!root && m->mount_point[plen] != '/') continue;
        return 1;
    }
    return 0;
}

status_t vfs_open(const char *path, int flags, uint32_t mode, vfs_file_descriptor_t *fd) {
    if (path == NULL) {
        panic("vfs_open: path is NULL");
    }
    if (fd == NULL) {
        panic("vfs_open: fd is NULL");
    }

    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) {
        return FAILURE;
    }
    fd->mount = mount;
    fd->position = 0;
    fd->flags = flags;
    fd->pipe = NULL;
    fd->native_path = vfs_get_native_path(path, mount);
    if (!fd->native_path) {
        return FAILURE;
    }
    fd->valid = 1;

    /* Check existence via fstat */
    vfs_stat_t stat_buf;
    status_t res = vfs_fstat(fd, &stat_buf);
    if (res == SUCCESS) {
        /* File exists */
        if (flags & O_EXCL) {
            kfree(fd->native_path);
            fd->valid = 0;
            return ALREADY_EXISTS;
        }
        if (S_ISFIFO(stat_buf.st_mode) && !(flags & O_PATH)) {
            /* Named pipe: hand off to the shared-pipe rendezvous/blocking
             * logic instead of a normal mount-backed open. On success,
             * fd->native_path is kept as the pipe fd's occupied-marker
             * (freed later by pipe_close, same as any other pipe fd).
             * O_PATH is exempted -- stat()/statx() are implemented via a
             * real vfs_open()+fstat()+close() (see syscall_stat/syscall_statx
             * in syscall.c), and POSIX requires stat() on a FIFO to never
             * block or otherwise "connect" to it. */
            int64_t fr = fifo_open(fd->mount, fd->native_path, flags, fd);
            if (fr != 0) {
                kfree(fd->native_path);
                fd->valid = 0;
                return (status_t)fr;
            }
            return SUCCESS;
        }
    } else {
        /* File does not exist */
        if ((flags & O_CREAT) && fd->mount->ops->create) {
            res = fd->mount->ops->create(fd->mount->major, fd->mount->minor,
                                         fd->native_path, mode & 07777);
            if (res != SUCCESS) {
                kfree(fd->native_path);
                fd->valid = 0;
                return FAILURE;
            }
        } else {
            kfree(fd->native_path);
            fd->valid = 0;
            return FAILURE;
        }
    }

    return SUCCESS;
}


/* Open a directory — like vfs_open but doesn't honor O_CREAT/O_EXCL. Still
   requires the path to resolve to something real: either a directory the
   underlying filesystem reports via fstat (including filesystems like x1fs
   that synthesize "implicit" directories for a path with children but no
   explicit dirent — x1fs_fstat's own x1fs_is_directory() check covers that),
   or a path with a mount nested inside it (e.g. /dev, via vfs_fstat's own
   mount-overlay fallback). Without this check, vfs_find_mount's "/"
   catch-all made this silently succeed for ANY path, e.g. /proc/self/fd on
   a kernel with no /proc — only to fail on the first readdir(), which not
   every libc's opendir()/readdir() split tolerates gracefully. */
status_t vfs_open_dir(const char *path, vfs_file_descriptor_t *fd) {
    if (path == NULL) panic("vfs_open_dir: path is NULL");
    if (fd == NULL)   panic("vfs_open_dir: fd is NULL");

    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;

    fd->mount = mount;
    fd->position = 0;
    fd->flags = O_RDONLY;
    fd->pipe = NULL;
    fd->synth_index = 0;
    fd->native_path = vfs_get_native_path(path, mount);
    if (!fd->native_path) return FAILURE;
    fd->valid = 1;

    vfs_stat_t stat_buf;
    if (vfs_fstat(fd, &stat_buf) != SUCCESS) {
        kfree(fd->native_path);
        fd->valid = 0;
        return FAILURE;
    }

    return SUCCESS;
}

status_t vfs_close(vfs_file_descriptor_t *fd) {

    if (fd == NULL) {
        panic("vfs_close: fd is NULL");
    }
    if (fd->pipe) {
        pipe_close(fd);
        return SUCCESS;
    }
    if (fd->mount == NULL || fd->mount->ops == NULL) {
        panic("vfs_close: Invalid mount or close operation");
    }
    if (fd->native_path == NULL) {
        panic("vfs_close: fd->native_path is NULL");
    }
    kfree(fd->native_path);
    fd->valid = 0;
    return SUCCESS;
}

ssize_t vfs_read(vfs_file_descriptor_t *fd, void *buf, size_t count) {

    if (fd == NULL || buf == NULL) {
        panic("vfs_read: fd or buf is NULL");
    }
    if (fd->pipe) {
        return pipe_read(fd, buf, count);
    }
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->read == NULL) {
        panic("vfs_read: Invalid mount or read operation");
    }

    ssize_t bytes_read = fd->mount->ops->read(fd->mount->major, fd->mount->minor, fd->native_path, fd->position, buf, count);
    if (bytes_read > 0) {
        fd->position += bytes_read;
    }


    return bytes_read;
}

ssize_t vfs_write(vfs_file_descriptor_t *fd, const void *buf, size_t count) {

    if (fd == NULL || buf == NULL) {
        panic("vfs_write: fd or buf is NULL");
    }
    if (fd->pipe) {
        return pipe_write(fd, buf, count);
    }
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->write == NULL) {
        panic("vfs_write: Invalid mount or write operation");
    }

    if (fd->flags & O_APPEND) {
        /* Every O_APPEND write must land at the current end of file, not
         * wherever this fd's position last was -- otherwise `>>` silently
         * overwrites from offset 0 instead of appending. */
        vfs_stat_t st;
        if (vfs_fstat(fd, &st) == SUCCESS && (size_t)st.st_size > fd->position) {
            fd->position = (size_t)st.st_size;
        }
    }

    ssize_t bytes_written = fd->mount->ops->write(fd->mount->major, fd->mount->minor, fd->native_path, fd->position, buf, count);
    if (bytes_written > 0) {
        fd->position += bytes_written;
    }


    return bytes_written;
}

status_t vfs_fstat(vfs_file_descriptor_t *fd, vfs_stat_t *buf) {


    if (fd == NULL || buf == NULL) {
        panic("vfs_fstat: fd or buf is NULL");
    }
    if (fd->pipe) {
        return pipe_fstat(fd, buf);
    }
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->fstat == NULL) {
        panic("vfs_fstat: Invalid mount or fstat operation");
    }
    status_t res = fd->mount->ops->fstat(fd->mount->major, fd->mount->minor, fd->native_path, buf);

    if (res != SUCCESS) {
        char *abs = vfs_reconstruct_abs_path(fd);
        if (abs) {
            if (vfs_path_has_mount_inside(abs)) {
                memset(buf, 0, sizeof(vfs_stat_t));
                buf->st_mode = 0x41ED; /* S_IFDIR | rwxr-xr-x */
                buf->st_nlink = 2;
                res = SUCCESS;
            }
            kfree(abs);
        }
    }

    return res;
}

/* Reconstruct the absolute path a directory fd refers to, from its mount
   point + native (mount-relative) path -- the inverse of vfs_get_native_path. */
static char * vfs_reconstruct_abs_path(vfs_file_descriptor_t *fd) {
    const char *mp = fd->mount->mount_point;
    if (strcmp(mp, "/") == 0) {
        char *abs = kmalloc(strlen(fd->native_path) + 1);
        if (abs) strcpy(abs, fd->native_path);
        return abs;
    }
    char *abs = kmalloc(strlen(mp) + strlen(fd->native_path) + 1);
    if (abs) {
        strcpy(abs, mp);
        strcat(abs, fd->native_path);
    }
    return abs;
}

/* True if mount m sits directly inside directory abs_dir (e.g. abs_dir="/dev",
   m->mount_point="/dev/null" -- but not "/dev/sub/null"). On match, *name_out
   points at the child's name within m->mount_point. */
static int vfs_mount_is_direct_child(const char *abs_dir, vfs_mount_t *m, const char **name_out) {
    size_t dir_len = strlen(abs_dir);
    int root = (dir_len == 1 && abs_dir[0] == '/');
    size_t prefix_len = root ? dir_len : dir_len + 1; /* account for the extra '/' */

    if (strlen(m->mount_point) <= prefix_len) return 0;
    if (strncmp(m->mount_point, abs_dir, dir_len) != 0) return 0;
    if (!root && m->mount_point[dir_len] != '/') return 0;

    const char *suffix = m->mount_point + prefix_len;
    if (*suffix == '\0' || strchr(suffix, '/') != NULL) return 0;
    *name_out = suffix;
    return 1;
}

/* Mounts (e.g. /dev/null, /dev/tty0) intercept opens on their exact path but
   have no corresponding dirent in the real filesystem underneath them, so
   they'd otherwise be invisible to `ls`. Once the real directory's entries
   are exhausted, synthesize one dirent per mount that lives directly inside
   the directory being listed. */
static ssize_t vfs_readdir_overlay_mounts(vfs_file_descriptor_t *fd, void *buf, size_t count) {
    char *abs_dir = vfs_reconstruct_abs_path(fd);
    if (!abs_dir) return 0;

    uint8_t *out = (uint8_t *)buf;
    ssize_t written = 0;
    size_t matched = 0;

    for (vfs_mount_t *m = vfs_mounts; m != NULL; m = m->next) {
        const char *name;
        if (!vfs_mount_is_direct_child(abs_dir, m, &name)) continue;

        if (matched >= fd->synth_index) {
            size_t name_len = strlen(name);
            size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;
            if ((size_t)written + reclen > count) break;

            memset(out, 0, reclen);
            *(uint64_t *)(out +  0) = matched + 1; /* d_ino */
            *(uint64_t *)(out +  8) = matched + 1; /* d_off */
            *(uint16_t *)(out + 16) = (uint16_t)reclen;
            *(uint8_t  *)(out + 18) = DT_CHR; /* every current mount overlay is a char device */
            memcpy(out + 19, name, name_len + 1);

            out += reclen;
            written += (ssize_t)reclen;
            fd->synth_index++;
        }
        matched++;
    }

    kfree(abs_dir);
    return written;
}

ssize_t vfs_readdir(vfs_file_descriptor_t *fd, void *buf, size_t count) {
    if (fd == NULL || buf == NULL) return -EINVAL;
    if (fd->pipe) return -ENOTDIR;
    if (fd->mount == NULL || fd->mount->ops == NULL) return -EBADF;
    if (fd->mount->ops->readdir == NULL) return -ENOTDIR;

    ssize_t real_bytes = fd->mount->ops->readdir(fd->mount->major, fd->mount->minor,
                                   fd->native_path, &fd->position, buf, count);
    if (real_bytes != 0) return real_bytes;

    return vfs_readdir_overlay_mounts(fd, buf, count);
}

status_t vfs_ioctl(vfs_file_descriptor_t *fd, uint64_t request, void * arg) {

    if (fd == NULL) {
        panic("vfs_ioctl: fd is NULL");
    }
    if (fd->pipe) {
        return FAILURE; /* pipes have no ioctls; lets sys_isatty() see ENOTTY */
    }
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->ioctl == NULL) {
        panic("vfs_ioctl: Invalid mount or ioctl operation");
    }
    status_t res = fd->mount->ops->ioctl(fd->mount->major, fd->mount->minor, fd->native_path, request, arg);

    return res;
}

status_t vfs_mkdir(const char *path, uint32_t mode) {
    if (!path) panic("vfs_mkdir: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->mkdir) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->mkdir(mount->major, mount->minor, native, mode);
    kfree(native);
    return r;
}

status_t vfs_mkfifo(const char *path, uint32_t mode) {
    if (!path) panic("vfs_mkfifo: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->mkfifo) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->mkfifo(mount->major, mount->minor, native, mode);
    kfree(native);
    return r;
}

status_t vfs_unlink(const char *path) {
    if (!path) panic("vfs_unlink: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->unlink) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->unlink(mount->major, mount->minor, native);
    kfree(native);
    return r;
}

status_t vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) panic("vfs_rename: path is NULL");
    vfs_mount_t *om = vfs_find_mount(oldpath);
    vfs_mount_t *nm = vfs_find_mount(newpath);
    if (!om || !nm || om != nm) return FAILURE; /* cross-device rename unsupported */
    if (!om->ops->rename_op) return NOT_IMPLEMENTED;
    char *on = vfs_get_native_path(oldpath, om);
    char *nn = vfs_get_native_path(newpath, nm);
    status_t r = om->ops->rename_op(om->major, om->minor, on, nn);
    kfree(on);
    kfree(nn);
    return r;
}

status_t vfs_rmdir(const char *path) {
    if (!path) panic("vfs_rmdir: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->rmdir) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->rmdir(mount->major, mount->minor, native);
    kfree(native);
    return r;
}

status_t vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) panic("vfs_symlink: NULL argument");
    vfs_mount_t *mount = vfs_find_mount(linkpath);
    if (!mount) return FAILURE;
    if (!mount->ops->symlink) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(linkpath, mount);
    status_t r = mount->ops->symlink(mount->major, mount->minor, native, target);
    kfree(native);
    return r;
}

/* Like vfs_fstat, but takes a path directly and never follows a symlink at
   the end of it -- used by follow_symlinks() (syscall.c) to test whether a
   path prefix is itself a symlink. Deliberately skips vfs_fstat's mount-
   overlay fallback (the "/dev is implicitly a directory" trick): a failed
   lookup here is simply treated by the caller as "not a symlink". */
status_t vfs_lstat_path(const char *path, vfs_stat_t *buf) {
    if (!path) panic("vfs_lstat_path: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->fstat) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->fstat(mount->major, mount->minor, native, buf);
    kfree(native);
    return r;
}

ssize_t vfs_readlink_path(const char *path, char *buf, size_t bufsz) {
    if (!path) return -EINVAL;
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return -ENOENT;
    if (!mount->ops->readlink) return -EINVAL;
    char *native = vfs_get_native_path(path, mount);
    ssize_t r = mount->ops->readlink(mount->major, mount->minor, native, buf, bufsz);
    kfree(native);
    return r;
}

status_t vfs_chmod(const char *path, uint32_t mode) {
    if (!path) panic("vfs_chmod: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->chmod) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->chmod(mount->major, mount->minor, native, mode);
    kfree(native);
    return r;
}

status_t vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!path) panic("vfs_chown: path is NULL");
    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;
    if (!mount->ops->chown) return NOT_IMPLEMENTED;
    char *native = vfs_get_native_path(path, mount);
    status_t r = mount->ops->chown(mount->major, mount->minor, native, uid, gid);
    kfree(native);
    return r;
}