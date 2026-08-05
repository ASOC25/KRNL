#include <krnl/fs/ext2/ext2.h>
#include <krnl/vfs/vfs.h>
#include <krnl/devices/devices.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define EXT2_SUPER_MAGIC    0xEF53
#define EXT2_SUPER_OFFSET   1024        /* superblock always at byte 1024 */
#define EXT2_ROOT_INO       2
#define EXT2_BAD_INO        1

/* i_mode file-type bits */
#define EXT2_S_IFSOCK  0xC000
#define EXT2_S_IFLNK   0xA000
#define EXT2_S_IFREG   0x8000
#define EXT2_S_IFBLK   0x6000
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFCHR   0x2000
#define EXT2_S_IFIFO   0x1000

#define S_ISREG(m)  (((m) & 0xF000) == EXT2_S_IFREG)
/* S_ISLNK/S_ISDIR come from krnl/vfs/vfs.h (included above) -- numerically
   identical to EXT2_S_IFLNK/EXT2_S_IFDIR, kept as shared definitions. */

/* dir entry file_type values */
#define EXT2_FT_UNKNOWN   0
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_FIFO      5
#define EXT2_FT_SOCK      6
#define EXT2_FT_SYMLINK   7

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    int32_t  s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_reserved[820];
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;      /* 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* name bytes follow immediately */
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* In-memory filesystem descriptor                                     */
/* ------------------------------------------------------------------ */

struct ext2_fs {
    device_major_t major;
    device_minor_t minor;
    struct ext2_superblock sb;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t num_groups;
    struct ext2_group_desc *groups;
    struct ext2_fs *next;
};

static struct ext2_fs *ext2_cache = NULL;

static struct ext2_fs *ext2_register(device_major_t major, device_minor_t minor);

/* ------------------------------------------------------------------ */
/* Low-level block I/O                                                 */
/* ------------------------------------------------------------------ */

static void blk_read(struct ext2_fs *fs, uint32_t bno, void *buf) {
    int64_t r = devices_read(fs->major, fs->minor,
                             (uint64_t)bno * fs->block_size, fs->block_size,
                             (uint8_t *)buf);
    if (r != (int64_t)fs->block_size)
        panic("ext2: blk_read: short read on block %u", bno);
}

static void blk_write(struct ext2_fs *fs, uint32_t bno, const void *buf) {
    int64_t r = devices_write(fs->major, fs->minor,
                              (uint64_t)bno * fs->block_size, fs->block_size,
                              (const uint8_t *)buf);
    if (r != (int64_t)fs->block_size)
        panic("ext2: blk_write: short write on block %u", bno);
}

static void sb_write(struct ext2_fs *fs) {
    int64_t r = devices_write(fs->major, fs->minor,
                              EXT2_SUPER_OFFSET, sizeof(struct ext2_superblock),
                              (const uint8_t *)&fs->sb);
    if (r != (int64_t)sizeof(struct ext2_superblock))
        panic("ext2: sb_write: failed");
}

static void gdt_write(struct ext2_fs *fs) {
    uint64_t gdt_off = (uint64_t)(fs->sb.s_first_data_block + 1) * fs->block_size;
    uint64_t gdt_sz  = (uint64_t)fs->num_groups * sizeof(struct ext2_group_desc);
    int64_t r = devices_write(fs->major, fs->minor, gdt_off, gdt_sz,
                              (const uint8_t *)fs->groups);
    if (r != (int64_t)gdt_sz)
        panic("ext2: gdt_write: failed");
}

/* ------------------------------------------------------------------ */
/* Inode I/O                                                           */
/* ------------------------------------------------------------------ */

static void inode_read(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out) {
    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->sb.s_inodes_per_group;
    uint64_t off   = (uint64_t)fs->groups[group].bg_inode_table * fs->block_size
                   + (uint64_t)idx * fs->inode_size;
    int64_t r = devices_read(fs->major, fs->minor, off,
                             sizeof(struct ext2_inode), (uint8_t *)out);
    if (r != (int64_t)sizeof(struct ext2_inode))
        panic("ext2: inode_read: failed for ino %u", ino);
}

static void inode_write(struct ext2_fs *fs, uint32_t ino,
                        const struct ext2_inode *inode) {
    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->sb.s_inodes_per_group;
    uint64_t off   = (uint64_t)fs->groups[group].bg_inode_table * fs->block_size
                   + (uint64_t)idx * fs->inode_size;
    int64_t r = devices_write(fs->major, fs->minor, off,
                              sizeof(struct ext2_inode), (const uint8_t *)inode);
    if (r != (int64_t)sizeof(struct ext2_inode))
        panic("ext2: inode_write: failed for ino %u", ino);
}

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t bitmap_find_free(const uint8_t *bm, uint32_t nbits) {
    for (uint32_t i = 0; i < nbits; i++) {
        if (!(bm[i / 8] & (1u << (i % 8)))) return i;
    }
    return (uint32_t)-1;
}

