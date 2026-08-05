#include <krnl/fs/x1fs/x1fs.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/devices/devices.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/allocator.h>
#include <krnl/drivers/ramdisk/ramdisk.h>

// Note: The virtual path is the full path assuming the ramdisk is mounted at /
// - uint32_t SIGNATURE (X1FS) - 0x58314653
// - uint32_t NUM_ENTRIES
// - uint64_t OFFSET_TO_STRING_TABLE (calculated as 4 + 4 + 8 + 8 + NUM_ENTRIES * 64)
// - uint64_t OFFSET_TO_FILE_DATA (calculated as OFFSET_TO_STRING_TABLE + size of string table aligned to 512 bytes)
// - FsEntry[NUM_ENTRIES] (each entry is 64 bytes: [8 bytes] string table index, [16 bytes] md5 hash, [8 bytes] file address, [8 bytes] file size, [24 bytes] padding)
// - String Table (null-terminated strings for virtual file paths)
// - File Data (actual file contents)

struct x1fs_entry {
    uint64_t string_table_index;
    uint8_t md5_hash[16];
    uint64_t file_address;
    uint64_t file_size;
    uint8_t padding[24];
} __attribute__((packed));

struct x1fs_header {
    uint32_t signature;
    uint32_t num_entries;
    uint64_t offset_to_string_table;
    uint64_t offset_to_file_data;
} __attribute__((packed));

/* Runtime-created file/directory, kept entirely in RAM. The static image
 * above (header/entries/string_table) is read-only -- it's built once by
 * create-ramdisk.py and reflects exactly what's baked into the ramdisk
 * image -- so anything created after boot (touch, echo >, mkdir, ...)
 * lives in this overlay list instead. Directories are still implicit
 * (derived from path prefixes, same as the static side) except that a
 * dyn entry can also directly represent an empty directory (is_dir=1)
 * so mkdir works before any file exists inside it. */
struct x1fs_dyn_entry {
    char *name;     /* stripped path, e.g. "cuak.txt" or "tmp/sub/file" */
    int is_dir;
    int is_symlink;
    int is_fifo;
    char *symlink_target;  /* heap string, NULL unless is_symlink */
    uint8_t *data;  /* NULL for directories */
    size_t size;
    size_t capacity;
    uint32_t mode;  /* permission bits only (low 12 bits); type bits come from is_dir/is_symlink */
    uint32_t uid;
    uint32_t gid;
    uint64_t ino;   /* stable, unique inode number -- see x1fs_fs.next_dyn_ino */
    struct x1fs_dyn_entry *next;
};

struct x1fs_fs {
    device_major_t major;
    device_minor_t minor;
    struct x1fs_header header;
    struct x1fs_entry *entries;
    char **string_table;
    struct x1fs_dyn_entry *dyn_entries;
    uint32_t dyn_count;
    /* Static entries are numbered 1..header.num_entries (their own string
       table index + 1); this counter hands out every dyn entry's inode
       number starting right after that range, so neither side can ever
       collide. Without real, distinct inode numbers here (previously every
       entry reported st_ino == 0), GNU cat's "don't copy a file onto
       itself" safety check (compares st_dev/st_ino between its input and
       output) false-positived for *any* two x1fs files used together as
       cat's input and output -- e.g. `cat somefifo > somefile` refused
       with "input file is output file" and wrote nothing, on every x1fs
       FIFO redirect. */
    uint64_t next_dyn_ino;
    struct x1fs_fs *next;
};

struct x1fs_fs *device_cache = NULL;
struct x1fs_fs * x1fs_register_device(device_major_t major, device_minor_t minor);

/* The vfs_fs_t X1FS registers with the VFS at boot -- saved so mount()
   (syscall_mount, via x1fs_get_ops()) can attach a fresh, deviceless tmpfs
   instance to an arbitrary path without going through vfs_new_mount()'s
   detect_fs() probe, which assumes a real backing device to read a
   signature from. */
static vfs_fs_t *g_x1fs_ops = NULL;

