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
    /* Accmode-based (not a single O_WRONLY bit test) so an O_RDWR fd -- only
       possible for a named FIFO opened that way, anonymous pipe ends are
       always pure O_RDONLY/O_WRONLY -- bumps both refcounts. */
    int accmode = fd->flags & 3;
    if (accmode != O_WRONLY) fd->pipe->readers++;
    if (accmode != O_RDONLY) fd->pipe->writers++;
}

/* --- Named FIFO support --------------------------------------------------
   A path created by mkfifo() lets independent open() calls rendezvous on
   one shared pipe_t, exactly like the two ends handed out by pipe_create()
   above -- the only new part is finding/creating that shared pipe_t and
   blocking at open() time until a peer shows up. Keyed by (mount,
   native_path) rather than inode number: x1fs never assigns real per-file
   inode numbers (every x1fs_fstat() call reports st_ino == 0), so inode
   identity isn't reliably unique there. */

typedef struct fifo_entry {
    vfs_mount_t *mount;
    char *native_path;
    pipe_t *pipe;
    struct fifo_entry *next;
} fifo_entry_t;

static fifo_entry_t *fifo_registry = NULL;

static pipe_t *fifo_lookup(vfs_mount_t *mount, const char *native_path) {
    for (fifo_entry_t *e = fifo_registry; e; e = e->next) {
        if (e->mount == mount && strcmp(e->native_path, native_path) == 0)
            return e->pipe;
    }
    return NULL;
}

static status_t fifo_register(vfs_mount_t *mount, const char *native_path, pipe_t *p) {
    fifo_entry_t *e = kmalloc(sizeof(fifo_entry_t));
    if (!e) return FAILURE;
    e->native_path = kmalloc(strlen(native_path) + 1);
    if (!e->native_path) { kfree(e); return FAILURE; }
    strcpy(e->native_path, native_path);
    e->mount = mount;
    e->pipe = p;
    e->next = fifo_registry;
    fifo_registry = e;
    return SUCCESS;
}

/* No-op if `p` was never registered (i.e. it backs an anonymous pipe, not
   a FIFO) -- lets pipe_close() call this unconditionally for every pipe_t. */
static void fifo_unregister(pipe_t *p) {
    fifo_entry_t **cur = &fifo_registry;
    while (*cur) {
        if ((*cur)->pipe == p) {
            fifo_entry_t *dead = *cur;
            *cur = dead->next;
            kfree(dead->native_path);
            kfree(dead);
            return;
        }
        cur = &(*cur)->next;
    }
}

/* Frees `p` and drops its registry entry once both ends are closed. Called
   from pipe_close() (every pipe, FIFO or anonymous) and from fifo_open()'s
   own abort paths (a blocked opener interrupted before any peer arrived). */
static void fifo_release_if_orphaned(pipe_t *p) {
    if (p->readers == 0 && p->writers == 0) {
        fifo_unregister(p);
        kfree(p->buffer);
        kfree(p);
    }
}

int64_t fifo_open(vfs_mount_t *mount, const char *native_path, int flags, vfs_file_descriptor_t *fd) {
    pipe_t *p = fifo_lookup(mount, native_path);
    if (!p) {
        p = kmalloc(sizeof(pipe_t));
        if (!p) return -ENOMEM;
        p->buffer = kmalloc(PIPE_BUF_SIZE);
        if (!p->buffer) {
            kfree(p);
            return -ENOMEM;
        }
        p->capacity = PIPE_BUF_SIZE;
        p->head = p->tail = p->count = 0;
        p->readers = 0;
        p->writers = 0;
        if (fifo_register(mount, native_path, p) != SUCCESS) {
            kfree(p->buffer);
            kfree(p);
            return -ENOMEM;
        }
    }

    int accmode = flags & 3; /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
    int wants_read  = (accmode != O_WRONLY);
    int wants_write = (accmode != O_RDONLY);

    /* Mark our presence immediately (before any blocking) so a concurrent
       peer's own open() sees us and doesn't block/fail needlessly. */
    if (wants_read) {
        p->readers++;
        if (p->readers == 1) wakeup(pipe_write_channel(p)); /* a blocked writer may now proceed */
    }
    if (wants_write) {
        p->writers++;
        if (p->writers == 1) wakeup(pipe_read_channel(p)); /* a blocked reader may now proceed */
    }

    thread_t *thread = process_get_current_thread();

    if (wants_read && !wants_write) {
        /* O_RDONLY: POSIX allows immediate success with zero writers under
           O_NONBLOCK (subsequent read() just sees EOF); otherwise block
           until a writer connects. */
        while (p->writers == 0) {
            if (flags & O_NONBLOCK) break;
            if (!sleep(thread, pipe_read_channel(p))) {
                p->readers--;
                fifo_release_if_orphaned(p);
                return -EINTR;
            }
        }
    } else if (wants_write && !wants_read) {
        /* O_WRONLY: fails immediately (-ENXIO) under O_NONBLOCK with no
           reader present; otherwise blocks until one connects. */
        if (p->readers == 0) {
            if (flags & O_NONBLOCK) {
                p->writers--;
                fifo_release_if_orphaned(p);
                return -ENXIO;
            }
            while (p->readers == 0) {
                if (!sleep(thread, pipe_write_channel(p))) {
                    p->writers--;
                    fifo_release_if_orphaned(p);
                    return -EINTR;
                }
            }
        }
    }
    /* O_RDWR: never blocks (standard POSIX/Linux extension, avoids the
       self-deadlock a strict reader-then-writer wait would otherwise risk). */

    fd->pipe = p;
    return 0;
}

void pipe_close(vfs_file_descriptor_t *fd) {
    pipe_t *p = fd->pipe;
    if (!p) return;

    int accmode = fd->flags & 3;
    if (accmode != O_WRONLY) {
        if (p->readers > 0) p->readers--;
        if (p->readers == 0) wakeup(pipe_write_channel(p)); /* writers see EPIPE */
    }
    if (accmode != O_RDONLY) {
        if (p->writers > 0) p->writers--;
        if (p->writers == 0) wakeup(pipe_read_channel(p)); /* readers see EOF */
    }

    if (fd->native_path) {
        kfree(fd->native_path);
        fd->native_path = NULL;
    }
    fd->pipe = NULL;
    fd->valid = 0;

    fifo_release_if_orphaned(p);
}
