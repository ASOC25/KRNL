#include <krnl/vfs/vfs.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/libraries/assert/assert.h>

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

vfs_mount_t * vfs_new_mount(device_major_t major, device_minor_t minor, const char *mount_point) {
    if (mount_point == NULL) {
        panic("vfs_new_mount: mount_point or ops is NULL");
    }
    if (strlen(mount_point) >= VFS_PATH_MAX) {
        panic("vfs_new_mount: Mount point path too long");
    }

    vfs_fs_t *ops = detect_fs(major, minor);

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

status_t vfs_open(const char *path, int flags, vfs_file_descriptor_t *fd) {
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
    } else {
        /* File does not exist */
        if ((flags & O_CREAT) && fd->mount->ops->create) {
            res = fd->mount->ops->create(fd->mount->major, fd->mount->minor,
                                         fd->native_path, 0644);
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


/* Open a directory — like vfs_open but skips the fstat existence check so
   filesystems that have implicit directories (e.g. x1fs) can be opened. */
status_t vfs_open_dir(const char *path, vfs_file_descriptor_t *fd) {
    if (path == NULL) panic("vfs_open_dir: path is NULL");
    if (fd == NULL)   panic("vfs_open_dir: fd is NULL");

    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) return FAILURE;

    fd->mount = mount;
    fd->position = 0;
    fd->flags = O_RDONLY;
    fd->native_path = vfs_get_native_path(path, mount);
    if (!fd->native_path) return FAILURE;
    fd->valid = 1;
    return SUCCESS;
}

status_t vfs_close(vfs_file_descriptor_t *fd) {

    if (fd == NULL) {
        panic("vfs_close: fd is NULL");
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
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->write == NULL) {
        panic("vfs_write: Invalid mount or write operation");
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
    if (fd->mount == NULL || fd->mount->ops == NULL || fd->mount->ops->fstat == NULL) {
        panic("vfs_fstat: Invalid mount or fstat operation");
    }
    status_t res = fd->mount->ops->fstat(fd->mount->major, fd->mount->minor, fd->native_path, buf);

    return res;
}

ssize_t vfs_readdir(vfs_file_descriptor_t *fd, void *buf, size_t count) {
    if (fd == NULL || buf == NULL) return -EINVAL;
    if (fd->mount == NULL || fd->mount->ops == NULL) return -EBADF;
    if (fd->mount->ops->readdir == NULL) return -ENOTDIR;
    return fd->mount->ops->readdir(fd->mount->major, fd->mount->minor,
                                   fd->native_path, &fd->position, buf, count);
}

status_t vfs_ioctl(vfs_file_descriptor_t *fd, uint64_t request, void * arg) {

    if (fd == NULL) {
        panic("vfs_ioctl: fd is NULL");
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