/* Minor 0 is always the real, boot-time ramdisk (see boot.c); each tmpfs
   mount gets the next one, so x1fs_get_fs()'s existing (major, minor)
   lookup finds it identically to any other x1fs instance. */
static device_minor_t g_next_tmpfs_minor = 1;

/* String table stores paths without a leading '/'; strip it from VFS-supplied paths. */
static inline const char * x1fs_strip_root(const char *path) {
    return (path && path[0] == '/') ? path + 1 : path;
}

static struct x1fs_dyn_entry * x1fs_dyn_find(struct x1fs_fs *fs, const char *name) {
    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

status_t x1fs_detect(device_major_t major, device_minor_t minor) {
    uint8_t buffer[4];
    int64_t read_bytes = devices_read(major, minor, 0, 4, buffer); //Read first 4 bytes (signature)

    if (read_bytes != 4) {
        return FAILURE;
    }

    uint32_t signature = *(uint32_t *)buffer;
    if (signature == 0x58314653) { // 'X1FS'
        return SUCCESS;
    }
    return FAILURE;
}

struct x1fs_fs * x1fs_get_fs(device_major_t major, device_minor_t minor) {
    struct x1fs_fs *current = device_cache;
    while (current != NULL) {
        if (current->major == major && current->minor == minor) {
            return current;
        }
        current = current->next;
    }
    
    return x1fs_register_device(major, minor);
}

ssize_t x1fs_read(device_major_t major, device_minor_t minor, const char * path, size_t skip, void * buf, size_t count) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) {
        panic("x1fs_read: Filesystem not found for device");
    }

    const char *name = x1fs_strip_root(path);

    /* Runtime-created files live entirely in RAM, not on the backing device. */
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, name);
    if (d) {
        if (d->is_dir) return -EISDIR;
        if (skip >= d->size) return 0;
        size_t n = count;
        if (skip + n > d->size) n = d->size - skip;
        memcpy(buf, d->data + skip, n);
        return (ssize_t)n;
    }

    // Find the file entry by path
    struct x1fs_entry *entry = NULL;
    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        if (strcmp(fs->string_table[i], name) == 0) {
            entry = &fs->entries[i];
            break;
        }
    }
    if (!entry) {
        kprintf("x1fs_read: File '%s' not found on device %d:%d\n", path, major, minor);
        return -1; // File not found
    }

    uint64_t file_absolute_address = fs->header.offset_to_file_data + entry->file_address;

    //kprintf("x1fs_read: Reading file '%s' on device %d:%d\n", path, major, minor);
    //kprintf("x1fs_read: File entry found at address %llu with size %llu bytes\n", file_absolute_address, entry->file_size);

    if (skip >= entry->file_size) {
        return 0; // Nothing to read
    }

    size_t bytes_to_read = count;
    if (skip + count > entry->file_size) {
        bytes_to_read = entry->file_size - skip;
    }

    int64_t read_bytes = devices_read(major, minor, file_absolute_address + skip, bytes_to_read, (uint8_t *)buf);
    if (read_bytes < 0) {
        kprintf("x1fs_read: Error reading file '%s' on device %d:%d\n", path, major, minor);
        return -1; // Read error
    }

    return read_bytes;
}

ssize_t x1fs_write(device_major_t major, device_minor_t minor, const char * path, size_t skip, const void *buf, size_t count) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return -EIO;

    const char *name = x1fs_strip_root(path);
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, name);
    if (!d || d->is_dir) {
        /* The static image (anything shipped in the ramdisk) is read-only;
         * only files created at runtime via create() are writable. */
        return -EROFS;
    }

    size_t need = skip + count;
    if (need > d->capacity) {
        size_t new_cap = d->capacity ? d->capacity * 2 : 256;
        while (new_cap < need) new_cap *= 2;
        uint8_t *grown = (uint8_t *)kmalloc(new_cap);
        if (d->size) memcpy(grown, d->data, d->size);
        if (d->data) kfree(d->data);
        d->data = grown;
        d->capacity = new_cap;
    }
    memcpy(d->data + skip, buf, count);
    if (need > d->size) d->size = need;
    return (ssize_t)count;
}

