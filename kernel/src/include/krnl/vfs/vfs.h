#ifndef _VFS_H
#define _VFS_H

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define VFS_PATH_MAX 4096

#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/time.h>
#include <krnl/devices/devices.h>

#define O_WRONLY    0x1
#define O_RDONLY    0x2
#define O_RDWR      0x4
#define O_CREAT     0x8
#define O_EXCL      0x10
#define O_NOCTTY    0x20
#define O_TRUNC     0x40
#define O_APPEND    0x80
#define O_NONBLOCK  0x100
#define O_DSYNC     0x200
#define O_DIRECT    0x400
#define O_LARGEFILE 0x800
#define O_DIRECTORY 0x1000
#define O_NOFOLLOW  0x2000
#define O_CLOEXEC   0x4000

typedef struct stat {
	uint64_t st_dev;
	uint64_t st_ino;
	unsigned long st_nlink;
	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	uint64_t st_rdev;
	long st_size;
	long st_blksize;
	int64_t st_blocks;
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	long __unused[3];
} vfs_stat_t;

typedef struct vfs_fs {
    char name[32];
    //read
    ssize_t (*read)(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count);
    //write
    ssize_t (*write)(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count);
    //fstat
    status_t (*fstat)(device_major_t major, device_minor_t minor, const char * path, vfs_stat_t * buf);
    status_t (*ioctl)(device_major_t major, device_minor_t minor, const char * path, uint64_t request, void * arg);
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

typedef struct vfs_path {
    vfs_mount_t * mount;
    char internal_path[VFS_PATH_MAX];
} vfs_path_t;

typedef struct vfs_file_descriptor {
    uint8_t valid;
    vfs_mount_t *mount;
    size_t position;
    int flags;
    char * native_path; // Path within the mounted filesystem
} vfs_file_descriptor_t;

status_t vfs_register_fs(vfs_fs_t *ops);
status_t vfs_unregister_fs(char *fs_name);

vfs_mount_t *vfs_new_mount(device_major_t major, device_minor_t minor, const char *mount_point);
status_t vfs_remove_mount(const char *mount_point);

status_t vfs_open(const char *path, int flags, vfs_file_descriptor_t *fd);
status_t vfs_close(vfs_file_descriptor_t *fd);
ssize_t vfs_read(vfs_file_descriptor_t *fd, void *buf, size_t count);
ssize_t vfs_write(vfs_file_descriptor_t *fd, const void *buf, size_t count);
status_t vfs_fstat(vfs_file_descriptor_t *fd, vfs_stat_t *buf);
status_t vfs_ioctl(vfs_file_descriptor_t *fd, uint64_t request, void * arg);

#endif