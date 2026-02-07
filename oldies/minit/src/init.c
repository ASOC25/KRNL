#include <minilibc.h>
#include <stdio.h>
#include <string.h>
#include <wait.h>
#include <signal.h>

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

void __attribute__ ((noinline)) waitpid_test() {
    //Test waitpid all cases
    printf("testing waitpid with pid < -1 (gid = abs(pid))\n");
    sys_setgid(100); // Set gid to 100 for testing
    int pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in waitpid test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        printf("child process sleeping for 2 seconds...\n");
        sys_nanosleep(&(struct timespec){.tv_sec=2, .tv_nsec=0}, NULL);
        printf("child process exiting with code 42.\n");
        sys_exit(42);
    } else {
        int status;
        int ret = sys_waitpid(-100, &status, 0);
        if (ret != pid) {
            printf("waitpid test failed: expected return pid %d, got %d\n", pid, ret);
            while (1);
        } else {
            printf("STATUS: %lx\n", status);
            int exit_code = WEXITSTATUS(status);
            if (exit_code != 42) {
                printf("waitpid test failed: expected exit code 42, got %d\n", exit_code);
                while (1);
                return;
            } else {
                printf("Exit code verified: %d\n", exit_code);
                printf("waitpid test passed for pid < -1 case.\n");
            }
        }
    }
    printf("testing waitpid with pid < -1 completed.\n");
    printf("testing waitpid with pid == -1\n");
    pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in waitpid test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        printf("child process sleeping for 2 seconds...\n");
        sys_nanosleep(&(struct timespec){.tv_sec=2, .tv_nsec=0}, NULL);
        sys_exit(43);
    } else {
        int status;
        int ret = sys_waitpid(-1, &status, 0);
        if (ret != pid) {
            printf("waitpid test failed: expected return pid %d, got %d\n", pid, ret);
        } else {
            printf("waitpid test passed for pid == -1 case.\n");
        }
    }
    printf("testing waitpid with pid == -1 completed.\n");
    printf("testing waitpid with pid == 0\n");
    pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in waitpid test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        printf("child process sleeping for 2 seconds...\n");
        sys_nanosleep(&(struct timespec){.tv_sec=2, .tv_nsec=0}, NULL);
        sys_exit(44);
    } else {
        int status;
        int ret = sys_waitpid(0, &status, 0);
        if (ret != pid) {
            printf("waitpid test failed: expected return pid %d, got %d\n", pid, ret);
        } else {
            printf("waitpid test passed for pid == 0 case.\n");
        }
    }
    printf("testing waitpid with pid == 0 completed.\n");
    printf("testing waitpid with pid > 0\n");
    pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in waitpid test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        printf("child process sleeping for 2 seconds...\n");
        sys_nanosleep(&(struct timespec){.tv_sec=2, .tv_nsec=0}, NULL);
        sys_exit(45);
    } else {
        int status;
        int ret = sys_waitpid(pid, &status, 0);
        if (ret != pid) {
            printf("waitpid test failed: expected return pid %d, got %d\n", pid, ret);
        } else {
            printf("waitpid test passed for pid > 0 case.\n");
        }
    }
    printf("testing waitpid with pid > 0 completed.\n");
}

void __attribute__ ((noinline)) fork_stress() {
    int pid = sys_fork();
    int pid2 = sys_getpid();
    if (pid < 0) {
        printf("Fork failed in stress test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        int local_counter = sys_getpid();
        while (1) {
            printf("Entering child process %d\n", local_counter);
            struct timespec duration = { .tv_sec = 5, .tv_nsec = 0 };
            sys_nanosleep(&duration, NULL); // Sleep for 5 seconds
            printf("Exiting child process %d\n", local_counter);
            sys_exit(0);
        }
    }
    if (pid2 != 102) {
        while (1) {
            printf("Parent process PID mismatch in stress test. Expected 102, got %d\n", pid2);
        }
    }
    int status;
    int ret = sys_waitpid(pid, &status, 0);
    if (ret != pid) {
        printf("Fork stress test failed: expected return pid %d, got %d\n", pid, ret);
    } else {
        printf("Fork stress test child process %d exited successfully.\n", pid);
    }
    printf("Fork stress test completed.\n");
}

void __attribute__ ((noinline)) fork_execve_exit_test() {
    // This test should fork a process, execve the shell prorgam which in turn will sleep for 5 seconds and exit with code 55
    // We don't want to waitpid here, just verify that fork and execve work correctly
    int pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in fork_execve_exit_test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        const char* new_argv[] = { "shell.elf", NULL };
        const char* new_envp[] = { "SHELL_ENV=MINISHELL", NULL };
        sys_execve("/shell.elf", (char **)new_argv, (char **)new_envp);
        // If execve returns, it failed
        printf("execve failed in fork_execve_exit_test.\n");
        sys_exit(1);
    }
    // Parent process
    printf("Fork execve exit test: child process %d created.\n", pid);
}