struct x1fs_fs * x1fs_register_device(device_major_t major, device_minor_t minor) {
    uint8_t buffer[512];
    int64_t read_bytes = devices_read(major, minor, 0, 24, buffer); //Read first 24 bytes (header)

    if (read_bytes != 24) {
        panic("x1fs_register_device: Failed to read filesystem header");
    }

    struct x1fs_header *header = (struct x1fs_header *)buffer;
    if (header->signature != 0x58314653) { // 'X1FS'
        panic("x1fs_register_device: Invalid X1FS signature");
    }
    if (header->num_entries == 0) {
        panic("x1fs_register_device: No entries in filesystem");
    }

    struct x1fs_fs *fs = (struct x1fs_fs *)kmalloc(sizeof(struct x1fs_fs));
    if (!fs) {
        panic("x1fs_register_device: Unable to allocate memory for filesystem structure");
    }

    fs->header = *header;
    fs->entries = (struct x1fs_entry *)kmalloc(sizeof(struct x1fs_entry) * fs->header.num_entries);
    if (!fs->entries) {
        panic("x1fs_register_device: Unable to allocate memory for filesystem entries");
    }

    read_bytes = devices_read(major, minor, 24, sizeof(struct x1fs_entry) * fs->header.num_entries, (uint8_t *)fs->entries);
    if (read_bytes != (int64_t)sizeof(struct x1fs_entry) * fs->header.num_entries) {
        panic("x1fs_register_device: Failed to read filesystem entries");
    }

    // Load string table
    size_t string_table_size = fs->header.offset_to_file_data - fs->header.offset_to_string_table;
    uint8_t *string_table_buffer = (uint8_t *)kmalloc(string_table_size);
    if (!string_table_buffer) {
        panic("x1fs_register_device: Unable to allocate memory for string table");
    }
    read_bytes = devices_read(major, minor, fs->header.offset_to_string_table, string_table_size, string_table_buffer);
    if (read_bytes != (int64_t)string_table_size) {
        panic("x1fs_register_device: Failed to read string table");
    }
    fs->string_table = (char **)kmalloc(sizeof(char *) * fs->header.num_entries);
    if (!fs->string_table) {
        panic("x1fs_register_device: Unable to allocate memory for string table pointers");
    }

    // Parse string table
    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        uint64_t index = fs->entries[i].string_table_index;
        if (index >= string_table_size) {
            panic("x1fs_register_device: Invalid string table index");
        }
        fs->string_table[i] = (char *)(string_table_buffer + index);
    }

    // Cache the filesystem structure for this device
    fs->major = major;
    fs->minor = minor;
    fs->dyn_entries = NULL;
    fs->dyn_count = 0;
    fs->next_dyn_ino = (uint64_t)fs->header.num_entries + 1; /* static entries own 1..num_entries */
    fs->next = device_cache;
    device_cache = fs;

    //kprintf("New X1FS mounted on device %d:%d\n", fs->major, fs->minor);
    //kprintf("X1FS Number of entries: %d\n", fs->header.num_entries);
    //kprintf("X1FS Offset to string table: %llu\n", fs->header.offset_to_string_table);
    //kprintf("X1FS Offset to file data: %llu\n", fs->header.offset_to_file_data);
    //kprintf("X1FS Entries:\n");
    //for (uint32_t i = 0; i < fs->header.num_entries; i++) {
    //    kprintf("  %s (Size: %llu bytes, Address: %llu)\n", fs->string_table[i], fs->entries[i].file_size, fs->entries[i].file_address);
    //}
    //kprintf("\n");
    //Dump the first 16 bytes of each file
    //for (uint32_t i = 0; i < fs->header.num_entries; i++) {
    //    kprintf("First 16 bytes of %s:\n", fs->string_table[i]);
    //    uint8_t buffer[16];
    //    ssize_t bytes_read = x1fs_read(fs->major, fs->minor, fs->string_table[i], 0, buffer, 16);
    //    if (bytes_read > 0) {
    //        for (ssize_t j = 0; j < bytes_read; j++) {
    //            kprintf("%02x ", buffer[j]);
    //        }
    //        kprintf("\n");
    //    }
    //}

    return fs;
}