static void bitmap_set(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear(uint8_t *bm, uint32_t bit) {
    bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* ------------------------------------------------------------------ */
/* Block allocation                                                    */
/* ------------------------------------------------------------------ */

static uint32_t alloc_block(struct ext2_fs *fs, uint32_t preferred_group) {
    uint8_t *bm = kmalloc(fs->block_size);

    for (uint32_t pass = 0; pass < fs->num_groups; pass++) {
        uint32_t g = (preferred_group + pass) % fs->num_groups;
        if (fs->groups[g].bg_free_blocks_count == 0) continue;

        blk_read(fs, fs->groups[g].bg_block_bitmap, bm);
        uint32_t nbits = fs->sb.s_blocks_per_group;
        uint32_t idx = bitmap_find_free(bm, nbits);
        if (idx == (uint32_t)-1) continue;

        bitmap_set(bm, idx);
        blk_write(fs, fs->groups[g].bg_block_bitmap, bm);

        /* Block number = group * s_blocks_per_group + s_first_data_block + idx */
        uint32_t bno = g * fs->sb.s_blocks_per_group + fs->sb.s_first_data_block + idx;
        fs->groups[g].bg_free_blocks_count--;
        fs->sb.s_free_blocks_count--;
        gdt_write(fs);
        sb_write(fs);

        kfree(bm);
        return bno;
    }
    kfree(bm);
    return 0; /* no free block */
}

static void free_block(struct ext2_fs *fs, uint32_t bno) {
    if (!bno) return;
    uint32_t idx_in_group = (bno - fs->sb.s_first_data_block) % fs->sb.s_blocks_per_group;
    uint32_t g            = (bno - fs->sb.s_first_data_block) / fs->sb.s_blocks_per_group;
    if (g >= fs->num_groups) return;

    uint8_t *bm = kmalloc(fs->block_size);
    blk_read(fs, fs->groups[g].bg_block_bitmap, bm);
    bitmap_clear(bm, idx_in_group);
    blk_write(fs, fs->groups[g].bg_block_bitmap, bm);
    kfree(bm);

    fs->groups[g].bg_free_blocks_count++;
    fs->sb.s_free_blocks_count++;
    gdt_write(fs);
    sb_write(fs);
}

/* ------------------------------------------------------------------ */
/* Inode allocation                                                    */
/* ------------------------------------------------------------------ */

static uint32_t alloc_inode(struct ext2_fs *fs, uint32_t preferred_group,
                             int is_dir) {
    uint8_t *bm = kmalloc(fs->block_size);

    for (uint32_t pass = 0; pass < fs->num_groups; pass++) {
        uint32_t g = (preferred_group + pass) % fs->num_groups;
        if (fs->groups[g].bg_free_inodes_count == 0) continue;

        blk_read(fs, fs->groups[g].bg_inode_bitmap, bm);
        uint32_t idx = bitmap_find_free(bm, fs->sb.s_inodes_per_group);
        if (idx == (uint32_t)-1) continue;

        bitmap_set(bm, idx);
        blk_write(fs, fs->groups[g].bg_inode_bitmap, bm);

        uint32_t ino = g * fs->sb.s_inodes_per_group + idx + 1;
        fs->groups[g].bg_free_inodes_count--;
        fs->sb.s_free_inodes_count--;
        if (is_dir) fs->groups[g].bg_used_dirs_count++;
        gdt_write(fs);
        sb_write(fs);

        kfree(bm);
        return ino;
    }
    kfree(bm);
    return 0;
}

static void free_inode(struct ext2_fs *fs, uint32_t ino, int was_dir) {
    if (!ino) return;
    uint32_t g   = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t idx = (ino - 1) % fs->sb.s_inodes_per_group;
    if (g >= fs->num_groups) return;

    uint8_t *bm = kmalloc(fs->block_size);
    blk_read(fs, fs->groups[g].bg_inode_bitmap, bm);
    bitmap_clear(bm, idx);
    blk_write(fs, fs->groups[g].bg_inode_bitmap, bm);
    kfree(bm);

    fs->groups[g].bg_free_inodes_count++;
    fs->sb.s_free_inodes_count++;
    if (was_dir && fs->groups[g].bg_used_dirs_count > 0)
        fs->groups[g].bg_used_dirs_count--;
    gdt_write(fs);
    sb_write(fs);
}

/* ------------------------------------------------------------------ */
/* File block addressing                                               */
/* ------------------------------------------------------------------ */

static uint32_t file_get_block(struct ext2_fs *fs, const struct ext2_inode *inode,
                               uint32_t idx) {
    uint32_t ptrs = fs->block_size / 4;

    if (idx < 12) return inode->i_block[idx];
    idx -= 12;

    if (idx < ptrs) {
        if (!inode->i_block[12]) return 0;
        uint32_t *buf = kmalloc(fs->block_size);
        blk_read(fs, inode->i_block[12], buf);
        uint32_t res = buf[idx];
        kfree(buf);
        return res;
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        if (!inode->i_block[13]) return 0;
        uint32_t *b1 = kmalloc(fs->block_size);
        uint32_t *b2 = kmalloc(fs->block_size);
        blk_read(fs, inode->i_block[13], b1);
        uint32_t res = 0;
        if (b1[idx / ptrs]) {
            blk_read(fs, b1[idx / ptrs], b2);
            res = b2[idx % ptrs];
        }
        kfree(b1); kfree(b2);
        return res;
    }
    return 0; /* triple indirect not supported */
}

static void file_set_block(struct ext2_fs *fs, struct ext2_inode *inode,
                           uint32_t ino, uint32_t idx, uint32_t bno) {
    uint32_t ptrs  = fs->block_size / 4;
    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;

    if (idx < 12) {
        inode->i_block[idx] = bno;
        return;
    }
    idx -= 12;

    if (idx < ptrs) {
        uint32_t *buf = kmalloc(fs->block_size);
        if (!inode->i_block[12]) {
            uint32_t ib = alloc_block(fs, group);
            if (!ib) { kfree(buf); return; }
            memset(buf, 0, fs->block_size);
            blk_write(fs, ib, buf);
            inode->i_block[12] = ib;
            inode->i_blocks += fs->block_size / 512;
        } else {
            blk_read(fs, inode->i_block[12], buf);
        }
        buf[idx] = bno;
        blk_write(fs, inode->i_block[12], buf);
        kfree(buf);
        return;
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        uint32_t *b1 = kmalloc(fs->block_size);
        uint32_t *b2 = kmalloc(fs->block_size);
        if (!inode->i_block[13]) {
            uint32_t ib = alloc_block(fs, group);
            if (!ib) { kfree(b1); kfree(b2); return; }
            memset(b1, 0, fs->block_size);
            blk_write(fs, ib, b1);
            inode->i_block[13] = ib;
            inode->i_blocks += fs->block_size / 512;
        } else {
            blk_read(fs, inode->i_block[13], b1);
        }
        uint32_t i1 = idx / ptrs, i2 = idx % ptrs;
        if (!b1[i1]) {
            uint32_t ib = alloc_block(fs, group);
            if (!ib) { kfree(b1); kfree(b2); return; }
            memset(b2, 0, fs->block_size);
            blk_write(fs, ib, b2);
            b1[i1] = ib;
            blk_write(fs, inode->i_block[13], b1);
            inode->i_blocks += fs->block_size / 512;
        } else {
            blk_read(fs, b1[i1], b2);
        }
        b2[i2] = bno;
        blk_write(fs, b1[i1], b2);
        kfree(b1); kfree(b2);
        return;
    }
    panic("ext2: file_set_block: triple indirect not supported");
}

static void free_all_blocks(struct ext2_fs *fs, struct ext2_inode *inode) {
    if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0) {
        /* fast symlink: i_block[] holds the raw target string, not real
           block pointers -- must never reach free_block(). */
        inode->i_size = 0;
        return;
    }

    uint32_t ptrs = fs->block_size / 4;

    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            free_block(fs, inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }
    if (inode->i_block[12]) {
        uint32_t *buf = kmalloc(fs->block_size);
        blk_read(fs, inode->i_block[12], buf);
        for (uint32_t i = 0; i < ptrs; i++)
            if (buf[i]) free_block(fs, buf[i]);
        kfree(buf);
        free_block(fs, inode->i_block[12]);
        inode->i_block[12] = 0;
    }
    if (inode->i_block[13]) {
        uint32_t *b1 = kmalloc(fs->block_size);
        blk_read(fs, inode->i_block[13], b1);
        for (uint32_t i = 0; i < ptrs; i++) {
            if (!b1[i]) continue;
            uint32_t *b2 = kmalloc(fs->block_size);
            blk_read(fs, b1[i], b2);
            for (uint32_t j = 0; j < ptrs; j++)
                if (b2[j]) free_block(fs, b2[j]);
            kfree(b2);
            free_block(fs, b1[i]);
        }
        kfree(b1);
        free_block(fs, inode->i_block[13]);
        inode->i_block[13] = 0;
    }
    inode->i_blocks = 0;
    inode->i_size   = 0;
}

/* ------------------------------------------------------------------ */
/* Directory operations                                                */
/* ------------------------------------------------------------------ */

/* name bytes follow immediately after the 8-byte dir-entry header */
static inline char *de_name(struct ext2_dir_entry *de) {
    return (char *)((uint8_t *)de + sizeof(struct ext2_dir_entry));
}

static inline uint16_t de_actual(const struct ext2_dir_entry *de) {
    /* minimum rounded-up size occupied by an entry with name_len bytes */
    return (uint16_t)((sizeof(struct ext2_dir_entry) + de->name_len + 3) & ~3u);
}

/* Look up name in directory dir_ino; returns inode number or 0 */
static uint32_t dir_lookup(struct ext2_fs *fs, uint32_t dir_ino,
                           const char *name) {
    struct ext2_inode inode;
    inode_read(fs, dir_ino, &inode);
    if (!S_ISDIR(inode.i_mode)) return 0;

    uint32_t dir_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    uint8_t *buf = kmalloc(fs->block_size);
    uint8_t  nl  = (uint8_t)strlen(name);

    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t bno = file_get_block(fs, &inode, b);
        if (!bno) continue;
        blk_read(fs, bno, buf);

        uint32_t off = 0;
        while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == nl &&
                memcmp(de_name(de), name, nl) == 0) {
                uint32_t r = de->inode;
                kfree(buf);
                return r;
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
    return 0;
}

/* Add (name → child_ino) entry to directory dir_ino */
static status_t dir_add(struct ext2_fs *fs, uint32_t dir_ino,
                        const char *name, uint32_t child_ino, uint8_t ftype) {
    uint8_t  nl     = (uint8_t)strlen(name);
    uint16_t needed = (uint16_t)((sizeof(struct ext2_dir_entry) + nl + 3) & ~3u);

    struct ext2_inode dir_inode;
    inode_read(fs, dir_ino, &dir_inode);
    uint32_t dir_blocks = (dir_inode.i_size + fs->block_size - 1) / fs->block_size;
    uint8_t *buf = kmalloc(fs->block_size);

    /* Search existing blocks for a slot */
    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t bno = file_get_block(fs, &dir_inode, b);
        if (!bno) continue;
        blk_read(fs, bno, buf);

        uint32_t off = 0;
        while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;

            uint16_t actual = de->inode ? de_actual(de) : 0;
            uint16_t free   = de->rec_len - actual;
            if (free >= needed) {
                if (de->inode) {
                    /* split: shrink current, place new after it */
                    uint16_t old_rec = de->rec_len;
                    de->rec_len = actual;
                    struct ext2_dir_entry *ne =
                        (struct ext2_dir_entry *)(buf + off + actual);
                    ne->inode     = child_ino;
                    ne->rec_len   = old_rec - actual;
                    ne->name_len  = nl;
                    ne->file_type = ftype;
                    memcpy(de_name(ne), name, nl);
                } else {
                    /* reuse deleted slot */
                    de->inode     = child_ino;
                    de->name_len  = nl;
                    de->file_type = ftype;
                    memcpy(de_name(de), name, nl);
                }
                blk_write(fs, bno, buf);
                kfree(buf);
                return SUCCESS;
            }
            off += de->rec_len;
        }
    }

    /* No slot found – allocate a new directory block */
    uint32_t group  = (dir_ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t new_bno = alloc_block(fs, group);
    if (!new_bno) { kfree(buf); return FAILURE; }

    memset(buf, 0, fs->block_size);
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)buf;
    de->inode     = child_ino;
    de->rec_len   = (uint16_t)fs->block_size;
    de->name_len  = nl;
    de->file_type = ftype;
    memcpy(de_name(de), name, nl);
    blk_write(fs, new_bno, buf);

    file_set_block(fs, &dir_inode, dir_ino, dir_blocks, new_bno);
    dir_inode.i_size   += fs->block_size;
    dir_inode.i_blocks += fs->block_size / 512;
    inode_write(fs, dir_ino, &dir_inode);
    kfree(buf);
    return SUCCESS;
}

