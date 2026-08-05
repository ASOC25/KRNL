#include <krnl/fs/tty/tty.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/process/process.h>
#include <krnl/process/signals.h>
#include <krnl/process/scheduler.h>

#define TCGETS    0x5401
#define TCSETS    0x5402
#define TCSETSW   0x5403
#define TCSETSF   0x5404
#define TCXONC    0x540A
#define TIOCGWINSZ  0x5413
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TTY_CHECK_VAL 0x69

/* constants for tcflow()'s action argument, passed straight through as the
   TCXONC ioctl's value (not a pointer) — matches Linux/glibc numbering. */
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

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

/* Default termios: canonical mode with echo enabled.
   c_cc layout matches mlibc/glibc's termios cc indices:
   VINTR=0 VQUIT=1 VERASE=2 VKILL=3 VEOF=4 VTIME=5 VMIN=6 VSWTC=7
   VSTART=8 VSTOP=9 VSUSP=10 VEOL=11 VREPRINT=12 VDISCARD=13 VWERASE=14
   VLNEXT=15 VEOL2=16 */
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

/* Process group currently allowed to generate ISIG signals / read the tty
   as "foreground". Set once at boot (see tty_set_foreground_pgrp) and
   updated by bash et al. via TIOCSPGRP when handing the terminal to jobs. */
static int16_t tty_foreground_pgrp = 0;

/* ---- Line discipline state (single global console tty) ---------------- */

#define TTY_BUF_SIZE 4096
#define TTY_READ_WAIT 0x54545259 /* "TTRY" — wait channel for blocking reads */

/* In-progress canonical line, not yet terminated by a newline/EOF. */
static uint8_t tty_line_buf[TTY_BUF_SIZE];
static size_t  tty_line_len = 0;

/* Ring buffer of bytes that are ready to be handed to read(): complete
   canonical lines (including the trailing '\n'), or raw bytes one at a
   time when ICANON is off. */
static uint8_t tty_cooked_buf[TTY_BUF_SIZE];
static size_t  tty_cooked_head = 0;
static size_t  tty_cooked_tail = 0;
static size_t  tty_cooked_count = 0;

/* Set when VEOF (Ctrl-D) is pressed on an empty line: the next read()
   returns 0 (EOF) instead of blocking. */
static int tty_eof_pending = 0;

static void tty_cooked_push(uint8_t c) {
    if (tty_cooked_count >= TTY_BUF_SIZE) return; /* drop on overflow */
    tty_cooked_buf[tty_cooked_tail] = c;
    tty_cooked_tail = (tty_cooked_tail + 1) % TTY_BUF_SIZE;
    tty_cooked_count++;
}

static void tty_raw_putc(uint8_t c) {
    devices_write(SERIAL_DRIVER_MAJOR_NUMBER, 0, 0, 1, &c);
}

/* Echoes one output byte, honoring OPOST/ONLCR (\n -> \r\n) same as a real
   write() would, so typed characters line up with a following prompt. */
static void tty_echo_char(uint8_t c) {
    if (!(tty_termios_state.c_lflag & ECHO)) return;
    if (c == '\n' && (tty_termios_state.c_oflag & OPOST) && (tty_termios_state.c_oflag & ONLCR)) {
        tty_raw_putc('\r');
        tty_raw_putc('\n');
    } else {
        tty_raw_putc(c);
    }
}

/* Visual feedback for VERASE/VKILL: backspace, blank the erased column, backspace again. */
static void tty_echo_erase(void) {
    if (!(tty_termios_state.c_lflag & ECHO)) return;
    tty_raw_putc(8);
    tty_raw_putc(' ');
    tty_raw_putc(8);
}

/* Visual feedback for a control char that generated a signal, e.g. "^C". */
static void tty_echo_ctrl(char letter) {
    if (!(tty_termios_state.c_lflag & ECHO)) return;
    tty_raw_putc('^');
    tty_raw_putc((uint8_t)letter);
    tty_echo_char('\n');
}

void tty_set_foreground_pgrp(int pgid) {
    tty_foreground_pgrp = (int16_t)pgid;
}

void tty_input_byte(uint8_t c) {
    tcflag_t lflag = tty_termios_state.c_lflag;
    tcflag_t iflag = tty_termios_state.c_iflag;
    cc_t *cc = tty_termios_state.c_cc;

    /* ISIG: control chars generate signals to the foreground process group
       regardless of canonical/raw mode and regardless of whether that group
       is currently blocked in read() — this is what lets Ctrl-C interrupt a
       busy-looping foreground job. */
    if (lflag & ISIG) {
        if (cc[0] != 0 && c == cc[0]) { /* VINTR */
            tty_echo_ctrl('C');
            tty_line_len = 0;
            if (tty_foreground_pgrp > 0) scheduler_signal_pgrp(tty_foreground_pgrp, SIGINT);
            return;
        }
        if (cc[1] != 0 && c == cc[1]) { /* VQUIT */
            tty_echo_ctrl('\\');
            tty_line_len = 0;
            if (tty_foreground_pgrp > 0) scheduler_signal_pgrp(tty_foreground_pgrp, SIGQUIT);
            return;
        }
        if (cc[10] != 0 && c == cc[10]) { /* VSUSP */
            tty_echo_ctrl('Z');
            tty_line_len = 0;
            if (tty_foreground_pgrp > 0) scheduler_signal_pgrp(tty_foreground_pgrp, SIGTSTP);
            return;
        }
    }

    if ((iflag & ICRNL) && c == '\r') c = '\n';

    if (!(lflag & ICANON)) {
        /* Raw mode: every byte goes straight to the read queue. */
        tty_echo_char(c);
        tty_cooked_push(c);
        wakeup(TTY_READ_WAIT);
        return;
    }

    if (cc[2] != 0 && c == cc[2]) { /* VERASE */
        if (tty_line_len > 0) {
            tty_line_len--;
            tty_echo_erase();
        }
        return;
    }
    if (cc[3] != 0 && c == cc[3]) { /* VKILL */
        while (tty_line_len > 0) {
            tty_line_len--;
            tty_echo_erase();
        }
        return;
    }
    if (cc[4] != 0 && c == cc[4]) { /* VEOF */
        if (tty_line_len == 0) {
            tty_eof_pending = 1;
        } else {
            for (size_t i = 0; i < tty_line_len; i++) tty_cooked_push(tty_line_buf[i]);
            tty_line_len = 0;
        }
        wakeup(TTY_READ_WAIT);
        return;
    }

    tty_echo_char(c);
    if (tty_line_len < TTY_BUF_SIZE) tty_line_buf[tty_line_len++] = c;
    if (c == '\n') {
        for (size_t i = 0; i < tty_line_len; i++) tty_cooked_push(tty_line_buf[i]);
        tty_line_len = 0;
        wakeup(TTY_READ_WAIT);
    }
}

