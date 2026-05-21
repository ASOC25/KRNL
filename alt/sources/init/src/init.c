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

int main(int argc, char* argv[]){
    print_banner();
    char *bash_args[] = {"/usr/bin/bash", NULL};
    execv("/usr/bin/bash", bash_args);
    perror("execv /usr/bin/bash");
    return EXIT_FAILURE;
}