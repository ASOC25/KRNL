#ifndef _TTY_H
#define _TTY_H

#include <krnl/libraries/std/stdint.h>

#define TTY_MAX_FILES 1024
#define SERIAL_DRIVER_MAJOR_NUMBER 3 //TODO: Change this to a real tty driver!!!

void tty_init(void);

/* Feeds one raw input byte through the tty line discipline (echo, line
   editing, ISIG signal generation). Called from the serial IRQ handler as
   bytes arrive, independent of whether any thread is currently blocked in
   tty_read() — this is what lets Ctrl-C interrupt a foreground process that
   isn't reading stdin. */
void tty_input_byte(uint8_t c);

/* Sets the controlling process group for the console tty (used by ISIG to
   target SIGINT/SIGQUIT/SIGTSTP, and by TIOCGPGRP/TIOCSPGRP). */
void tty_set_foreground_pgrp(int pgid);

#endif