void kill_signal_handler(int signum) {
    // Simply exit the process with code 128 + signum
    int pid = sys_getpid();
    printf("Process %d received signal %d. Ignoring!\n", pid, signum);
}

void __attribute__ ((noinline)) kill_test() {
    //Create a new process that sleeps indefinitely
    //Then send it a SIGKILL and verify it exits
    struct sigaction sa = {0};
    sa.sa_handler = kill_signal_handler;
    sa.sa_flags = 0;
    memset(&sa.sa_mask, 0, sizeof(sa.sa_mask));
    if (sys_sigaction(9, &sa, NULL) != 0) {
        printf("Failed to set signal handler for SIGKILL in kill_test.\n");
        return;
    }

    int pid = sys_fork();
    if (pid < 0) {
        printf("Fork failed in kill_test.\n");
        return;
    } else if (pid == 0) {
        // Child process
        while (1) {
            struct timespec duration = { .tv_sec = 1, .tv_nsec = 0 };
            sys_nanosleep(&duration, NULL); // Sleep for 1 second
        }
    } else {
        // Parent process
        struct timespec duration = { .tv_sec = 2, .tv_nsec = 0 };
        sys_nanosleep(&duration, NULL); // Sleep for 2 seconds to ensure child is sleeping
        printf("Sending SIGKILL to process %d\n", pid);
        sys_kill(pid, 9); // Send SIGKILL
        int status;
        int ret = sys_waitpid(pid, &status, 0);
        if (ret != pid) {
            printf("Kill test failed: expected return pid %d, got %d\n", pid, ret);
        } else {
            if (WIFSIGNALED(status) && WTERMSIG(status) == 9) {
                printf("Kill test passed: process %d terminated by SIGKILL.\n", pid);
            } else {
                printf("Kill test failed: process %d did not terminate by SIGKILL.\n", pid);
                printf("It ended by signal number %d\n", WTERMSIG(status));
            }
        }
    }
}

void init() {
    const char* new_argv[] = { "shell.elf", NULL };
    const char* new_envp[] = { "SHELL_ENV=MINISHELL", NULL };
    printf("Init process execveing shell.elf...\n");
    sys_execve("/shell.elf", (char **)new_argv, (char **)new_envp);
    printf("Init process execve failed!\n");
    sys_exit(1);
    //printf("Starting fork stress test (press Ctrl+C to stop)...\n");
    //fork_stress();
    //printf("Starting waitpid test...\n");
    //waitpid_test();
    //printf("Entering scheduling loop.\n");
//
    //printf("Starting fork_execve_exit_test 100 times...\n");
    //for (int i = 0; i < 100; i++)
    //    fork_execve_exit_test();
//
    ////Now waitpid for all children
    //printf("Waiting for all child processes to exit...\n");
    //while (1) {
    //    int status;
    //    int ret = sys_waitpid(-1, &status, 0);
    //    if (ret < 0) {
    //        printf("No more child processes to wait for. Exiting wait loop.\n");
    //        break;
    //    } else {
    //        printf("Reaped child process %d with status %lx\n", ret, status);
    //    }
    //}
//
    //printf("All tests completed. Exiting main process.\n");
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

    //Create idle process:
    int idle_pid = sys_fork();
    if (idle_pid < 0) {
        printf("Failed to create idle process. Exiting.\n");
        sys_exit(1);
    } else if (idle_pid == 0) {
        //waitpid_test();
        //kill_test();
        init();
        printf("Main process exiting.\n");
        sys_exit(0);
    }
    printf("Idle process created with PID %d\n", idle_pid);

    //Idle process
    while (1) {}
    return 0; //Never reached
}