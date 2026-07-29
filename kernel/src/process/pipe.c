#include <krnl/process/pipe.h>
#include <krnl/process/process.h>
#include <krnl/process/signals.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>

#define PIPE_BUF_SIZE 65536

/* Distinct wait channels for "data available" vs "space available",
   derived from the pipe's address like futex/serial already do for
   their own wait channels. */
static int64_t pipe_read_channel(pipe_t *p)  { return (int64_t)(uintptr_t)p; }
static int64_t pipe_write_channel(pipe_t *p) { return ~(int64_t)(uintptr_t)p; }

status_t pipe_create(vfs_file_descriptor_t *read_fd, vfs_file_descriptor_t *write_fd) {
    pipe_t *p = kmalloc(sizeof(pipe_t));
    if (!p) return FAILURE;
    p->buffer = kmalloc(PIPE_BUF_SIZE);
    if (!p->buffer) {
        kfree(p);
        return FAILURE;
    }
    p->capacity = PIPE_BUF_SIZE;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;

    memset(read_fd, 0, sizeof(*read_fd));
    read_fd->valid = 1;
    read_fd->pipe = p;
    read_fd->flags = O_RDONLY;
    read_fd->native_path = kmalloc(1); /* dummy: marks the fd slot as occupied */

    memset(write_fd, 0, sizeof(*write_fd));
    write_fd->valid = 1;
    write_fd->pipe = p;
    write_fd->flags = O_WRONLY;
    write_fd->native_path = kmalloc(1);

    return SUCCESS;
}

ssize_t pipe_read(vfs_file_descriptor_t *fd, void *buf, size_t count) {
    pipe_t *p = fd->pipe;
    if (count == 0) return 0;

    thread_t *thread = process_get_current_thread();
    while (p->count == 0) {
        if (p->writers == 0) return 0; /* EOF: no writers left */
        if (fd->flags & O_NONBLOCK) return -EAGAIN;
        if (!sleep(thread, pipe_read_channel(p))) return -EINTR;
    }

    size_t chunk = count < p->count ? count : p->count;
    uint8_t *out = (uint8_t *)buf;
    for (size_t i = 0; i < chunk; i++) {
        out[i] = p->buffer[p->head];
        p->head = (p->head + 1) % p->capacity;
    }
    p->count -= chunk;
    wakeup(pipe_write_channel(p));
    return (ssize_t)chunk;
}

ssize_t pipe_write(vfs_file_descriptor_t *fd, const void *buf, size_t count) {
    pipe_t *p = fd->pipe;
    if (count == 0) return 0;
    if (p->readers == 0) return -EPIPE;

    thread_t *thread = process_get_current_thread();
    const uint8_t *in = (const uint8_t *)buf;
    size_t written = 0;
    while (written < count) {
        if (p->readers == 0) {
            return written > 0 ? (ssize_t)written : -EPIPE;
        }
        size_t free_space = p->capacity - p->count;
        if (free_space == 0) {
            if (fd->flags & O_NONBLOCK) {
                return written > 0 ? (ssize_t)written : -EAGAIN;
            }
            if (!sleep(thread, pipe_write_channel(p)))
                return written > 0 ? (ssize_t)written : -EINTR;
            continue;
        }
        size_t chunk = (count - written) < free_space ? (count - written) : free_space;
        for (size_t i = 0; i < chunk; i++) {
            p->buffer[p->tail] = in[written + i];
            p->tail = (p->tail + 1) % p->capacity;
        }
        p->count += chunk;
        written += chunk;
        wakeup(pipe_read_channel(p));
    }
    return (ssize_t)written;
}

status_t pipe_fstat(vfs_file_descriptor_t *fd, vfs_stat_t *buf) {
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = 0x1000 | 0600; /* S_IFIFO | rw------- */
    buf->st_nlink = 1;
    buf->st_size = (long)fd->pipe->count;
    buf->st_blksize = (long)fd->pipe->capacity;
    return SUCCESS;
}

void pipe_dup(vfs_file_descriptor_t *fd) {
    if (!fd || !fd->pipe) return;
    if (fd->flags & O_WRONLY) fd->pipe->writers++;
    else                      fd->pipe->readers++;
}

void pipe_close(vfs_file_descriptor_t *fd) {
    pipe_t *p = fd->pipe;
    if (!p) return;

    if (fd->flags & O_WRONLY) {
        if (p->writers > 0) p->writers--;
        if (p->writers == 0) wakeup(pipe_read_channel(p)); /* readers see EOF */
    } else {
        if (p->readers > 0) p->readers--;
        if (p->readers == 0) wakeup(pipe_write_channel(p)); /* writers see EPIPE */
    }

    if (fd->native_path) {
        kfree(fd->native_path);
        fd->native_path = NULL;
    }
    fd->pipe = NULL;
    fd->valid = 0;

    if (p->readers == 0 && p->writers == 0) {
        kfree(p->buffer);
        kfree(p);
    }
}