/* Remove the entry named name from directory dir_ino */
static status_t dir_remove(struct ext2_fs *fs, uint32_t dir_ino,
                            const char *name) {
    uint8_t nl = (uint8_t)strlen(name);
    struct ext2_inode inode;
    inode_read(fs, dir_ino, &inode);
    uint32_t dir_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    uint8_t *buf = kmalloc(fs->block_size);

    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t bno = file_get_block(fs, &inode, b);
        if (!bno) continue;
        blk_read(fs, bno, buf);

        uint32_t  off  = 0;
        struct ext2_dir_entry *prev = NULL;
        while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len == nl &&
                memcmp(de_name(de), name, nl) == 0) {
                if (prev) {
                    /* merge into previous entry */
                    prev->rec_len += de->rec_len;
                } else {
                    /* first entry – mark as deleted */
                    de->inode = 0;
                }
                blk_write(fs, bno, buf);
                kfree(buf);
                return SUCCESS;
            }
            prev = de;
            off += de->rec_len;
        }
    }
    kfree(buf);
    return NOT_FOUND;
}

/* Returns 1 if directory dir_ino is empty (only . and ..) */
static int dir_is_empty(struct ext2_fs *fs, uint32_t dir_ino) {
    struct ext2_inode inode;
    inode_read(fs, dir_ino, &inode);
    uint32_t dir_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    uint8_t *buf = kmalloc(fs->block_size);

    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t bno = file_get_block(fs, &inode, b);
        if (!bno) continue;
        blk_read(fs, bno, buf);
        uint32_t off = 0;
        while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode) {
                uint8_t nl = de->name_len;
                char *n = de_name(de);
                int is_dot = (nl == 1 && n[0] == '.');
                int is_dotdot = (nl == 2 && n[0] == '.' && n[1] == '.');
                if (!is_dot && !is_dotdot) {
                    kfree(buf);
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

static uint32_t path_lookup(struct ext2_fs *fs, const char *path) {
    if (!path || path[0] != '/') return 0;
    if (path[1] == '\0') return EXT2_ROOT_INO;

    uint32_t ino = EXT2_ROOT_INO;
    char comp[256];
    const char *p = path + 1;

    while (*p) {
        size_t len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len == 0) { p++; continue; }
        if (len >= 256) return 0;
        memcpy(comp, p, len);
        comp[len] = '\0';
        ino = dir_lookup(fs, ino, comp);
        if (!ino) return 0;
        p += len;
        if (*p == '/') p++;
    }
    return ino;
}

/* Split "/a/b/c" into parent="/a/b" name="c".
   Returns 0 on success, -1 on error. */
static int split_path(const char *path, char *parent, size_t psz,
                      char *name, size_t nsz) {
    size_t len = strlen(path);
    if (!len || path[0] != '/') return -1;

    const char *slash = path + len - 1;
    while (slash > path && *slash != '/') slash--;

    size_t plen = (size_t)(slash - path);
    if (plen == 0) plen = 1; /* parent is "/" */
    if (plen >= psz) return -1;
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    const char *n = slash + 1;
    size_t nlen = strlen(n);
    if (!nlen || nlen >= nsz) return -1;
    memcpy(name, n, nlen + 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FS cache management                                                 */
/* ------------------------------------------------------------------ */

static struct ext2_fs *ext2_get_fs(device_major_t major, device_minor_t minor) {
    struct ext2_fs *cur = ext2_cache;
    while (cur) {
        if (cur->major == major && cur->minor == minor) return cur;
        cur = cur->next;
    }
    return ext2_register(major, minor);
}

static struct ext2_fs *ext2_register(device_major_t major, device_minor_t minor) {
    struct ext2_superblock *sb_buf = kmalloc(sizeof(struct ext2_superblock));
    int64_t r = devices_read(major, minor, EXT2_SUPER_OFFSET,
                             sizeof(struct ext2_superblock), (uint8_t *)sb_buf);
    if (r != (int64_t)sizeof(struct ext2_superblock) || sb_buf->s_magic != EXT2_SUPER_MAGIC) {
        kfree(sb_buf);
        panic("ext2: register: bad superblock on device %d:%d", major, minor);
    }

    struct ext2_fs *fs = kmalloc(sizeof(struct ext2_fs));
    fs->major      = major;
    fs->minor      = minor;
    fs->sb         = *sb_buf;
    fs->block_size = 1024u << fs->sb.s_log_block_size;
    fs->inode_size = (fs->sb.s_rev_level >= 1) ? fs->sb.s_inode_size : 128;
    fs->num_groups = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1)
                     / fs->sb.s_blocks_per_group;
    kfree(sb_buf);

    uint64_t gdt_off  = (uint64_t)(fs->sb.s_first_data_block + 1) * fs->block_size;
    uint64_t gdt_size = (uint64_t)fs->num_groups * sizeof(struct ext2_group_desc);
    fs->groups = kmalloc(gdt_size);
    r = devices_read(major, minor, gdt_off, gdt_size, (uint8_t *)fs->groups);
    if (r != (int64_t)gdt_size)
        panic("ext2: register: failed to read GDT on device %d:%d", major, minor);

    fs->next   = ext2_cache;
    ext2_cache = fs;
    return fs;
}

/* ------------------------------------------------------------------ */
/* VFS interface: detect                                               */
/* ------------------------------------------------------------------ */

status_t ext2_detect(device_major_t major, device_minor_t minor) {
    uint8_t buf[2];
    int64_t r = devices_read(major, minor, EXT2_SUPER_OFFSET + 56, 2, buf);
    if (r != 2) return FAILURE;
    uint16_t magic = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return (magic == EXT2_SUPER_MAGIC) ? SUCCESS : FAILURE;
}

/* ------------------------------------------------------------------ */
/* VFS interface: read                                                 */
/* ------------------------------------------------------------------ */

ssize_t ext2_read(device_major_t major, device_minor_t minor, const char *path,
                  size_t skip, void *buf, size_t count) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return -ENOENT;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    if (S_ISDIR(inode.i_mode)) return -EISDIR;

    if (skip >= inode.i_size) return 0;
    if (skip + count > inode.i_size) count = inode.i_size - skip;

    uint8_t *blk_buf = kmalloc(fs->block_size);
    size_t   done    = 0;

    while (done < count) {
        uint32_t block_idx = (uint32_t)((skip + done) / fs->block_size);
        uint32_t block_off = (uint32_t)((skip + done) % fs->block_size);
        uint32_t to_read   = fs->block_size - block_off;
        if (to_read > count - done) to_read = (uint32_t)(count - done);

        uint32_t bno = file_get_block(fs, &inode, block_idx);
        if (!bno) {
            memset((uint8_t *)buf + done, 0, to_read);
        } else {
            blk_read(fs, bno, blk_buf);
            memcpy((uint8_t *)buf + done, blk_buf + block_off, to_read);
        }
        done += to_read;
    }
    kfree(blk_buf);
    return (ssize_t)done;
}

/* ------------------------------------------------------------------ */
/* VFS interface: write                                                */
/* ------------------------------------------------------------------ */

ssize_t ext2_write(device_major_t major, device_minor_t minor, const char *path,
                   size_t skip, const void *buf, size_t count) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return -ENOENT;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    if (S_ISDIR(inode.i_mode)) return -EISDIR;

    uint8_t *blk_buf = kmalloc(fs->block_size);
    size_t   done    = 0;
    uint32_t group   = (ino - 1) / fs->sb.s_inodes_per_group;

    while (done < count) {
        uint32_t block_idx = (uint32_t)((skip + done) / fs->block_size);
        uint32_t block_off = (uint32_t)((skip + done) % fs->block_size);
        uint32_t to_write  = fs->block_size - block_off;
        if (to_write > count - done) to_write = (uint32_t)(count - done);

        uint32_t bno = file_get_block(fs, &inode, block_idx);
        if (!bno) {
            bno = alloc_block(fs, group);
            if (!bno) { kfree(blk_buf); return done ? (ssize_t)done : -ENOSPC; }
            memset(blk_buf, 0, fs->block_size);
            file_set_block(fs, &inode, ino, block_idx, bno);
            inode.i_blocks += fs->block_size / 512;
        } else if (block_off || to_write != fs->block_size) {
            blk_read(fs, bno, blk_buf);
        }

        memcpy(blk_buf + block_off, (const uint8_t *)buf + done, to_write);
        blk_write(fs, bno, blk_buf);
        done += to_write;
    }

    if (skip + done > inode.i_size) inode.i_size = (uint32_t)(skip + done);
    inode_write(fs, ino, &inode);
    kfree(blk_buf);
    return (ssize_t)done;
}

