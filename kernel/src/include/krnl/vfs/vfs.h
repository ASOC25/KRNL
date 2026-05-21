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

/* Must be exactly 256 bytes — matches mlibc's struct statx */
typedef struct {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __pad0;
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	struct { int64_t tv_sec; uint32_t tv_nsec; uint32_t __pad; } stx_atime;
	struct { int64_t tv_sec; uint32_t tv_nsec; uint32_t __pad; } stx_btime;
	struct { int64_t tv_sec; uint32_t tv_nsec; uint32_t __pad; } stx_ctime;
	struct { int64_t tv_sec; uint32_t tv_nsec; uint32_t __pad; } stx_mtime;
	uint32_t stx_rdev_major;
	uint32_t stx_rdev_minor;
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
	uint64_t stx_mnt_id;
	uint32_t stx_dio_mem_align;
	uint32_t stx_dio_offset_align;
	uint64_t __spare[12];
} __attribute__((packed)) vfs_statx_t;
_Static_assert(sizeof(vfs_statx_t) == 256, "vfs_statx_t must be 256 bytes");

#define STATX_BASIC_STATS 0x7ffU

/* Kernel-side directory entry written into readdir buffers.
   Layout matches mlibc's struct dirent: ino(8), off(8), reclen(2), type(1), name[]. */
typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1]; /* actual name follows; d_reclen covers the full entry */
} krnl_dirent_t;

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_CHR     2
#define DT_REG     8

typedef struct vfs_fs {
    char name[32];
    //read
    ssize_t (*read)(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count);
    //write
    ssize_t (*write)(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count);
    //fstat
    status_t (*fstat)(device_major_t major, device_minor_t minor, const char * path, vfs_stat_t * buf);
    status_t (*ioctl)(device_major_t major, device_minor_t minor, const char * path, uint64_t request, void * arg);
    /* readdir: fill buf with krnl_dirent_t entries starting at *index; update *index */
    ssize_t (*readdir)(device_major_t major, device_minor_t minor, const char * path, size_t *index, void * buf, size_t count);
    //Detect filesystem
    status_t (*detect)(device_major_t major, device_minor_t minor);
    /* Write-capable operations (NULL = unsupported / read-only) */
    status_t (*mkdir)(device_major_t major, device_minor_t minor, const char *path, uint32_t mode);
    status_t (*create)(device_major_t major, device_minor_t minor, const char *path, uint32_t mode);
    status_t (*unlink)(device_major_t major, device_minor_t minor, const char *path);
    status_t (*rename_op)(device_major_t major, device_minor_t minor, const char *oldpath, const char *newpath);
    status_t (*rmdir)(device_major_t major, device_minor_t minor, const char *path);

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
status_t vfs_open_dir(const char *path, vfs_file_descriptor_t *fd);
status_t vfs_close(vfs_file_descriptor_t *fd);
ssize_t vfs_read(vfs_file_descriptor_t *fd, void *buf, size_t count);
ssize_t vfs_write(vfs_file_descriptor_t *fd, const void *buf, size_t count);
status_t vfs_fstat(vfs_file_descriptor_t *fd, vfs_stat_t *buf);
status_t vfs_ioctl(vfs_file_descriptor_t *fd, uint64_t request, void * arg);
ssize_t vfs_readdir(vfs_file_descriptor_t *fd, void *buf, size_t count);

/* Write-capable VFS operations */
status_t vfs_mkdir(const char *path, uint32_t mode);
status_t vfs_unlink(const char *path);
status_t vfs_rename(const char *oldpath, const char *newpath);
status_t vfs_rmdir(const char *path);

#endif