static int x1fs_is_directory(struct x1fs_fs *fs, const char *path) {
    if (strcmp(path, "/") == 0) return 1;
    const char *name = x1fs_strip_root(path);
    if (*name == '\0') return 1; /* stripped "/" */
    size_t plen = strlen(name);
    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        const char *ep = fs->string_table[i];
        if (strncmp(ep, name, plen) == 0 && ep[plen] == '/') return 1;
    }
    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL; d = d->next) {
        if (d->is_dir && strcmp(d->name, name) == 0) return 1;
        size_t dlen = strlen(d->name);
        if (dlen > plen && strncmp(d->name, name, plen) == 0 && d->name[plen] == '/') return 1;
    }
    return 0;
}

status_t x1fs_fstat(device_major_t major, device_minor_t minor, const char * path, vfs_stat_t * buf) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) {
        panic("x1fs_fstat: Filesystem not found for device");
    }

    // Find the file entry by exact path
    const char *name = x1fs_strip_root(path);
    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        if (strcmp(fs->string_table[i], name) == 0) {
            memset(buf, 0, sizeof(vfs_stat_t));
            buf->st_ino = (uint64_t)i + 1; /* static entries own 1..num_entries, see x1fs_fs.next_dyn_ino */
            buf->st_size = fs->entries[i].file_size;
            buf->st_mode = 0x81A4; // Regular file rw-r--r--
            buf->st_nlink = 1;
            return SUCCESS;
        }
    }

    // Check runtime-created (dynamic) files and directories
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, name);
    if (d) {
        memset(buf, 0, sizeof(vfs_stat_t));
        buf->st_ino = d->ino;
        buf->st_uid = d->uid;
        buf->st_gid = d->gid;
        if (d->is_dir) {
            buf->st_mode = 0x4000 | (d->mode & 0x0FFF); // S_IFDIR
            buf->st_nlink = 2;
        } else if (d->is_symlink) {
            buf->st_mode = 0xA000 | (d->mode & 0x0FFF); // S_IFLNK
            buf->st_size = (long)strlen(d->symlink_target);
            buf->st_nlink = 1;
        } else if (d->is_fifo) {
            buf->st_mode = S_IFIFO | (d->mode & 0x0FFF);
            buf->st_nlink = 1;
        } else {
            buf->st_size = (long)d->size;
            buf->st_mode = 0x8000 | (d->mode & 0x0FFF); // S_IFREG
            buf->st_nlink = 1;
        }
        return SUCCESS;
    }

    // Check if the path is an implicit directory
    if (x1fs_is_directory(fs, path)) {
        memset(buf, 0, sizeof(vfs_stat_t));
        buf->st_mode = 0x41ED; // S_IFDIR | rwxr-xr-x
        buf->st_nlink = 2;
        return SUCCESS;
    }

    return FAILURE;
}

/* Shared state for emitting one dirent at a time, whether the entry comes
 * from the static string table or from the runtime dyn_entries overlay --
 * both loops in x1fs_readdir below funnel through this so a directory name
 * derived from a static file (e.g. "bin/sh") and one derived from a
 * runtime-created file (e.g. "tmp/foo.txt") are deduplicated against each
 * other, and so a bare `mkdir`-created empty directory shows up exactly
 * like one that already has children in it. */
struct x1fs_readdir_state {
    size_t virtual_idx;
    ssize_t bytes_written;
    size_t *index;
    uint8_t *out;
    size_t count;
    char (*seen)[256];
    int num_seen;
    int stop;
};

/* `child` is the remainder of an entry's path past the directory being
 * listed. `leaf_dtype` says what DT_* type the entry itself is if `child`
 * has no further '/' (DT_DIR for a dyn mkdir() entry, DT_LNK for a dyn
 * symlink entry, DT_REG for every static entry and dyn file, since those
 * are always leaves). */
