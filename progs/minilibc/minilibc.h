#ifndef _MINILIBC_H
#define _MINILIBC_H
#include <stdint.h>
#include <time.h>
#include <signal.h>

#define MAP_FAILED ((void *)-1)

#define MAP_SHARED 0x1
#define MAP_PRIVATE 0x2
#define MAP_ANONYMOUS 0x20

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_NONE 0x8

#define MS_SYNC 0x0
#define MS_ASYNC 0x1
#define MS_INVALIDATE 0x2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_WRONLY    0x1
#define O_RDONLY    0x2
#define O_RDWR      0x4
#define O_CREAT     0x8
#define O_EXCL      0x10
#define O_NOCTTY    0x20
#define O_TRUNC     0x40
#define O_APPEND    0x80
#define O_NONBLOCK  0x100
#define O_DSYNC     0x200
#define O_DIRECT    0x400
#define O_LARGEFILE 0x800
#define O_DIRECTORY 0x1000
#define O_NOFOLLOW  0x2000
#define O_CLOEXEC   0x4000

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_mode;
    uint64_t st_nlink;
    uint64_t st_uid;
    uint64_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

typedef struct stack {
    void * base;
    int flags;
    void * top;
} stack_t;

typedef struct stat stat_t;
typedef int16_t pid_t;
typedef int16_t gid_t;

void minilibc_init();
void sys_read(int fd, char * buffer, int size);
void sys_write(int fd, char * buffer, int size);
int sys_open(const char * path, int flags);
int sys_close(int fd);
int sys_stat(const char * path, struct stat * buf);
int sys_fstat(int fd, struct stat * buf);
int sys_seek(int fd, int offset, int whence);
void * sys_mmap(void * addr, uint64_t length, int prot, int flags, int fd, uint64_t offset);
int sys_munmap(void * addr, uint64_t length);
int sys_mprotect(void * addr, uint64_t length, int prot);
void sys_sched(void);
int sys_ioctl(int fd, uint64_t request, void * arg);
int sys_exit(int code);
int sys_tell(int fd);
int sys_fork();
int sys_execve(const char * filename, char ** argv, char ** envp);
int sys_waitpid(int pid, int * status, int options);
int sys_getpid();
int sys_nanosleep(struct timespec * duration, struct timespec * rem);
int sys_setgid(gid_t gid);
int sys_debug();
int sys_kill(int pid, int sig);
int sys_sigaction(int signum, const struct sigaction * act, struct sigaction * oldact);

#endif