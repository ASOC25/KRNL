#include <krnl/vfs/vfs.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>

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

vfs_mount_t * vfs_find_mount(const char *path) {
    vfs_mount_t *current = vfs_mounts;
    while (current != NULL) {
        size_t mount_point_len = strlen(current->mount_point);
        if (strncmp(path, current->mount_point, mount_point_len) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

char * vfs_get_native_path(const char *path, vfs_mount_t *mount) {
    //Native path is the path relative to the mount point
    size_t mount_point_len = strlen(mount->mount_point);
    if (strncmp(path, mount->mount_point, mount_point_len) != 0) {
        panic("vfs_get_native_path: Path does not start with mount point");
    }
    const char *native_path = path + mount_point_len;
    if (*native_path == '\0') {
        native_path = "/";
    }
    char *native_path_copy = kmalloc(strlen(native_path) + 1);
    if (!native_path_copy) {
        panic("vfs_get_native_path: Unable to allocate memory for native path");
    }
    strcpy(native_path_copy, native_path);
    return native_path_copy;
}

vfs_file_descriptor_t * vfs_open(const char *path, int flags) {
    if (path == NULL) {
        panic("vfs_open: path is NULL");
    }

    vfs_mount_t *mount = vfs_find_mount(path);
    if (!mount) {
        return NULL;
    }

    vfs_file_descriptor_t *fd = (vfs_file_descriptor_t *)kmalloc(sizeof(vfs_file_descriptor_t));
    if (!fd) {
        panic("vfs_open: Unable to allocate memory for file descriptor");
    }

    fd->mount = mount;
    fd->position = 0;
    fd->flags = flags;
    fd->native_path = vfs_get_native_path(path, mount);
    if (!fd->native_path) {
        kfree(fd);
        panic("vfs_open: Unable to get native path");
    }

    return fd;
}


ssize_t vfs_close(vfs_file_descriptor_t *fd) {
    if (fd == NULL) {
        panic("vfs_close: fd is NULL");
    }

    if (fd->native_path) {
        kfree(fd->native_path);
    }
    kfree(fd);
    return 0;
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