static void x1fs_readdir_emit(struct x1fs_readdir_state *st, const char *child, uint8_t leaf_dtype) {
    if (st->stop || child[0] == '\0') return;

    const char *slash = strchr(child, '/');
    char name[256];
    uint8_t dtype;

    if (slash == NULL) {
        strncpy(name, child, 255);
        name[255] = '\0';
        dtype = leaf_dtype;
    } else {
        size_t dlen = (size_t)(slash - child);
        if (dlen >= 256) dlen = 255;
        strncpy(name, child, dlen);
        name[dlen] = '\0';
        dtype = DT_DIR;
    }

    if (dtype == DT_DIR) {
        for (int i = 0; i < st->num_seen; i++) {
            if (strcmp(st->seen[i], name) == 0) return; /* already emitted */
        }
        strncpy(st->seen[st->num_seen], name, 255);
        st->seen[st->num_seen][255] = '\0';
        st->num_seen++;
    }

    if (st->virtual_idx >= *st->index) {
        size_t name_len = strlen(name);
        /* reclen = fixed header (19 bytes) + name + NUL, aligned to 8 */
        size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;

        if ((size_t)st->bytes_written + reclen > st->count) {
            st->stop = 1;
            return;
        }

        memset(st->out, 0, reclen);
        *(uint64_t *)(st->out +  0) = st->virtual_idx + 1; /* d_ino */
        *(uint64_t *)(st->out +  8) = st->virtual_idx + 1; /* d_off */
        *(uint16_t *)(st->out + 16) = (uint16_t)reclen;    /* d_reclen */
        *(uint8_t  *)(st->out + 18) = dtype;               /* d_type */
        memcpy(st->out + 19, name, name_len + 1);          /* d_name */

        st->out += reclen;
        st->bytes_written += (ssize_t)reclen;
        (*st->index)++;
    }
    st->virtual_idx++;
}

ssize_t x1fs_readdir(device_major_t major, device_minor_t minor, const char *path, size_t *index, void *buf, size_t count) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return -EIO;

    /* Strip leading '/' — string table stores paths without it */
    const char *stripped = x1fs_strip_root(path);
    size_t stripped_len = strlen(stripped);

    /* Deduplicate subdirectory names: track names already emitted. Upper
     * bound: every static + dynamic entry could in principle introduce a
     * distinct subdirectory name. */
    size_t seen_capacity = fs->header.num_entries + fs->dyn_count;
    if (seen_capacity == 0) seen_capacity = 1;
    char (*seen)[256] = (char (*)[256])kmalloc(seen_capacity * 256);
    if (!seen) return -ENOMEM;

    struct x1fs_readdir_state st = {
        .virtual_idx = 0, .bytes_written = 0, .index = index,
        .out = (uint8_t *)buf, .count = count, .seen = seen,
        .num_seen = 0, .stop = 0,
    };

    for (uint32_t i = 0; i < fs->header.num_entries && !st.stop; i++) {
        const char *ep = fs->string_table[i];
        size_t prefix_len;

        if (*stripped == '\0') {
            /* root: all string table entries belong to the tree */
            prefix_len = 0;
        } else {
            if (strncmp(ep, stripped, stripped_len) != 0 || ep[stripped_len] != '/') continue;
            prefix_len = stripped_len + 1;
        }

        x1fs_readdir_emit(&st, ep + prefix_len, DT_REG);
    }

    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL && !st.stop; d = d->next) {
        const char *ep = d->name;
        size_t prefix_len;

        if (*stripped == '\0') {
            prefix_len = 0;
        } else {
            size_t elen = strlen(ep);
            if (elen <= stripped_len) continue;
            if (strncmp(ep, stripped, stripped_len) != 0 || ep[stripped_len] != '/') continue;
            prefix_len = stripped_len + 1;
        }

        x1fs_readdir_emit(&st, ep + prefix_len,
                         d->is_dir ? DT_DIR : (d->is_symlink ? DT_LNK : (d->is_fifo ? DT_FIFO : DT_REG)));
    }

    kfree(seen);
    return st.bytes_written;
}