/* ------------------------------------------------------------------ */
/* VFS interface: fstat                                                */
/* ------------------------------------------------------------------ */

status_t ext2_fstat(device_major_t major, device_minor_t minor, const char *path,
                    vfs_stat_t *out) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return FAILURE;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);

    memset(out, 0, sizeof(vfs_stat_t));
    out->st_ino     = ino;
    out->st_mode    = inode.i_mode;
    out->st_nlink   = inode.i_links_count;
    out->st_uid     = inode.i_uid;
    out->st_gid     = inode.i_gid;
    out->st_size    = inode.i_size;
    out->st_blocks  = inode.i_blocks;
    out->st_blksize = (long)fs->block_size;
    out->st_atim.tv_sec = (long long)inode.i_atime;
    out->st_mtim.tv_sec = (long long)inode.i_mtime;
    out->st_ctim.tv_sec = (long long)inode.i_ctime;
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: readdir                                              */
/* ------------------------------------------------------------------ */

ssize_t ext2_readdir(device_major_t major, device_minor_t minor, const char *path,
                     size_t *index, void *buf, size_t count) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t dir_ino = path_lookup(fs, path);
    if (!dir_ino) return -ENOENT;

    struct ext2_inode dir_inode;
    inode_read(fs, dir_ino, &dir_inode);
    if (!S_ISDIR(dir_inode.i_mode)) return -ENOTDIR;

    uint32_t dir_blocks = (dir_inode.i_size + fs->block_size - 1) / fs->block_size;
    uint8_t *blk_buf    = kmalloc(fs->block_size);
    uint8_t *out_ptr    = (uint8_t *)buf;
    ssize_t  written    = 0;
    size_t   vidx       = 0;

    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t bno = file_get_block(fs, &dir_inode, b);
        if (!bno) continue;
        blk_read(fs, bno, blk_buf);

        uint32_t off = 0;
        while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk_buf + off);
            if (de->rec_len == 0) break;
            if (!de->inode) { off += de->rec_len; continue; }

            if (vidx >= *index) {
                size_t nl     = de->name_len;
                size_t reclen = (19 + nl + 1 + 7) & ~(size_t)7;
                if ((size_t)written + reclen > count) goto done;

                memset(out_ptr, 0, reclen);
                *(uint64_t *)(out_ptr +  0) = de->inode;
                *(uint64_t *)(out_ptr +  8) = vidx + 1;
                *(uint16_t *)(out_ptr + 16) = (uint16_t)reclen;
                uint8_t dtype = DT_UNKNOWN;
                if (de->file_type == EXT2_FT_DIR)      dtype = DT_DIR;
                else if (de->file_type == EXT2_FT_REG_FILE) dtype = DT_REG;
                else if (de->file_type == EXT2_FT_CHRDEV)   dtype = DT_CHR;
                else if (de->file_type == EXT2_FT_SYMLINK)  dtype = DT_LNK;
                else if (de->file_type == EXT2_FT_FIFO)     dtype = DT_FIFO;
                *(uint8_t *)(out_ptr + 18) = dtype;
                memcpy(out_ptr + 19, de_name(de), nl);
                (out_ptr + 19)[nl] = '\0';

                out_ptr += reclen;
                written += (ssize_t)reclen;
                (*index)++;
            }
            vidx++;
            off += de->rec_len;
        }
    }
