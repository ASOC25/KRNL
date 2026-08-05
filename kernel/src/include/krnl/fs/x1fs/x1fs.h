#ifndef _X1FS_H
#define _X1FS_H

#include <krnl/vfs/vfs.h>
#include <krnl/devices/devices.h>

#define X1FS_MAX_FILES 1024

void x1fs_init(void);

/* Used by syscall_mount() to back a "tmpfs" mount: a fresh, empty x1fs
   instance with no backing ramdisk device, registered under a freshly
   allocated minor number. See x1fs.c for details. */
vfs_fs_t * x1fs_get_ops(void);
status_t x1fs_create_tmpfs(device_minor_t *out_minor);
status_t x1fs_destroy_tmpfs(device_minor_t minor);

#endif
