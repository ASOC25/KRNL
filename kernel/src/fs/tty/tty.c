#include <krnl/fs/tty/tty.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/stdint.h>

#define TCGETS    0x5401
#define TCSETS    0x5402
#define TCSETSW   0x5403
#define TCSETSF   0x5404
#define TIOCGWINSZ  0x5413
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TTY_CHECK_VAL 0x69

#define ECHO   0000010
#define ICANON 0000002
#define ISIG   0000001
#define IEXTEN 0100000
#define ICRNL  0000400
#define OPOST  0000001
#define ONLCR  0000004
#define CS8    0000060
#define CREAD  0000200
#define CLOCAL 0004000

#define NCCS 32
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

struct tty_termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  ibaud;
    speed_t  obaud;
};

/* Default termios: canonical mode with echo enabled */
static struct tty_termios tty_termios_state = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD | CLOCAL,
    .c_lflag = ECHO | ICANON | ISIG | IEXTEN,
    .c_line  = 0,
    .c_cc    = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0 },
    .ibaud   = 38400,
    .obaud   = 38400,
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static int16_t tty_foreground_pgrp = 0;

ssize_t tty_read(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count) {
    (void)path;
    return devices_read(major, minor, skip, count, (uint8_t *)buf);
}
ssize_t tty_write(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count) {
    (void)path;
    int64_t written_bytes = devices_write(major, minor, skip, count, (const uint8_t *)buf);
    return written_bytes;
}
status_t tty_detect(device_major_t major, device_minor_t minor) {
    (void)major;
    (void)minor;
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
    if (request == TCGETS) {
        if (arg) memcpy(arg, &tty_termios_state, sizeof(tty_termios_state));
        return SUCCESS;
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        if (arg) memcpy(&tty_termios_state, arg, sizeof(tty_termios_state));
        return SUCCESS;
    }
    if (request == TIOCGWINSZ) {
        if (arg) {
            struct winsize *ws = (struct winsize *)arg;
            ws->ws_row    = 25;
            ws->ws_col    = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
        }
        return TTY_CHECK_VAL;
    }
    if (request == TIOCGPGRP) {
        if (arg) *(int32_t *)arg = (int32_t)tty_foreground_pgrp;
        return SUCCESS;
    }
    if (request == TIOCSPGRP) {
        if (arg) tty_foreground_pgrp = (int16_t)(*(int32_t *)arg);
        return SUCCESS;
    }
    return -ENOTTY;
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
    tty_ops->readdir = NULL;
    tty_ops->next = NULL;

    status_t result = vfs_register_fs(tty_ops);
    if (result != SUCCESS) {
        panic("tty_init: Failed to register TTY with VFS");
    }
}
