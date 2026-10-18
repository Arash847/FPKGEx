/*
 * innerfs.h - Reader for the decoded PS5 inner PFS image.
 *
 * Layout (verified against two independent packages):
 *
 *   superblock   version i64 == 2, magic i64 == 20130315
 *                block_size  u32 @ 0x18   (0x10000)
 *                dinode_count u64 @ 0x30
 *   dinode k     @ sb + 0x10000 + (k / inodes_per_block)*block_size
 *                     + (k % inodes_per_block)*INODE_SIZE
 *                mode      u16 @ 0x00   (0x4000 dir, 0x8000 file)
 *                size      i64 @ 0x08
 *                data_off  u64 @ 0x60   absolute byte offset in the image
 *   dirents      @ data_off, `size` bytes, each:
 *                child_inode u32 @ 0, type u32 @ 4,
 *                name_len    u32 @ 8, ent_size u32 @ 12,
 *                name[]         @ 16     (ent_size = align8(16 + name_len))
 *
 * The reader is filesystem agnostic: the caller supplies an image accessor.
 */
#ifndef FPKEX_INNERFS_H
#define FPKEX_INNERFS_H

#include <stddef.h>
#include <stdint.h>

#define IF_PATH_MAX 640

typedef enum {
    IF_TYPE_FILE = 2,
    IF_TYPE_DIR  = 3,
    IF_TYPE_DOT  = 4,   /* "."  */
    IF_TYPE_DOTDOT = 5  /* ".." */
} if_type_t;

typedef struct {
    char     path[IF_PATH_MAX];
    uint32_t ino;
    uint32_t type;
    uint64_t size;
    uint64_t data_off;   /* in image coordinates */
} if_entry_t;

typedef struct {
    uint64_t superblock;
    uint64_t block_size;
    uint64_t dinode_count;
    uint64_t dinode_block_count;
} if_super_t;

/* Opaque reader handle. */
typedef struct if_reader if_reader_t;

/* Reads `len` bytes at `off` from the image. Returns 0 on success. */
typedef int (*if_read_fn)(void *ud, uint64_t off, void *buf, size_t len);

/* Enumeration callback. Return non-zero to stop early. */
typedef int (*if_walk_fn)(void *ud, const if_entry_t *e);

void if_init(if_reader_t **out);

/*
 * Opens an inner image. `read` supplies image bytes. Returns 0 on success and
 * fills `*out` with a handle the caller must pass to if_close.
 */
int  if_open(if_reader_t **out, if_read_fn read, void *ud,
             const char *err_buf, size_t err_size);

void if_close(if_reader_t *r);

/* Locates and validates the superblock inside the image. */
int  if_find_super(if_reader_t *r, if_super_t *out);

/* Fills `out` with one inode's fields. */
int  if_inode(if_reader_t *r, uint32_t ino, if_entry_t *out);

/*
 * Walks the directory tree rooted at `root_ino`, calling `cb` for every entry.
 * Paths are built with '/' separators and are relative to the root.
 * Returns 0 on success, -1 on error.
 */
int  if_walk(if_reader_t *r, uint32_t root_ino, if_walk_fn cb, void *ud,
             char *err_buf, size_t err_size);

/*
 * Reads `count` bytes of a file's data. Returns bytes read or -1.
 */
long if_read_data(if_reader_t *r, const if_entry_t *e, uint64_t off,
                  void *buf, size_t count);

#endif /* FPKEX_INNERFS_H */