done:
    kfree(blk_buf);
    return written;
}

/* ------------------------------------------------------------------ */
/* VFS interface: ioctl                                                */
/* ------------------------------------------------------------------ */

status_t ext2_ioctl(device_major_t major, device_minor_t minor, const char *path,
                    uint64_t request, void *arg) {
    (void)major; (void)minor; (void)path; (void)request; (void)arg;
    return FAILURE;
}

/* ------------------------------------------------------------------ */
/* VFS interface: mkdir                                                */
/* ------------------------------------------------------------------ */

status_t ext2_mkdir(device_major_t major, device_minor_t minor, const char *path,
                    uint32_t mode) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    if (path_lookup(fs, path)) return ALREADY_EXISTS;

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;

    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    uint32_t group = (parent_ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t new_ino = alloc_inode(fs, group, 1);
    if (!new_ino) return FAILURE;

    uint32_t bno = alloc_block(fs, group);
    if (!bno) { free_inode(fs, new_ino, 1); return FAILURE; }

    /* Initialise the directory block with . and .. */
    uint8_t *buf = kmalloc(fs->block_size);
    memset(buf, 0, fs->block_size);

    /* . entry */
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)buf;
    dot->inode     = new_ino;
    dot->rec_len   = (uint16_t)((sizeof(struct ext2_dir_entry) + 1 + 3) & ~3u);
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    de_name(dot)[0] = '.';

    /* .. entry */
    struct ext2_dir_entry *dotdot =
        (struct ext2_dir_entry *)((uint8_t *)buf + dot->rec_len);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(fs->block_size - dot->rec_len);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    de_name(dotdot)[0] = '.';
    de_name(dotdot)[1] = '.';
    blk_write(fs, bno, buf);
    kfree(buf);

    /* Initialise inode */
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = (uint16_t)(EXT2_S_IFDIR | (mode & 0xFFF));
    inode.i_links_count = 2; /* dir itself + . */
    inode.i_size        = fs->block_size;
    inode.i_blocks      = fs->block_size / 512;
    inode.i_block[0]    = bno;
    inode_write(fs, new_ino, &inode);

    /* Add entry in parent */
    dir_add(fs, parent_ino, name, new_ino, EXT2_FT_DIR);

    /* Increment parent link count (for the new ..) */
    struct ext2_inode par_inode;
    inode_read(fs, parent_ino, &par_inode);
    par_inode.i_links_count++;
    inode_write(fs, parent_ino, &par_inode);

    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: create file                                          */
