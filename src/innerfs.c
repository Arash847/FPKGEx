/*
 * innerfs.c - Reader for the decoded PS5 inner PFS image. See innerfs.h.
 */
#include "innerfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PFS_BLOCK_SIZE   0x10000u
#define INODE_SIZE       0xA8u
#define PFS_MAGIC        20130315LL
#define PFS_VERSION      2LL

#define MODE_DIR         0x4000u
#define MODE_FILE        0x8000u

struct if_reader {
    if_read_fn read;
    void      *ud;
    if_super_t sup;
    int        have_super;
};

/* ------------------------------------------------------------------ */

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static int64_t  rd64s(const unsigned char *p) { return (int64_t)rd64(p); }

void if_init(if_reader_t **out) { if(out) *out = 0; }

int
if_open(if_reader_t **out, if_read_fn read, void *ud,
        const char *err_buf, size_t err_size) {
    if_reader_t *r;

    (void)err_buf;
    (void)err_size;
    if(!out || !read) return -1;

    r = (if_reader_t *)calloc(1, sizeof(*r));
    if(!r) return -1;
    r->read = read;
    r->ud = ud;
    *out = r;
    return 0;
}

void
if_close(if_reader_t *r) { free(r); }

/*
 * Scans block boundaries for a valid superblock. The result is cached: the
 * tree walk asks for one inode per directory entry, so re-scanning here would
 * turn an O(entries) walk into O(entries * blocks) reads.
 */
int
if_find_super(if_reader_t *r, if_super_t *out) {
    uint64_t off;
    unsigned char h[0x48];

    if(!r || !out) return -1;
    if(r->have_super) { *out = r->sup; return 0; }

    /* The header is always block aligned; probe forward until it turns up. */
    for(off = 0; off < ((uint64_t)1 << 34); off += PFS_BLOCK_SIZE) {
        if(r->read(r->ud, off, h, sizeof(h)) != 0) return -1;

        if(rd64s(h + 0x00) != PFS_VERSION) continue;
        if(rd64s(h + 0x08) != PFS_MAGIC) continue;
        if(rd32(h + 0x18) != PFS_BLOCK_SIZE) continue;

        {
            uint64_t count = rd64(h + 0x30);
            if(count == 0 || count > ((uint64_t)1 << 22)) continue;

            r->sup.superblock = off;
            r->sup.block_size = PFS_BLOCK_SIZE;
            r->sup.dinode_count = count;
            r->sup.dinode_block_count = rd64(h + 0x40);
            r->have_super = 1;
            *out = r->sup;
            return 0;
        }
    }
    return -1;
}

static uint64_t
inode_offset(uint64_t sb, uint64_t block_size, uint32_t ino) {
    uint64_t per = block_size / INODE_SIZE;
    uint64_t blk = ino / per;
    uint64_t idx = ino % per;
    return sb + block_size + blk * block_size + idx * INODE_SIZE;
}

int
if_inode(if_reader_t *r, uint32_t ino, if_entry_t *out) {
    if_super_t sup;
    unsigned char raw[INODE_SIZE];
    uint64_t      off;

    if(!r || !out) return -1;
    if(if_find_super(r, &sup) != 0) return -1;
    if(ino >= sup.dinode_count) return -1;

    off = inode_offset(sup.superblock, sup.block_size, ino);
    if(r->read(r->ud, off, raw, sizeof(raw)) != 0) return -1;

    memset(out, 0, sizeof(*out));
    out->ino = ino;

    {
        uint16_t mode = rd16(raw + 0x00);
        if(mode & MODE_DIR)      out->type = IF_TYPE_DIR;
        else if(mode & MODE_FILE) out->type = IF_TYPE_FILE;
        else                     out->type = 0;

        out->size     = (uint64_t)rd64s(raw + 0x08);
        out->data_off = rd64(raw + 0x60);
    }
    return 0;
}

long
if_read_data(if_reader_t *r, const if_entry_t *e, uint64_t off,
             void *buf, size_t count) {
    if(!r || !e || !buf) return -1;
    if(off > e->size || count > e->size - off) return -1;
    if(r->read(r->ud, e->data_off + off, buf, count) != 0) return -1;
    return (long)count;
}

/* ------------------------------------------------------------------ */

static size_t
join_path(char *out, size_t out_size, const char *parent, const char *name) {
    size_t n = 0;
    if(out_size == 0) return 0;
    out[0] = 0;
    if(parent && parent[0]) {
        n = strlen(parent);
        if(n >= out_size) n = out_size - 1;
        memcpy(out, parent, n);
    }
    if(n + 1 < out_size && n > 0) out[n++] = '/';
    if(name) {
        size_t m = strlen(name);
        if(m > out_size - n - 1) m = out_size - n - 1;
        memcpy(out + n, name, m);
        n += m;
    }
    out[n] = 0;
    return n;
}

/*
 * Emits every dirent of one directory. `visit` is called with the built path.
 */
static int
walk_dir(if_reader_t *r, uint32_t dir_ino, const char *prefix,
         if_walk_fn cb, void *ud, int depth, char *err_buf, size_t err_size) {
    if_entry_t dinode;
    unsigned char *buf;
    uint64_t       off = 0;
    int            rc = 0;

    if(depth > 64) {
        if(err_buf && err_size) {
            snprintf(err_buf, err_size, "directory nesting too deep at depth %d", depth);
        }
        return -1;
    }
    if(if_inode(r, dir_ino, &dinode) != 0) return -1;
    if(dinode.type != IF_TYPE_DIR) return -1;
    if(dinode.size == 0 || dinode.size > (64u << 20)) return -1;

    buf = (unsigned char *)malloc((size_t)dinode.size);
    if(!buf) return -1;
    if(r->read(r->ud, dinode.data_off, buf, (size_t)dinode.size) != 0) {
        free(buf);
        return -1;
    }

    while(off + 16 <= dinode.size) {
        uint32_t child = rd32(buf + off + 0);
        uint32_t type = rd32(buf + off + 4);
        uint32_t nlen = rd32(buf + off + 8);
        uint32_t esz = rd32(buf + off + 12);
        if_entry_t e;
        char     path[IF_PATH_MAX];
        char     name[256];

        if(esz == 0) break;                        /* end of directory */
        if(esz < 16 || (esz & 7) != 0) break;      /* malformed */
        if(nlen == 0 || nlen >= sizeof(name)) break;
        if(16u + nlen > esz || off + esz > dinode.size) break;

        memcpy(name, buf + off + 16, nlen);
        name[nlen] = 0;

        /* Skip "." and "..": they only duplicate what we already track. */
        if(type == IF_TYPE_DOT || type == IF_TYPE_DOTDOT) {
            off += esz;
            continue;
        }
        if(type != IF_TYPE_FILE && type != IF_TYPE_DIR) {
            off += esz;
            continue;
        }

        join_path(path, sizeof(path), prefix, name);

        if(if_inode(r, child, &e) != 0) { off += esz; continue; }
        e.ino = child;
        e.type = type;
        join_path(e.path, sizeof(e.path), prefix, name);

        if(cb && cb(ud, &e)) { free(buf); return 1; }

        if(type == IF_TYPE_DIR) {
            rc = walk_dir(r, child, path, cb, ud, depth + 1, err_buf, err_size);
            if(rc) { free(buf); return rc; }
        }
        off += esz;
    }

    free(buf);
    return 0;
}

int
if_walk(if_reader_t *r, uint32_t root_ino, if_walk_fn cb, void *ud,
        char *err_buf, size_t err_size) {
    return walk_dir(r, root_ino, "", cb, ud, 0, err_buf, err_size);
}
