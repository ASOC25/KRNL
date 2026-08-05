#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

void print_banner() {
    for (int i = 0; i < 10; i++) {
        printf("\n");
    }
    printf("OMEN Operating System - \n");
    printf("Credits to Omen Team and contributors.\n");
    printf("Visit github.com/omen-osdev/ for more information.\n");
    printf("Version: 0.1.0 21/06/25\n");
    printf("\n");
}

static pid_t spawn_shell(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("init: fork");
        return -1;
    }
    if (pid == 0) {
        char *bash_args[] = {"/usr/bin/bash", NULL};
        execv("/usr/bin/bash", bash_args);
        perror("init: execv /usr/bin/bash");
        _exit(EXIT_FAILURE);
    }
    return pid;
}

int main(int argc, char* argv[]){
    (void)argc;
    (void)argv;
    print_banner();

    /* Init is the tty's session/pgrp leader until the shell takes it over
       (see process_init()'s tty_set_foreground_pgrp seed). Ignore the
       job-control signals a stray Ctrl-C/Ctrl-Z could deliver to that
       foreground group during the brief window between fork() and bash's
       own initialize_job_control() claiming the terminal -- bash always
       reinstalls its own dispositions for these at startup regardless of
       what it inherits, so this only protects init itself. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    pid_t shell_pid = spawn_shell();

    for (;;) {
        int status;
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR) continue;
            /* ECHILD means init currently has no children at all, which
               shouldn't happen since the shell is always one -- respawn
               defensively rather than spin. */
            shell_pid = spawn_shell();
            continue;
        }

        /* Any other reaped pid is an orphan the kernel reparented to init
           when its real parent exited first; just reaping it here is all
           that's needed. */
        if (reaped == shell_pid) {
            shell_pid = spawn_shell();
        }
    }
}