/* ------------------------------------------------------------------ */

status_t ext2_create(device_major_t major, device_minor_t minor, const char *path,
                     uint32_t mode) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    if (path_lookup(fs, path)) return ALREADY_EXISTS;

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;

    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    uint32_t group   = (parent_ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t new_ino = alloc_inode(fs, group, 0);
    if (!new_ino) return FAILURE;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = (uint16_t)(EXT2_S_IFREG | (mode & 0xFFF));
    inode.i_links_count = 1;
    inode_write(fs, new_ino, &inode);

    dir_add(fs, parent_ino, name, new_ino, EXT2_FT_REG_FILE);
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: mkfifo                                               */
/* ------------------------------------------------------------------ */

status_t ext2_mkfifo(device_major_t major, device_minor_t minor, const char *path,
                     uint32_t mode) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    if (path_lookup(fs, path)) return ALREADY_EXISTS;

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;

    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    uint32_t group   = (parent_ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t new_ino = alloc_inode(fs, group, 0);
    if (!new_ino) return FAILURE;

    /* No data blocks -- a FIFO carries no on-disk content, its bytes only
       ever live in the in-RAM pipe_t created the first time it's open()ed. */
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = (uint16_t)(EXT2_S_IFIFO | (mode & 0xFFF));
    inode.i_links_count = 1;
    inode_write(fs, new_ino, &inode);

    dir_add(fs, parent_ino, name, new_ino, EXT2_FT_FIFO);
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: chmod / chown                                        */
/* ------------------------------------------------------------------ */

status_t ext2_chmod(device_major_t major, device_minor_t minor, const char *path, uint32_t mode) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return FAILURE;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    inode.i_mode = (uint16_t)((inode.i_mode & 0xF000) | (mode & 0x0FFF));
    inode_write(fs, ino, &inode);
    return SUCCESS;
}

status_t ext2_chown(device_major_t major, device_minor_t minor, const char *path, uint32_t uid, uint32_t gid) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return FAILURE;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    inode.i_uid = (uint16_t)uid;
    inode.i_gid = (uint16_t)gid;
    inode_write(fs, ino, &inode);
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: symlink / readlink                                   */
/* ------------------------------------------------------------------ */

status_t ext2_symlink(device_major_t major, device_minor_t minor,
                      const char *path, const char *target) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    if (path_lookup(fs, path)) return ALREADY_EXISTS;

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;
    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    size_t tlen = strlen(target);
    uint32_t group = (parent_ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t new_ino = alloc_inode(fs, group, 0);
    if (!new_ino) return FAILURE;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = (uint16_t)(EXT2_S_IFLNK | 0777);
    inode.i_links_count = 1;
    inode.i_size        = (uint32_t)tlen;

    if (tlen <= sizeof(inode.i_block)) {
        /* fast symlink: target fits directly in the 60 bytes of i_block[] */
        memcpy(inode.i_block, target, tlen);
    } else {
        /* slow symlink: target lives in one real data block */
        uint32_t bno = alloc_block(fs, group);
        if (!bno) { free_inode(fs, new_ino, 0); return FAILURE; }
        uint8_t *buf = kmalloc(fs->block_size);
        memset(buf, 0, fs->block_size);
        memcpy(buf, target, tlen);
        blk_write(fs, bno, buf);
        kfree(buf);
        inode.i_block[0] = bno;
        inode.i_blocks   = fs->block_size / 512;
    }

    inode_write(fs, new_ino, &inode);
    dir_add(fs, parent_ino, name, new_ino, EXT2_FT_SYMLINK);
    return SUCCESS;
}

ssize_t ext2_readlink(device_major_t major, device_minor_t minor,
                      const char *path, char *buf, size_t bufsz) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return -ENOENT;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    if (!S_ISLNK(inode.i_mode)) return -EINVAL;

    size_t n = (size_t)inode.i_size < bufsz ? (size_t)inode.i_size : bufsz;
    if (inode.i_blocks == 0) {
        memcpy(buf, inode.i_block, n);
    } else {
        uint8_t *blkbuf = kmalloc(fs->block_size);
        blk_read(fs, inode.i_block[0], blkbuf);
        memcpy(buf, blkbuf, n);
        kfree(blkbuf);
    }
    return (ssize_t)n;
}

