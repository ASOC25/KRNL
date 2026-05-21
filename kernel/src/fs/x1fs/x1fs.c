#include <krnl/fs/x1fs/x1fs.h>
#include <krnl/vfs/vfs.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/devices/devices.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/allocator.h>

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

struct x1fs_fs {
    device_major_t major;
    device_minor_t minor;
    struct x1fs_header header;
    struct x1fs_entry *entries;
    char **string_table;
    struct x1fs_fs *next;
};

struct x1fs_fs *device_cache = NULL;
struct x1fs_fs * x1fs_register_device(device_major_t major, device_minor_t minor);

/* String table stores paths without a leading '/'; strip it from VFS-supplied paths. */
static inline const char * x1fs_strip_root(const char *path) {
    return (path && path[0] == '/') ? path + 1 : path;
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

    // Find the file entry by path
    const char *name = x1fs_strip_root(path);
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
    (void)major;
    (void)minor;
    (void)path;
    (void)skip;
    (void)buf;
    (void)count;

    // X1FS is read-only in this implementation
    return -1; // Write not supported
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
            buf->st_size = fs->entries[i].file_size;
            buf->st_mode = 0x81A4; // Regular file rw-r--r--
            buf->st_nlink = 1;
            return SUCCESS;
        }
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

ssize_t x1fs_readdir(device_major_t major, device_minor_t minor, const char *path, size_t *index, void *buf, size_t count) {
    struct x1fs_fs *fs = x1fs_get_fs(major, minor);
    if (!fs) return -EIO;

    /* Strip leading '/' — string table stores paths without it */
    const char *stripped = x1fs_strip_root(path);
    size_t stripped_len = strlen(stripped);
    size_t virtual_idx = 0;
    ssize_t bytes_written = 0;
    uint8_t *out = (uint8_t *)buf;

    /* Deduplicate subdirectory names: track names already emitted. */
    char (*seen)[256] = (char (*)[256])kmalloc(fs->header.num_entries * 256);
    if (!seen) return -ENOMEM;
    int num_seen = 0;

    for (uint32_t i = 0; i < fs->header.num_entries; i++) {
        const char *ep = fs->string_table[i];
        size_t prefix_len;

        if (*stripped == '\0') {
            /* root: all string table entries belong to the tree */
            prefix_len = 0;
        } else {
            if (strncmp(ep, stripped, stripped_len) != 0 || ep[stripped_len] != '/') continue;
            prefix_len = stripped_len + 1;
        }

        const char *child = ep + prefix_len;
        if (child[0] == '\0') continue;
        const char *slash = strchr(child, '/');

        char name[256];
        uint8_t dtype;

        if (slash == NULL) {
            strncpy(name, child, 255);
            name[255] = '\0';
            dtype = DT_REG;
        } else {
            size_t dlen = (size_t)(slash - child);
            if (dlen >= 256) dlen = 255;
            strncpy(name, child, dlen);
            name[dlen] = '\0';
            dtype = DT_DIR;

            /* Skip duplicate subdirectory names */
            int dup = 0;
            for (int d = 0; d < num_seen; d++) {
                if (strcmp(seen[d], name) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            strncpy(seen[num_seen], name, 255);
            seen[num_seen][255] = '\0';
            num_seen++;
        }

        /* This is unique virtual entry at virtual_idx */
        if (virtual_idx >= *index) {
            size_t name_len = strlen(name);
            /* reclen = fixed header (19 bytes) + name + NUL, aligned to 8 */
            size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;

            if ((size_t)bytes_written + reclen > count) break;

            memset(out, 0, reclen);
            *(uint64_t *)(out +  0) = virtual_idx + 1; /* d_ino */
            *(uint64_t *)(out +  8) = virtual_idx + 1; /* d_off */
            *(uint16_t *)(out + 16) = (uint16_t)reclen; /* d_reclen */
            *(uint8_t  *)(out + 18) = dtype;             /* d_type */
            memcpy(out + 19, name, name_len + 1);        /* d_name */

            out += reclen;
            bytes_written += reclen;
            (*index)++;
        }
        virtual_idx++;
    }

    kfree(seen);
    return bytes_written;
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
    x1fs_ops->next = NULL;

    status_t result = vfs_register_fs(x1fs_ops);
    if (result != SUCCESS) {
        panic("x1fs_init: Failed to register X1FS with VFS");
    }
}