status_t x1fs_ioctl(device_major_t major, device_minor_t minor, const char * path, uint64_t request, void * arg) {
    (void)major;
    (void)minor;
    (void)path;
    (void)request;
    (void)arg;

    // X1FS does not support any ioctls in this implementation
    return FAILURE; // Ioctl not supported
}

static struct x1fs_dyn_entry * x1fs_dyn_new(struct x1fs_fs *fs, const char *name, int is_dir, uint32_t mode) {
    struct x1fs_dyn_entry *d = (struct x1fs_dyn_entry *)kmalloc(sizeof(struct x1fs_dyn_entry));
    d->name = (char *)kmalloc(strlen(name) + 1);
    strcpy(d->name, name);
    d->is_dir = is_dir;
    d->is_symlink = 0;
    d->is_fifo = 0;
    d->symlink_target = NULL;
    d->data = NULL;
    d->size = 0;
    d->capacity = 0;
    d->mode = mode ? (mode & 0x0FFF) : (is_dir ? 0755u : 0644u);
    d->uid = 0;
    d->gid = 0;
    d->ino = fs->next_dyn_ino++;
    d->next = fs->dyn_entries;
    fs->dyn_entries = d;
    fs->dyn_count++;
    return d;
}

/* create()/mkdir() are only ever called by vfs_open()/vfs_mkdir() after
 * their own existence check (fstat) has already reported "not found", so
 * no pre-existing-entry check is needed here. */
status_t x1fs_create(device_major_t major, device_minor_t minor, const char *path, uint32_t mode) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    x1fs_dyn_new(fs, x1fs_strip_root(path), 0, mode);
    return SUCCESS;
}

status_t x1fs_mkdir(device_major_t major, device_minor_t minor, const char *path, uint32_t mode) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    x1fs_dyn_new(fs, x1fs_strip_root(path), 1, mode);
    return SUCCESS;
}

/* Unlike create()/mkdir(), vfs_mkfifo() has no prior existence check of its
 * own (mirrors x1fs_symlink below), so this does it itself against both the
 * dynamic overlay and the read-only static string table. */
status_t x1fs_mkfifo(device_major_t major, device_minor_t minor, const char *path, uint32_t mode) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    const char *name = x1fs_strip_root(path);
    if (x1fs_dyn_find(fs, name)) return ALREADY_EXISTS;
    for (uint32_t i = 0; i < fs->header.num_entries; i++)
        if (strcmp(fs->string_table[i], name) == 0) return ALREADY_EXISTS;

    struct x1fs_dyn_entry *d = x1fs_dyn_new(fs, name, 0, mode);
    d->is_fifo = 1;
    return SUCCESS;
}

/* Unlike create()/mkdir(), vfs_symlink() has no prior existence check of its
 * own, so this has to do it itself against both the dynamic overlay and the
 * read-only static string table. */
status_t x1fs_symlink(device_major_t major, device_minor_t minor,
                      const char *path, const char *target) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    const char *name = x1fs_strip_root(path);
    if (x1fs_dyn_find(fs, name)) return ALREADY_EXISTS;
    for (uint32_t i = 0; i < fs->header.num_entries; i++)
        if (strcmp(fs->string_table[i], name) == 0) return ALREADY_EXISTS;

    struct x1fs_dyn_entry *d = x1fs_dyn_new(fs, name, 0, 0777);
    d->is_symlink = 1;
    d->symlink_target = (char *)kmalloc(strlen(target) + 1);
    strcpy(d->symlink_target, target);
    return SUCCESS;
}

ssize_t x1fs_readlink(device_major_t major, device_minor_t minor,
                      const char *path, char *buf, size_t bufsz) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return -EIO;
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, x1fs_strip_root(path));
    if (!d || !d->is_symlink) return -EINVAL;
    size_t tlen = strlen(d->symlink_target);
    size_t n = tlen < bufsz ? tlen : bufsz;
    memcpy(buf, d->symlink_target, n);
    return (ssize_t)n;
}