/* ------------------------------------------------------------------ */
/* VFS interface: unlink                                               */
/* ------------------------------------------------------------------ */

status_t ext2_unlink(device_major_t major, device_minor_t minor, const char *path) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return NOT_FOUND;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    if (S_ISDIR(inode.i_mode)) return FAILURE; /* use rmdir */

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;

    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    dir_remove(fs, parent_ino, name);

    inode.i_links_count--;
    if (inode.i_links_count == 0) {
        free_all_blocks(fs, &inode);
        inode.i_dtime = 0;
        inode_write(fs, ino, &inode);
        free_inode(fs, ino, 0);
    } else {
        inode_write(fs, ino, &inode);
    }
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: rename                                               */
/* ------------------------------------------------------------------ */

status_t ext2_rename(device_major_t major, device_minor_t minor,
                     const char *oldpath, const char *newpath) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);

    uint32_t src_ino = path_lookup(fs, oldpath);
    if (!src_ino) return NOT_FOUND;

    char old_parent[512], old_name[256];
    char new_parent[512], new_name[256];
    if (split_path(oldpath, old_parent, sizeof(old_parent), old_name, sizeof(old_name)) < 0)
        return FAILURE;
    if (split_path(newpath, new_parent, sizeof(new_parent), new_name, sizeof(new_name)) < 0)
        return FAILURE;

    uint32_t old_parent_ino = path_lookup(fs, old_parent);
    uint32_t new_parent_ino = path_lookup(fs, new_parent);
    if (!old_parent_ino || !new_parent_ino) return FAILURE;

    struct ext2_inode src_inode;
    inode_read(fs, src_ino, &src_inode);
    int is_dir = S_ISDIR(src_inode.i_mode);

    /* If destination exists: remove it first (file only; reject non-empty dir) */
    uint32_t dst_ino = path_lookup(fs, newpath);
    if (dst_ino) {
        struct ext2_inode dst;
        inode_read(fs, dst_ino, &dst);
        if (S_ISDIR(dst.i_mode)) {
            if (!dir_is_empty(fs, dst_ino)) return FAILURE;
            /* remove destination dir */
            dir_remove(fs, new_parent_ino, new_name);
            free_all_blocks(fs, &dst);
            inode_write(fs, dst_ino, &dst);
            free_inode(fs, dst_ino, 1);
            struct ext2_inode np;
            inode_read(fs, new_parent_ino, &np);
            if (np.i_links_count > 0) np.i_links_count--;
            inode_write(fs, new_parent_ino, &np);
        } else {
            dst.i_links_count--;
            if (!dst.i_links_count) {
                free_all_blocks(fs, &dst);
                free_inode(fs, dst_ino, 0);
            } else {
                inode_write(fs, dst_ino, &dst);
            }
            dir_remove(fs, new_parent_ino, new_name);
        }
    }

    /* Add new entry, remove old entry */
    uint8_t ftype = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    dir_add(fs, new_parent_ino, new_name, src_ino, ftype);
    dir_remove(fs, old_parent_ino, old_name);

    /* For directories: update the .. entry and parent link counts */
    if (is_dir && old_parent_ino != new_parent_ino) {
        /* Patch .. in the moved directory */
        struct ext2_inode moved;
        inode_read(fs, src_ino, &moved);
        uint32_t dir_blocks = (moved.i_size + fs->block_size - 1) / fs->block_size;
        uint8_t *buf = kmalloc(fs->block_size);
        for (uint32_t b = 0; b < dir_blocks; b++) {
            uint32_t bno = file_get_block(fs, &moved, b);
            if (!bno) continue;
            blk_read(fs, bno, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext2_dir_entry) <= fs->block_size) {
                struct ext2_dir_entry *de = (struct ext2_dir_entry *)(buf + off);
                if (de->rec_len == 0) break;
                if (de->inode && de->name_len == 2 &&
                    de_name(de)[0] == '.' && de_name(de)[1] == '.') {
                    de->inode = new_parent_ino;
                    blk_write(fs, bno, buf);
                    break;
                }
                off += de->rec_len;
            }
        }
        kfree(buf);

        struct ext2_inode old_par, new_par;
        inode_read(fs, old_parent_ino, &old_par);
        inode_read(fs, new_parent_ino, &new_par);
        if (old_par.i_links_count > 0) old_par.i_links_count--;
        new_par.i_links_count++;
        inode_write(fs, old_parent_ino, &old_par);
        inode_write(fs, new_parent_ino, &new_par);
    }
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* VFS interface: rmdir                                                */
/* ------------------------------------------------------------------ */

