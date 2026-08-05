#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <krnl/syscall.h>
#include <krnl/ttycheck.h>
#include <bits/ensure.h>
#include <frg/vector.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/allocator.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <abi-bits/seek-whence.h>

namespace mlibc{
    int sys_open(const char *pathname, int flags, mode_t mode, int *fd){
        auto result = do_syscall(SYS_FILE_OPEN, pathname, flags, mode);

        if(result < 0){
            return -result;
        }

        *fd = result;
        return 0;
    }

    int sys_openat(int dirfd, const char *pathname, int flags, mode_t mode, int *fd){
        auto result = do_syscall(SYS_OPENAT, dirfd, pathname, flags, mode);

        if(result < 0){
            return -result;
        }

        *fd = result;
        return 0;
    }

    int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read){
        auto result = do_syscall(SYS_FILE_READ, fd, buf, count);

        if(result < 0){
            *bytes_read = 0;
            return -result;
        }

        *bytes_read = result;
        return 0;
    }

    int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written){
        auto result = do_syscall(SYS_FILE_WRITE, fd, buf, count);

        if(result < 0){
            return -result;
        }

        *bytes_written = result;
        return 0;
    }

    int sys_seek(int fd, off_t offset, int whence, off_t *new_offset){
        auto result = do_syscall(SYS_FILE_SEEK, fd, offset, whence);

        if(result < 0){
            return -result;
        }

        *new_offset = result;
        return 0;
    }

    int sys_close(int fd){
        auto result = do_syscall(SYS_FILE_CLOSE, fd);

        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_flock(int fd, int options){
        // TODO
        __ensure(!"Not implemented");
        return 0;
    }

    int sys_ioctl(int fd, unsigned long request, void* arg, int* ptr_result){
        auto result = do_syscall(SYS_FILE_IOCTL, fd, request, arg);

        if(result < 0){
            return -result;
        }

        if(ptr_result != NULL){
            *ptr_result = result;
        }
        
        return 0;
    }

    int sys_open_dir(const char *path, int *handle){
        // TODO
        auto result = do_syscall(SYS_DIR_OPEN, path);
        if(result < 0){
            return -result;
        }
        *handle = result;
        return 0;
    }

    int sys_read_entries(int handle, void *buffer, size_t max_size, size_t *bytes_read){
        // TODO
        auto result = do_syscall(SYS_DIR_READ, handle, buffer, max_size);
        if(result < 0){
            *bytes_read = 0;
            return -result;
        }
        *bytes_read = result;
        return 0;
    }

    int sys_pread(int fd, void *buf, size_t n, off_t off, ssize_t *bytes_read){
        // TODO
        auto result = do_syscall(SYS_FILE_PREAD, fd, buf, n, off);
        if(result < 0){
            *bytes_read = 0;
            return -result;
        }
        *bytes_read = result;
        return 0;
    }

    int sys_pipe(int *fds, int flags){
        auto result = do_syscall(SYS_PIPE, fds, flags);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_mkdir(const char *path, mode_t mode) {
	    auto result = do_syscall(SYS_MKDIR, path, mode);
        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_mkfifoat(int dirfd, const char *path, mode_t mode) {
        auto result = do_syscall(SYS_MKFIFOAT, dirfd, path, mode);
        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_mount(const char *source, const char *target, const char *fstype, unsigned long flags, const void *data) {
        auto result = do_syscall(SYS_MOUNT, source, target, fstype, flags, data);
        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_umount2(const char *target, int flags) {
        auto result = do_syscall(SYS_UMOUNT, target, flags);
        if(result < 0){
            return -result;
        }

        return 0;
    }

#ifndef MLIBC_BUILDING_RTLD
    // In contrast to the isatty() library function, the sysdep function uses return value
    // zero (and not one) to indicate that the file is a terminal.
    int sys_isatty(int fd){
        struct winsize ws;
        auto result = do_syscall(SYS_FILE_IOCTL, fd, TIOCGWINSZ, &ws);
        if (result == TTY_CHECK_VAL) return 0;
        return ENOTTY;
    }

    int sys_ttyname(int fd, char *buf, size_t size) {
        if (sys_isatty(fd) != 0)
            return ENOTTY;
        const char *name = "/dev/tty0";
        if (size <= strlen(name))
            return ERANGE;
        strcpy(buf, name);
        return 0;
    }
#endif

    int sys_rmdir(const char *path){
        auto result = do_syscall(SYS_RMDIR, path, strlen(path));
        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_unlinkat(int dirfd, const char *path, int flags){
        auto result = do_syscall(SYS_UNLINKAT, dirfd, path, flags);
        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_rename(const char *path, const char *new_path){
        auto result = do_syscall(SYS_RENAME, path, new_path);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_renameat(int olddirfd, const char *old_path, int newdirfd, const char *new_path){
        auto result = do_syscall(SYS_RENAMEAT, olddirfd, old_path, newdirfd, new_path);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_symlink(const char *target_path, const char *link_path){
        auto result = do_syscall(SYS_SYMLINK, target_path, link_path);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_symlinkat(const char *target_path, int dirfd, const char *link_path){
        auto result = do_syscall(SYS_SYMLINKAT, target_path, dirfd, link_path);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_readlink(const char *path, void *buffer, size_t max_size, ssize_t *length){
        auto result = do_syscall(SYS_READLINK, path, buffer, max_size);
        if(result < 0){
            *length = 0;
            return -result;
        }
        *length = result;
        return 0;
    }

    int sys_fchmodat(int fd, const char *pathname, mode_t mode, int flags){
        auto result = do_syscall(SYS_FCHMODAT, fd, pathname, mode, flags);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_chmod(const char *pathname, mode_t mode){
        return sys_fchmodat(AT_FDCWD, pathname, mode, 0);
    }

    int sys_fchmod(int fd, mode_t mode){
        return sys_fchmodat(fd, "", mode, AT_EMPTY_PATH);
    }

    int sys_fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags){
        auto result = do_syscall(SYS_FCHOWNAT, dirfd, pathname, owner, group, flags);
        if(result < 0){
            return -result;
        }
        return 0;
    }

    int sys_readlinkat(int dirfd, const char *path, void *buffer, size_t max_size, ssize_t *length){
        auto result = do_syscall(SYS_READLINKAT, dirfd, path, buffer, max_size);
        if(result < 0){
            *length = 0;
            return -result;
        }
        *length = result;
        return 0;
    }

    int sys_stat(fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf){
        auto result = 0;
        switch(fsfdt){
            case fsfd_target::path:{
                /* -100 == AT_FDCWD: relative paths resolve against the cwd. */
                result = do_syscall(SYS_PATH_STAT, path, strlen(path), flags, statbuf, -100);
                break;
            }
            case fsfd_target::fd:{
                result = do_syscall(SYS_FD_STAT, fd, flags, statbuf);
                break;
            }
            case fsfd_target::fd_path:{
                /* fstatat(fd, path, ...): resolve path relative to the
                   already-open directory fd instead of the cwd. */
                result = do_syscall(SYS_PATH_STAT, path, strlen(path), flags, statbuf, fd);
                break;
            }
            default:{
                mlibc::infoLogger() << "mlibc warning: sys_stat: unsupported fsfd target" << frg::endlog;
                return EINVAL;
            }
        }

        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_getcwd(char* buffer, size_t size){
        auto result = do_syscall(SYS_GETCWD, buffer, size);

        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_dup(int fd, int flags, int *newfd) {
        // TODO
        auto result = do_syscall(SYS_DUP, fd, flags);

        if(result < 0){
            return -result;
        }

        *newfd = result;
        return 0;
    }
    
    int sys_dup2(int fd, int flags, int newfd) {
        // TODO
        auto result = do_syscall(SYS_DUP2, fd, newfd, flags);

        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set *exceptfds, const struct timespec* timeout, const sigset_t* sigmask, int *num_events) {
        (void)sigmask; // sigmask is not used in this implementation
        auto result = do_syscall(SYS_PSELECT, nfds, readfds, writefds, exceptfds, timeout, num_events);

        if(result < 0){
            return -result;
        }
        
        return 0;
    }

    int sys_fcntl(int fd, int request, va_list args, int* ptr_result){
        auto result = do_syscall(SYS_FCNTL, fd, request, va_arg(args, uint64_t));

        if(result < 0){
            return -result;
        }

        if(ptr_result != NULL){
            *ptr_result = result;
        }
        
        return 0;
    }

#ifndef MLIBC_BUILDING_RTLD
    int sys_chdir(const char *path){
        auto result = do_syscall(SYS_CHDIR, path, strlen(path));

        if(result < 0){
            return -result;
        }

        return 0;
    }

    int sys_fchdir(int fd){
        auto result = do_syscall(SYS_FCHDIR, fd);

        if(result < 0){
            return -result;
        }

        return 0;
    }
#endif
}