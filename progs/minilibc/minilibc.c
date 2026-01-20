
#include <stdint.h>
#include <minilibc.h>
#include <auxv.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <signal.h>

struct minilibc_data {
    uint8_t loaded;
};

struct minilibc_data minilibc_data;

void minilibc_init() {
    // Initialize the mini libc
    if (minilibc_data.loaded) {
        return;
    }
    minilibc_data.loaded = 1;
}

static inline uint64_t syscall(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    uint64_t result;

    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    register uint64_t r9 __asm__("r9") = arg6;

    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(function),
          "D"(arg1),
          "S"(arg2),
          "d"(arg3),
          "r"(r10),
          "r"(r8),
          "r"(r9)
        : "memory"
    );

    return result;
}

void sys_read(int fd, char * buffer, int size) {
    syscall(0, (uint64_t)fd, (uint64_t)buffer, (uint64_t)size, 0, 0, 0);
}
void sys_write(int fd, char * buffer, int size) {
    syscall(1, (uint64_t)fd, (uint64_t)buffer, (uint64_t)size, 0, 0, 0);
}
int sys_open(const char * path, int flags) {
    return (int)syscall(2, (uint64_t)path, (uint64_t)flags, 0, 0, 0, 0);
}
int sys_close(int fd) {
    return (int)syscall(3, (uint64_t)fd, 0, 0, 0, 0, 0);
}
int sys_stat(const char * path, struct stat * buf) {
    return (int)syscall(4, (uint64_t)path, (uint64_t)buf, 0, 0, 0, 0);
}
int sys_fstat(int fd, struct stat * buf) {
    return (int)syscall(5, (uint64_t)fd, (uint64_t)buf, 0, 0, 0, 0);
}
int sys_seek(int fd, int offset, int whence) {
    return (int)syscall(6, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence, 0, 0, 0);
}
int sys_ioctl(int fd, uint64_t request, void * arg) {
    return (int)syscall(7, (uint64_t)fd, (uint64_t)request, (uint64_t)arg, 0, 0, 0);
}
int sys_exit(int code) {
    return (int)syscall(8, (uint64_t)code, 0, 0, 0, 0, 0);
}
void * sys_mmap(void * addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    return (void *)syscall(9, (uint64_t)addr, (uint64_t)length, (uint64_t)prot, (uint64_t)flags, (uint64_t)fd, (uint64_t)offset);
}
int sys_munmap(void * addr, uint64_t length) {
    return (int)syscall(10, (uint64_t)addr, (uint64_t)length, 0, 0, 0, 0);
}
int sys_mprotect(void * addr, uint64_t length, int prot) {
    return (int)syscall(11, (uint64_t)addr, (uint64_t)length, (uint64_t)prot, 0, 0, 0);
}
void sys_sched(void) {
    syscall(12, 0, 0, 0, 0, 0, 0);
}
int sys_pread(int fd, void * buf, size_t count, off_t offset) {
    return (int)syscall(13, (uint64_t)fd, (uint64_t)buf, (uint64_t)count, (uint64_t)offset, 0, 0);
}
int sys_tell(int fd) {
    return (int)syscall(14, (uint64_t)fd, 0, 0, 0, 0, 0);
}
int sys_fork() {
    return (int)syscall(15, 0, 0, 0, 0, 0, 0);
}
int sys_execve(const char * filename, char ** argv, char ** envp) {
    return (int)syscall(16, (uint64_t)filename, (uint64_t)argv, (uint64_t)envp, 0, 0, 0);
}
int sys_waitpid(int pid, int * status, int options) {
    return (int)syscall(17, (uint64_t)pid, (uint64_t)status, (uint64_t)options, 0, 0, 0);
}

int sys_getpid() {
    return (int)syscall(18, 0, 0, 0, 0, 0, 0);
}

int sys_nanosleep(struct timespec * duration, struct timespec * rem) {
    return (int)syscall(19, (uint64_t)duration, (uint64_t)rem, 0, 0, 0, 0);
}

int sys_setgid(gid_t gid) {
    return (int)syscall(20, (uint64_t)gid, 0, 0, 0, 0, 0);
}
int sys_debug() {
    return (int)syscall(47, 0, 0, 0, 0, 0, 0);
}
int sys_kill(int pid, int sig) {
    return (int)syscall(48, (uint64_t)pid, (uint64_t)sig, 0, 0, 0, 0);
}
int sys_sigaction(int signum, const struct sigaction * act, struct sigaction * oldact) {
    return (int)syscall(50, (uint64_t)signum, (uint64_t)act, (uint64_t)oldact, 0, 0, 0);
}