status_t ext2_rmdir(device_major_t major, device_minor_t minor, const char *path) {
    struct ext2_fs *fs = ext2_get_fs(major, minor);
    uint32_t ino = path_lookup(fs, path);
    if (!ino) return NOT_FOUND;

    struct ext2_inode inode;
    inode_read(fs, ino, &inode);
    if (!S_ISDIR(inode.i_mode)) return FAILURE;
    if (!dir_is_empty(fs, ino)) return FAILURE;

    char parent[512], name[256];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return FAILURE;

    uint32_t parent_ino = path_lookup(fs, parent);
    if (!parent_ino) return FAILURE;

    dir_remove(fs, parent_ino, name);

    free_all_blocks(fs, &inode);
    inode.i_links_count = 0;
    inode_write(fs, ino, &inode);
    free_inode(fs, ino, 1);

    /* Parent loses a link (the removed ..) */
    struct ext2_inode par;
    inode_read(fs, parent_ino, &par);
    if (par.i_links_count > 0) par.i_links_count--;
    inode_write(fs, parent_ino, &par);

    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void ext2_init(void) {
    vfs_fs_t *ops = kmalloc(sizeof(vfs_fs_t));
    memset(ops->name, 0, 32);
    strcpy(ops->name, "ext2");
    ops->read      = ext2_read;
    ops->write     = ext2_write;
    ops->detect    = ext2_detect;
    ops->fstat     = ext2_fstat;
    ops->ioctl     = ext2_ioctl;
    ops->readdir   = ext2_readdir;
    ops->mkdir     = ext2_mkdir;
    ops->create    = ext2_create;
    ops->mkfifo    = ext2_mkfifo;
    ops->unlink    = ext2_unlink;
    ops->rename_op = ext2_rename;
    ops->rmdir     = ext2_rmdir;
    ops->symlink   = ext2_symlink;
    ops->readlink  = ext2_readlink;
    ops->chmod     = ext2_chmod;
    ops->chown     = ext2_chown;
    ops->next      = NULL;

    status_t r = vfs_register_fs(ops);
    if (r != SUCCESS)
        panic("ext2_init: failed to register ext2 with VFS");
}
