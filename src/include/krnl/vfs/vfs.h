#ifndef _VFS_H
#define _VFS_H

#define VFS_PATH_MAX 4096

#include <krnl/libraries/std/stddef.h>
#include <krnl/devices/devices.h>

typedef struct vfs_fs {
    char name[32];
    //read
    ssize_t (*read)(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count);
    //write
    ssize_t (*write)(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count);
    //Detect filesystem
    status_t (*detect)(device_major_t major, device_minor_t minor);

    struct vfs_fs *next;
} vfs_fs_t;

typedef struct vfs_mount {
    device_major_t major;
    device_minor_t minor;
    struct vfs_fs *ops;
    char * mount_point;
    struct vfs_mount *next;
} vfs_mount_t;

typedef struct vfs_file_descriptor {
    vfs_mount_t *mount;
    size_t position;
    int flags;
    char * native_path; // Path within the mounted filesystem
} vfs_file_descriptor_t;

status_t vfs_register_fs(vfs_fs_t *ops);
status_t vfs_unregister_fs(char *fs_name);

vfs_mount_t *vfs_new_mount(device_major_t major, device_minor_t minor, const char *mount_point);
status_t vfs_remove_mount(const char *mount_point);

vfs_file_descriptor_t * vfs_open(const char *path, int flags);
ssize_t vfs_close(vfs_file_descriptor_t *fd);
ssize_t vfs_read(vfs_file_descriptor_t *fd, void *buf, size_t count);
ssize_t vfs_write(vfs_file_descriptor_t *fd, const void *buf, size_t count);

#endif