#include <minilibc.h>
#include <stdio.h>
#include <string.h>

#define COMMAND_MAX_LENGTH 256

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

int test_shell_command(int argc, char* argv[]) {
    int pid = sys_getpid();
    printf("Test shell command executed! PID: %d\n", pid);
    //Print arguments
    for (int i = 0; i < argc; i++) {
        printf(" Argv[%d]: %s\n", i, argv[i]);
    }
    return 0;
}

int exit_shell_command(int argc, char* argv[]) {
    int pid = sys_getpid();
    printf("Exiting shell now. PID: %d\n", pid);
    sys_exit(0);
    return 0; // Never reached
}

struct command {
    const char* name;
    int (*handler)(int argc, char* argv[]);
};

struct command commands[] = {
    { "exit", exit_shell_command },
    { "test", test_shell_command },
    { NULL, NULL }
};

// Try executing a program by name
int process_execution(const char* filename, const char *const argv[]) {
    printf("Attempting to execute program: %s\n", filename);
    printf("Arguments:\n");
    for (int i = 0; argv[i] != NULL; i++) {
        printf(" Argv[%d]: %s\n", i, argv[i]);
    }
    //Fork, execve, waitpid
    int pid = sys_fork();
    if (pid == 0) {
        //Child process
        char* const envp[] = { NULL };
        int result = sys_execve(filename, (char **)argv, (char **)envp);
        if (result < 0) {
            sys_exit(1);
        }
        //If execve returns, there was an error
        printf("Failed to execute program: %s\n", filename);
        sys_exit(1);
    } else if (pid > 0) {
        //Parent process
        int status;
        sys_waitpid(pid, &status, 0);
        printf("Program %s exited with status %d\n", filename, status);
    } else {
        //Fork failed
        printf("Failed to fork process for executing: %s\n", filename);
        return -1;
    }
    return 0;
}

int process_command(const char* command) {
    // For now, just print the command
    printf("Processing command: %s\n", command);
    //Extract the command name (first word) and arguments (other words)
    char cmd_copy[COMMAND_MAX_LENGTH];
    char * argv[COMMAND_MAX_LENGTH];
    strncpy(cmd_copy, command, sizeof(cmd_copy));
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    int argc = 0;
    char * token = strtok(cmd_copy, " ");
    while (token != NULL && argc < COMMAND_MAX_LENGTH - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
    if (argc == 0) {
        return -1; // No command entered
    }
    //Find and execute the command
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            return commands[i].handler(argc, argv);
        }
    }
    
    //If command not found, try executing it as a program
    return process_execution(argv[0], (const char *const *)argv);
}

void loop() {
    char command_buffer[COMMAND_MAX_LENGTH];
    char c;
    printf("Simple Shell. Type 'exit' to quit.\n");
    printf("> ");
    memset(command_buffer, 0, sizeof(command_buffer));
    while (1) {
        sys_read(0, &c, 1);
        printf("%c", c); // Echo the character
        if (c == '\n' || c == '\r') {
            //Make sure the buffer is null-terminated
            command_buffer[strlen(command_buffer)] = '\0';
            process_command(command_buffer);
            memset(command_buffer, 0, sizeof(command_buffer));
            printf("> ");
        } else {
            size_t len = strlen(command_buffer);
            if (len < sizeof(command_buffer) - 1) {
                command_buffer[len] = c;
                command_buffer[len + 1] = '\0';
            }
        }
    }
}

int main(int argc, char* argv[], char* envp[]) {
    minilibc_init();
    print_args(argc, argv, envp);
    int pid = sys_getpid();
    printf("Hello from shell! PID: %d\n", pid);
    loop();
    return 0;
}