ssize_t tty_read(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count) {
    (void)major;
    (void)minor;
    (void)path;
    (void)skip;
    if (count == 0) return 0;

    thread_t *current_thread = process_get_current_thread();
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;

    while (n == 0) {
        if (tty_cooked_count == 0) {
            if (tty_eof_pending) {
                tty_eof_pending = 0;
                return 0;
            }
            if (!current_thread) return -EIO;
            if (!sleep(current_thread, TTY_READ_WAIT)) return -EINTR;
            continue;
        }

        while (n < count && tty_cooked_count > 0) {
            out[n] = tty_cooked_buf[tty_cooked_head];
            tty_cooked_head = (tty_cooked_head + 1) % TTY_BUF_SIZE;
            tty_cooked_count--;
            uint8_t got = out[n];
            n++;
            /* One read() call returns at most one line in canonical mode. */
            if ((tty_termios_state.c_lflag & ICANON) && got == '\n') break;
        }
    }

    return (ssize_t)n;
}

ssize_t tty_write(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count) {
    (void)path;
    (void)skip;
    const uint8_t *in = (const uint8_t *)buf;
    tcflag_t oflag = tty_termios_state.c_oflag;

    if (!(oflag & OPOST) || !(oflag & ONLCR)) {
        return devices_write(major, minor, 0, count, in);
    }

    /* OPOST+ONLCR: translate outgoing '\n' -> "\r\n". */
    for (size_t i = 0; i < count; i++) {
        if (in[i] == '\n') {
            uint8_t cr = '\r';
            if (devices_write(major, minor, 0, 1, &cr) < 0) return (ssize_t)i;
        }
        if (devices_write(major, minor, 0, 1, &in[i]) < 0) return (ssize_t)i;
    }
    return (ssize_t)count;
}
status_t tty_detect(device_major_t major, device_minor_t minor) {
    (void)major;
    (void)minor;
    return (major == SERIAL_DRIVER_MAJOR_NUMBER) ? SUCCESS : FAILURE;
}
status_t tty_fstat(device_major_t major, device_minor_t minor, const char * path, vfs_stat_t * buf) {
    (void)path;
    memset(buf, 0, sizeof(vfs_stat_t));
    buf->st_mode = 0x2190; // Character device with rw-r--r-- permissions
    buf->st_nlink = 1;
    /* st_dev/st_ino must differ across distinct devices (major/minor) —
       see memdev_fstat's comment: otherwise callers that compare stat()
       results to tell devices apart (e.g. gnulib's SAME_INODE, used by GNU
       grep to detect "stdout is /dev/null") spuriously see this tty and
       /dev/null as the same file. */
    buf->st_dev  = (uint64_t)major;
    buf->st_ino  = (uint64_t)minor + 1;
    buf->st_rdev = ((uint64_t)major << 8) | (uint64_t)(minor & 0xff);
    return SUCCESS;
}

/* Real fd-readiness for select/pselect: readable iff a line (or raw byte,
   or pending EOF) is already buffered — mirrors tty_read's own blocking
   condition exactly. Writes to the serial console never block. */
int tty_poll(device_major_t major, device_minor_t minor, const char * path, int for_write) {
    (void)major;
    (void)minor;
    (void)path;
    if (for_write) return 1;
    return (tty_cooked_count > 0 || tty_eof_pending) ? 1 : 0;
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
        if (arg) {
            int32_t pgid = *(int32_t *)arg;
            if (pgid > 0) tty_set_foreground_pgrp(pgid);
        }
        return SUCCESS;
    }
    if (request == TCXONC) {
        /* Per ioctl(2)/tcflow(3): action is the value itself, not a pointer.
           This virtual console has no real flow-control hardware (no remote
           sender/receiver to pause) and writes never block, so all four
           actions are accepted as no-ops rather than failing with ENOSYS. */
        int action = (int)(intptr_t)arg;
        switch (action) {
            case TCOOFF:
            case TCOON:
            case TCIOFF:
            case TCION:
                return SUCCESS;
            default:
                return -EINVAL;
        }
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
    tty_ops->poll = tty_poll;
    tty_ops->readdir = NULL;
    tty_ops->next = NULL;

    status_t result = vfs_register_fs(tty_ops);
    if (result != SUCCESS) {
        panic("tty_init: Failed to register TTY with VFS");
    }
}
