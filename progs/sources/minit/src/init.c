#include <minilibc.h>
#include <stdio.h>
#include <string.h>

void print_args(int argc, char* argv[], char* envp[]) {
    printf("Argc: %d", argc);
    for (int i = 0; i < argc; i++) {
        printf("Argv[%d]: %s", i, argv[i]);
    }

    int envc = 0;
    if (envp != 0)
        for (envc = 0; envp[envc] != NULL; envc++);

    for (int i = 0; i < envc; i++) {
        printf("Envp[%d]: %s", i, envp[i]);
    }
    printf("\n");
}

void __attribute__ ((noinline)) fork_stress() {
    int pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in stress test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        int local_counter = sys_getpid();
        while (1) {
            printf("P%d\n", local_counter);
            sys_sched();
        }
    }
}

int main(int argc, char* argv[], char* envp[]) {
    minilibc_init();
    print_args(argc, argv, envp);
    printf("Hello from userspace!\n");
    int fd = sys_open("/lorem-ipsum.txt", O_RDWR);
    if (fd < 0) {
        printf("Failed to open file. Exiting.\n");
        sys_exit(1);
    }
    sys_seek(fd, 0, SEEK_END);
    int size = sys_tell(fd);
    sys_seek(fd, 0, SEEK_SET);
    char * buffer = (char *)sys_mmap(0, size + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    printf("File size: %d\n", size);
    printf("Buffer address: %p\n", buffer);
    printf("File contents:\n");
    buffer[size] = '\0'; // Null-terminate the buffer
    for (int i = 0; i < size; i++) {
        printf("%c", buffer[i]);
    }
    printf("\n");
    sys_munmap(buffer, size + 1);
    sys_close(fd);

    printf("Testing fork and execve:\n");
    for (int i = 0; i < 300; i++) {
        fork_stress();
    }
    printf("Entering scheduling loop.\n");
    while (1) {
        printf("IM A RESOURCE HOG!\n");
    }
    return 0;
}