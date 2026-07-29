#include <krnl/fs/memdev/memdev.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/debug/perf.h>
#include <krnl/arch/x86/hpet.h>

/* Non-cryptographic xorshift64* PRNG seeded from the TSC/HPET and re-mixed
   with a fresh TSC sample on every call. Good enough to back /dev/random
   and /dev/urandom for ordinary userspace use (mktemp, $RANDOM, ASLR-style
   seeds) -- not a security-grade CSPRNG, and both devices behave the same
   (neither ever blocks), matching modern Linux's post-5.6 behavior. */
static uint64_t memdev_prng_state = 0;

static void memdev_fill_random(uint8_t *buf, size_t count) {
    if (memdev_prng_state == 0) {
        memdev_prng_state = perf_rdtsc() ^ (hpet_get_current_time() * 2685821657736338717ULL) ^ 0x9E3779B97F4A7C15ULL;
        if (memdev_prng_state == 0) memdev_prng_state = 0xA5A5A5A5A5A5A5A5ULL;
    }
    for (size_t i = 0; i < count; i++) {
        memdev_prng_state ^= perf_rdtsc();
        memdev_prng_state ^= memdev_prng_state << 13;
        memdev_prng_state ^= memdev_prng_state >> 7;
        memdev_prng_state ^= memdev_prng_state << 17;
        buf[i] = (uint8_t)(memdev_prng_state >> 24);
    }
}

ssize_t memdev_read(device_major_t major, device_minor_t minor, const char *path, size_t skip, void *buf, size_t count) {
    (void)major;
    (void)path;
    (void)skip;
    switch (minor) {
        case MEMDEV_MINOR_NULL:
            return 0; /* always EOF */
        case MEMDEV_MINOR_ZERO:
            memset(buf, 0, count);
            return count;
        case MEMDEV_MINOR_RANDOM:
        case MEMDEV_MINOR_URANDOM:
            memdev_fill_random(buf, count);
            return count;
        default:
            return -ENODEV;
    }
}

ssize_t memdev_write(device_major_t major, device_minor_t minor, const char *path, size_t skip, const void *buf, size_t count) {
    (void)major;
    (void)minor;
    (void)path;
    (void)skip;
    (void)buf;
    /* null/zero/random/urandom all discard writes and report success */
    return count;
}

status_t memdev_detect(device_major_t major, device_minor_t minor) {
    (void)minor;
    return (major == MEMDEV_DRIVER_MAJOR) ? SUCCESS : FAILURE;
}

status_t memdev_fstat(device_major_t major, device_minor_t minor, const char *path, vfs_stat_t *buf) {
    (void)path;
    memset(buf, 0, sizeof(vfs_stat_t));
    buf->st_mode = 0x21B6; /* S_IFCHR | 0666 */
    buf->st_nlink = 1;
    /* st_dev/st_ino must differ across distinct devices (major/minor) —
       otherwise callers that compare stat() results to tell devices apart
       (e.g. glibc/gnulib's SAME_INODE, used by GNU grep to detect "stdout
       is /dev/null") spuriously see every character device as the same
       file. */
    buf->st_dev  = (uint64_t)major;
    buf->st_ino  = (uint64_t)minor + 1;
    buf->st_rdev = ((uint64_t)major << 8) | (uint64_t)(minor & 0xff);
    return SUCCESS;
}

status_t memdev_ioctl(device_major_t major, device_minor_t minor, const char *path, uint64_t request, void *arg) {
    (void)major;
    (void)minor;
    (void)path;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

void memdev_init(void) {
    vfs_fs_t *memdev_ops = (vfs_fs_t *)kmalloc(sizeof(vfs_fs_t));
    if (!memdev_ops) {
        panic("memdev_init: Unable to allocate memory for memdev operations");
    }

    memset(memdev_ops->name, 0, 32);
    strcpy(memdev_ops->name, "MEMDEV");
    memdev_ops->read = memdev_read;
    memdev_ops->write = memdev_write;
    memdev_ops->detect = memdev_detect;
    memdev_ops->fstat = memdev_fstat;
    memdev_ops->ioctl = memdev_ioctl;
    memdev_ops->readdir = NULL;
    memdev_ops->next = NULL;

    status_t result = vfs_register_fs(memdev_ops);
    if (result != SUCCESS) {
        panic("memdev_init: Failed to register memdev with VFS");
    }
}
