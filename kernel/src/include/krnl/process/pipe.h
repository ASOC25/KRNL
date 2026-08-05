#ifndef _KRNL_PROCESS_PIPE_H
#define _KRNL_PROCESS_PIPE_H

#include <krnl/vfs/vfs.h>

typedef struct pipe {
    uint8_t *buffer;
    size_t capacity;
    size_t head;   /* next byte to read */
    size_t tail;   /* next free slot to write */
    size_t count;  /* bytes currently buffered */
    int readers;
    int writers;
} pipe_t;

/* Fills read_fd/write_fd with the two ends of a freshly allocated pipe. */
status_t pipe_create(vfs_file_descriptor_t *read_fd, vfs_file_descriptor_t *write_fd);

ssize_t  pipe_read(vfs_file_descriptor_t *fd, void *buf, size_t count);
ssize_t  pipe_write(vfs_file_descriptor_t *fd, const void *buf, size_t count);
status_t pipe_fstat(vfs_file_descriptor_t *fd, vfs_stat_t *buf);

/* Increments the refcount for whichever end `fd` represents (fork/dup). */
void pipe_dup(vfs_file_descriptor_t *fd);

/* Decrements the refcount for whichever end `fd` represents, wakes the
   peer end if it just hit zero, and frees the pipe once both ends are
   closed. Also frees fd->native_path and clears fd. */
void pipe_close(vfs_file_descriptor_t *fd);

/* Opens the named FIFO living at (mount, native_path), creating its shared
   pipe_t on first open. Implements POSIX open-time rendezvous: O_RDONLY
   blocks until a writer is present, O_WRONLY blocks until a reader is
   present (or fails -ENXIO under O_NONBLOCK), O_RDWR never blocks. On
   success sets fd->pipe and returns 0; on failure returns a negative errno
   and leaves *fd otherwise untouched (caller still owns fd->native_path). */
int64_t fifo_open(vfs_mount_t *mount, const char *native_path, int flags, vfs_file_descriptor_t *fd);

#endif
