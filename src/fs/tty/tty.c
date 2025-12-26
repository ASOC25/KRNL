#include <krnl/fs/tty/tty.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>

ssize_t tty_read(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count) {
    (void)path;
    int64_t read_bytes = devices_read(major, minor, skip, count, (uint8_t *)buf);
    return read_bytes;
}
ssize_t tty_write(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count) {
    (void)path;
    int64_t written_bytes = devices_write(major, minor, skip, count, (const uint8_t *)buf);
    return written_bytes;
}
status_t tty_detect(device_major_t major, device_minor_t minor) {
    (void)major;
    (void)minor;
    // For simplicity, always return SUCCESS
    return (major == SERIAL_DRIVER_MAJOR_NUMBER) ? SUCCESS : FAILURE;
}
status_t tty_fstat(device_major_t major, device_minor_t minor, const char * path, vfs_stat_t * buf) {
    (void)major;
    (void)minor;
    (void)path;
    memset(buf, 0, sizeof(vfs_stat_t));
    buf->st_mode = 0x2190; // Character device with rw-r--r-- permissions
    buf->st_nlink = 1;
    return SUCCESS;
}

status_t tty_ioctl(device_major_t major, device_minor_t minor, const char * path, uint64_t request, void * arg) {
    (void)major;
    (void)minor;
    (void)path;
    (void)request;
    (void)arg;
    // TTY does not support any ioctls in this implementation
    return FAILURE; // Ioctl not supported
}

void tty_init(void) {
    vfs_fs_t *tty_ops = (vfs_fs_t *)kmalloc(sizeof(vfs_fs_t));
    if (!tty_ops) {
        panic("tty_init: Unable to allocate memory for TTY operations");
    }

    memset(tty_ops->name, 0, 32);
    strcpy(tty_ops->name, "TTY");
    tty_ops->read = tty_read;
    tty_ops->write = tty_write;
    tty_ops->detect = tty_detect;
    tty_ops->fstat = tty_fstat;
    tty_ops->ioctl = tty_ioctl;
    tty_ops->next = NULL;

    status_t result = vfs_register_fs(tty_ops);
    if (result != SUCCESS) {
        panic("tty_init: Failed to register TTY with VFS");
    }
}