/* Only runtime-created (dyn) entries can have their permissions changed --
 * files/dirs baked into the read-only static image have no per-entry mode
 * storage, matching write()'s existing "static image is read-only" rule. */
status_t x1fs_chmod(device_major_t major, device_minor_t minor, const char *path, uint32_t mode) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, x1fs_strip_root(path));
    if (!d) return NOT_IMPLEMENTED;
    d->mode = mode & 0x0FFF;
    return SUCCESS;
}

status_t x1fs_chown(device_major_t major, device_minor_t minor, const char *path, uint32_t uid, uint32_t gid) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    struct x1fs_dyn_entry *d = x1fs_dyn_find(fs, x1fs_strip_root(path));
    if (!d) return NOT_IMPLEMENTED;
    d->uid = uid;
    d->gid = gid;
    return SUCCESS;
}

status_t x1fs_unlink(device_major_t major, device_minor_t minor, const char *path) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    const char *name = x1fs_strip_root(path);

    struct x1fs_dyn_entry *prev = NULL;
    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL; d = d->next) {
        if (!d->is_dir && strcmp(d->name, name) == 0) {
            if (prev) prev->next = d->next; else fs->dyn_entries = d->next;
            fs->dyn_count--;
            if (d->data) kfree(d->data);
            if (d->symlink_target) kfree(d->symlink_target);
            kfree(d->name);
            kfree(d);
            return SUCCESS;
        }
        prev = d;
    }
    /* Files baked into the read-only static image can't be removed. */
    return NOT_IMPLEMENTED;
}

status_t x1fs_rmdir(device_major_t major, device_minor_t minor, const char *path) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    const char *name = x1fs_strip_root(path);
    size_t plen = strlen(name);

    /* Refuse if anything (static or dynamic) still lives inside it. */
    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        const char *ep = fs->string_table[i];
        if (strncmp(ep, name, plen) == 0 && ep[plen] == '/') return FAILURE;
    }
    struct x1fs_dyn_entry *prev = NULL, *target = NULL;
    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL; d = d->next) {
        if (d->is_dir && strcmp(d->name, name) == 0) { target = d; continue; }
        size_t dlen = strlen(d->name);
        if (dlen > plen && strncmp(d->name, name, plen) == 0 && d->name[plen] == '/') return FAILURE;
        if (!target) prev = d;
    }
    if (!target) return NOT_IMPLEMENTED;

    if (prev) prev->next = target->next; else fs->dyn_entries = target->next;
    fs->dyn_count--;
    kfree(target->name);
    kfree(target);
    return SUCCESS;
}

status_t x1fs_rename_op(device_major_t major, device_minor_t minor, const char *oldpath, const char *newpath) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return FAILURE;
    const char *old_name = x1fs_strip_root(oldpath);
    const char *new_name = x1fs_strip_root(newpath);
    size_t old_len = strlen(old_name);

    int renamed = 0;
    for (struct x1fs_dyn_entry *d = fs->dyn_entries; d != NULL; d = d->next) {
        size_t dlen = strlen(d->name);
        char *replacement = NULL;

        if (dlen == old_len && strcmp(d->name, old_name) == 0) {
            /* exact match: the entry itself */
            replacement = (char *)kmalloc(strlen(new_name) + 1);
            strcpy(replacement, new_name);
        } else if (dlen > old_len && strncmp(d->name, old_name, old_len) == 0 && d->name[old_len] == '/') {
            /* descendant of a renamed directory: keep its relative suffix */
            const char *suffix = d->name + old_len; /* includes leading '/' */
            replacement = (char *)kmalloc(strlen(new_name) + strlen(suffix) + 1);
            strcpy(replacement, new_name);
            strcat(replacement, suffix);
        }

        if (replacement) {
            kfree(d->name);
            d->name = replacement;
            renamed = 1;
        }
    }
    /* Files/dirs baked into the read-only static image can't be renamed. */
    return renamed ? SUCCESS : NOT_IMPLEMENTED;
}

vfs_fs_t * x1fs_get_ops(void) {
    return g_x1fs_ops;
}

/* Creates a fresh, empty, entirely-in-RAM x1fs instance with no backing
   ramdisk device at all -- i.e. a tmpfs. Reuses every existing x1fs op
   unchanged: with header.num_entries == 0, every "static image" code path
   (the ones that call devices_read()/index fs->string_table) simply never
   iterates, so only the dyn_entries overlay (already fully RAM-backed) is
   ever touched for one of these instances. */
status_t x1fs_create_tmpfs(device_minor_t *out_minor) {
    struct x1fs_fs *fs = (struct x1fs_fs *)kmalloc(sizeof(struct x1fs_fs));
    if (!fs) return FAILURE;
    memset(fs, 0, sizeof(struct x1fs_fs));
    fs->major = RAMDISK_DRIVER_MAJOR;
    fs->minor = g_next_tmpfs_minor++;
    fs->next_dyn_ino = 1; /* no static entries to reserve numbers for */
    fs->next = device_cache;
    device_cache = fs;
    *out_minor = fs->minor;
    return SUCCESS;
}

/* Frees a tmpfs instance created by x1fs_create_tmpfs() -- its dyn_entries
   overlay (the only thing it ever had) and the instance itself. Never
   called for minor 0 (the real, boot-time ramdisk), which syscall_umount
   refuses to touch. */
status_t x1fs_destroy_tmpfs(device_minor_t minor) {
    struct x1fs_fs **cur = &device_cache;
    while (*cur) {
        if ((*cur)->major == RAMDISK_DRIVER_MAJOR && (*cur)->minor == minor) {
            struct x1fs_fs *dead = *cur;
            *cur = dead->next;
            struct x1fs_dyn_entry *d = dead->dyn_entries;
            while (d) {
                struct x1fs_dyn_entry *next = d->next;
                kfree(d->name);
                if (d->symlink_target) kfree(d->symlink_target);
                if (d->data) kfree(d->data);
                kfree(d);
                d = next;
            }
            kfree(dead);
            return SUCCESS;
        }
        cur = &(*cur)->next;
    }
    return NOT_FOUND;
}

void x1fs_init(void) {
    vfs_fs_t *x1fs_ops = (vfs_fs_t *)kmalloc(sizeof(vfs_fs_t));
    if (!x1fs_ops) {
        panic("x1fs_init: Unable to allocate memory for X1FS operations");
    }

    memset(x1fs_ops->name, 0, 32);
    strcpy(x1fs_ops->name, "X1FS");
    x1fs_ops->read = x1fs_read;
    x1fs_ops->write = x1fs_write;
    x1fs_ops->detect = x1fs_detect;
    x1fs_ops->fstat = x1fs_fstat;
    x1fs_ops->ioctl = x1fs_ioctl;
    x1fs_ops->readdir = x1fs_readdir;
    x1fs_ops->poll = NULL; /* memory-backed I/O never blocks */
    x1fs_ops->mkdir = x1fs_mkdir;
    x1fs_ops->create = x1fs_create;
    x1fs_ops->mkfifo = x1fs_mkfifo;
    x1fs_ops->unlink = x1fs_unlink;
    x1fs_ops->rename_op = x1fs_rename_op;
    x1fs_ops->rmdir = x1fs_rmdir;
    x1fs_ops->symlink = x1fs_symlink;
    x1fs_ops->readlink = x1fs_readlink;
    x1fs_ops->chmod = x1fs_chmod;
    x1fs_ops->chown = x1fs_chown;
    x1fs_ops->next = NULL;

    status_t result = vfs_register_fs(x1fs_ops);
    if (result != SUCCESS) {
        panic("x1fs_init: Failed to register X1FS with VFS");
    }
    g_x1fs_ops = x1fs_ops;
}