/*
 * fpkg.c - Portable PlayStation fPKG reader.
 *
 * Container layout handled here (see fpkg.h for the overview):
 *
 *   [0]                       \x7fFIH / \x7fLIH / \x7fCNT outer header
 *   [cnt_offset]              \x7fCNT content header
 *       +0x10                 entry count          (big endian)
 *       +0x18                 entry table offset   (big endian)
 *       +0x40                 content id, 48 bytes
 *       +table_offset         entry count * 32 byte records
 *                             +0  type
 *                             +4  filename offset into the string table
 *                             +16 payload offset, relative to the CNT block
 *                             +20 payload size
 *   [tail]                   ZIP (PK\x03\x04) holding sce_suppl payloads
 */
#include "fpkg.h"
#include "inflate.h"
#include "jsonutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FPKG_DEBUG
#include <stdio.h>
#define FDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define FDBG(...) do { } while(0)
#endif

#define MAX_JSON_BYTES  (1024 * 1024)
#define MAX_STR_TABLE   (64 * 1024)
#define ZIP_EOCD_SCAN   (66 * 1024)

/* Payload of the SI ZIP is exposed under this root, matching the console. */
#define SI_ROOT "sce_suppl"

/*
 * CNT records whose type is known to materialise as a file but which carry no
 * entry in the string table. Best effort: a wrong guess only affects the file
 * name, never the payload.
 */
static const struct { uint32_t type; const char *name; } s_cnt_type_names[] = {
    { 0x040Au, "imagedigs.dat" }
};

/* ---------------------------------------------------------------- little endian */

static uint16_t rd16le(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64le(const unsigned char *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}
static uint32_t rd32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---------------------------------------------------------------- error helper */

static void
set_err(char *err, size_t err_size, const char *fmt, ...) {
    va_list ap;
    if(!err || err_size == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- file access */

/*
 * 64-bit file offsets. `long` is only 32 bits on Windows hosts, so a plain
 * fseek/ftell pair silently truncates on anything larger than 2 GB. Packages
 * regularly exceed that, so always go through these.
 */
#if defined(_WIN32)
#  include <stdio.h>
static int   pkg_seek(FILE *f, uint64_t off) { return _fseeki64(f, (__int64)off, SEEK_SET); }
static int64_t pkg_tell(FILE *f) { return _ftelli64(f); }
#else
#  include <sys/types.h>
static int   pkg_seek(FILE *f, uint64_t off) { return fseeko(f, (off_t)off, SEEK_SET); }
static int64_t pkg_tell(FILE *f) { return (int64_t)ftello(f); }
#endif

static long
read_at(const fpkg_t *p, uint64_t off, void *buf, size_t len) {
    FILE *f = (FILE *)p->handle;
    if(!f) return -1;
    if(pkg_seek(f, off) != 0) return -1;
    return (long)fread(buf, 1, len, f);
}

/* ---------------------------------------------------------------- entry list */

static fpkg_entry_t *
entries_push(fpkg_t *p, char *err, size_t err_size) {
    if(p->entry_count >= p->entry_cap) {
        int           cap = p->entry_cap ? p->entry_cap * 2 : 64;
        fpkg_entry_t *grown;
        if(cap > FPKG_MAX_FILES) cap = FPKG_MAX_FILES;
        if(cap <= p->entry_count) {
            set_err(err, err_size, "too many files in package");
            return 0;
        }
        grown = (fpkg_entry_t *)realloc(p->entries, (size_t)cap * sizeof(*grown));
        if(!grown) {
            set_err(err, err_size, "out of memory building file list");
            return 0;
        }
        p->entries = grown;
        p->entry_cap = cap;
    }
    memset(&p->entries[p->entry_count], 0, sizeof(p->entries[0]));
    return &p->entries[p->entry_count++];
}

static void
copy_bounded(char *dst, size_t dst_size, const char *src, size_t src_len) {
    size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---------------------------------------------------------------- CNT parsing */

/*
 * Copies the bare filename out of a string table, returning its length.
 */
static size_t
strtab_name(const char *tab, size_t tab_len, uint32_t off, const char **out) {
    size_t n = 0;
    if(off >= tab_len) { *out = ""; return 0; }
    *out = tab + off;
    while(off + n < tab_len && tab[off + n] != 0) n++;
    return n;
}

static int
parse_param_json(fpkg_t *p, const unsigned char *json, size_t json_len) {
    char *buf;

    if(!json || json_len == 0 || json_len > MAX_JSON_BYTES) return 0;
    buf = (char *)malloc(json_len + 1);
    if(!buf) return 0;
    memcpy(buf, json, json_len);
    buf[json_len] = 0;

    ju_get_string(buf, json_len, "contentId",    p->content_id, sizeof(p->content_id));
    ju_get_string(buf, json_len, "titleId",      p->title_id, sizeof(p->title_id));
    ju_get_string(buf, json_len, "contentVersion", p->version, sizeof(p->version));
    ju_get_string(buf, json_len, "masterVersion", p->master_version, sizeof(p->master_version));
    ju_get_string(buf, json_len, "applicationDrmType", p->drm, sizeof(p->drm));
    ju_get_string(buf, json_len, "requiredSystemSoftwareVersion",
                  p->required_system_sw, sizeof(p->required_system_sw));
    ju_get_string(buf, json_len, "defaultLanguage", p->default_language,
                  sizeof(p->default_language));

    /* titleName lives inside localizedParameters/<lang>/; take the first one. */
    if(!ju_get_string(buf, json_len, "titleName", p->title_name, sizeof(p->title_name))) {
        ju_get_string(buf, json_len, "localizedName", p->title_name, sizeof(p->title_name));
    }

    {
        char num[32];
        if(ju_get_number(buf, json_len, "applicationCategoryType", num, sizeof(num))) {
            copy_bounded(p->category, sizeof(p->category), num, strlen(num));
        }
    }

    free(buf);
    return 1;
}

static int
parse_cnt(fpkg_t *p, char *err, size_t err_size) {
    unsigned char       hdr[0x80];
    unsigned char      *table = 0;
    char               *strtab = 0;
    uint32_t            entry_count, table_offset, str_off = 0, str_size = 0;
    uint64_t            cnt_blob_bytes = 0;
    uint32_t            i;
    unsigned char      *param_json = 0;
    size_t              param_json_len = 0;
    int                 rc = -1;

    if(read_at(p, p->cnt_offset, hdr, sizeof(hdr)) != (long)sizeof(hdr)) {
        set_err(err, err_size, "cannot read CNT header");
        return -1;
    }
    if(memcmp(hdr, "\x7f" "CNT", 4) != 0) {
        set_err(err, err_size, "CNT block not found");
        return -1;
    }

    copy_bounded(p->content_id, sizeof(p->content_id), (const char *)hdr + 0x40, 48);

    entry_count = rd32be(hdr + 0x10);
    table_offset = rd32be(hdr + 0x18);
    if(entry_count == 0 || entry_count > 4096 || table_offset > 0x400000) {
        set_err(err, err_size, "CNT entry table looks invalid (count=%u off=0x%x)",
                entry_count, table_offset);
        return -1;
    }
    if(p->cnt_offset + table_offset + (uint64_t)entry_count * 32u > p->file_size) {
        set_err(err, err_size, "CNT entry table runs past end of file");
        return -1;
    }

    table = (unsigned char *)malloc((size_t)entry_count * 32u);
    if(!table) { set_err(err, err_size, "out of memory"); return -1; }
    if(read_at(p, p->cnt_offset + table_offset, table, (size_t)entry_count * 32u)
       != (long)((size_t)entry_count * 32u)) {
        set_err(err, err_size, "cannot read CNT entry table");
        goto done;
    }

    /* Locate the string table (type 0x0200). */
    for(i = 0; i < entry_count; i++) {
        const unsigned char *e = table + (size_t)i * 32u;
        if(rd32be(e) == 0x0200u) {
            str_off = rd32be(e + 16);
            str_size = rd32be(e + 20);
            break;
        }
    }
    FDBG("cnt: count=%u table=0x%x str_off=0x%x str_size=%u\n",
         entry_count, table_offset, str_off, str_size);
    if(str_size > 0 && str_size <= MAX_STR_TABLE) {
        strtab = (char *)malloc(str_size + 1);
        if(strtab) {
            long got = read_at(p, p->cnt_offset + str_off, strtab, str_size);
            FDBG("strtab read=%ld want=%u\n", got, str_size);
            if(got == (long)str_size) {
                strtab[str_size] = 0;
            } else {
                free(strtab);
                strtab = 0;
            }
        }
    }
    FDBG("strtab=%p\n", (void *)strtab);

    for(i = 0; i < entry_count; i++) {
        const unsigned char *e = table + (size_t)i * 32u;
        uint32_t            type = rd32be(e);
        uint32_t            fn_off = rd32be(e + 4);
        uint32_t            data_off = rd32be(e + 16);
        uint32_t            data_sz = rd32be(e + 20);
        const char         *name;
        size_t              name_len;
        fpkg_entry_t       *entry;

        if(data_sz == 0) continue;
        if(p->cnt_offset + data_off + data_sz > p->file_size) continue;
        if(type == 0x0200u) continue;          /* string table itself */

        cnt_blob_bytes += data_sz;

        name = "";
        name_len = strtab ? strtab_name(strtab, str_size, fn_off, &name) : 0;
        FDBG("ent[%u] type=0x%x fn_off=%u data_off=0x%x data_sz=%u name='%s' len=%u\n",
             i, type, fn_off, data_off, data_sz, name, (unsigned)name_len);
        if(name_len == 0) {
            /* No name in the string table: fall back to the type mapping. */
            size_t k;
            for(k = 0; k < sizeof(s_cnt_type_names) / sizeof(s_cnt_type_names[0]); k++) {
                if(s_cnt_type_names[k].type == type) {
                    name = s_cnt_type_names[k].name;
                    name_len = strlen(name);
                    break;
                }
            }
        }
        if(name_len == 0) continue;           /* structural record, not a file */

        /* param.json is the only file whose contents we also interpret. */
        if(strcmp(name, "param.json") == 0 && !param_json) {
            param_json = (unsigned char *)malloc(data_sz);
            if(param_json &&
               read_at(p, p->cnt_offset + data_off, param_json, data_sz) == (long)data_sz) {
                param_json_len = data_sz;
            } else {
                free(param_json);
                param_json = 0;
            }
        }

        entry = entries_push(p, err, err_size);
        if(!entry) goto done;
        entry->source = FPKG_SRC_CNT;
        entry->offset = p->cnt_offset + data_off;
        entry->comp_size = data_sz;
        entry->size = data_sz;
        entry->method = FPKG_METHOD_STORE;
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        snprintf(entry->path, sizeof(entry->path), "sce_sys/%s", name);
    }

    if(param_json) parse_param_json(p, param_json, param_json_len);

    /*
     * Anything in the PFS region that the CNT blobs do not account for is an
     * embedded application volume. Its layout is builder specific, so report
     * it rather than pretend it was extracted.
     */
    if(p->pfs_size > cnt_blob_bytes) {
        p->opaque_volume_size = p->pfs_size - cnt_blob_bytes;
        p->has_opaque_volume = p->opaque_volume_size > 64 * 1024;
    }

    rc = 0;

done:
    free(param_json);
    free(strtab);
    free(table);
    return rc;
}

/* ---------------------------------------------------------------- ZIP tail */

/* Entry sizes can be stored in a ZIP64 extra field; handle the common case. */
static void
zip64_extra(const unsigned char *extra, size_t extra_len,
            uint64_t *uncomp, uint64_t *local_off) {
    size_t i = 0;
    while(i + 4 <= extra_len) {
        uint16_t id = rd16le(extra + i);
        uint16_t sz = rd16le(extra + i + 2);
        if(i + 4 + sz > extra_len) break;
        if(id == 0x0001) {
            size_t j = i + 4;
            if(uncomp && *uncomp == 0xFFFFFFFFu && j + 8 <= i + 4 + sz) {
                *uncomp = rd64le(extra + j);
                j += 8;
            }
            if(local_off && *local_off == 0xFFFFFFFFu && j + 8 <= i + 4 + sz) {
                *local_off = rd64le(extra + j);
            }
            break;
        }
        i += 4 + sz;
    }
}

static int
parse_zip_tail(fpkg_t *p, char *err, size_t err_size) {
    unsigned char *scan = 0;
    size_t         scan_len;
    uint64_t       base;
    size_t         i;
    uint32_t       eocd = 0;
    uint32_t       cd_count, cd_size, cd_off;
    int            rc = -1;

    scan_len = ZIP_EOCD_SCAN < p->file_size ? ZIP_EOCD_SCAN : (size_t)p->file_size;
    base = p->file_size - scan_len;
    scan = (unsigned char *)malloc(scan_len);
    if(!scan) { set_err(err, err_size, "out of memory"); return -1; }
    if(read_at(p, base, scan, scan_len) != (long)scan_len) {
        set_err(err, err_size, "cannot read package tail");
        goto done;
    }
    for(i = scan_len - 22 + 1; i-- > 0;) {
        if(scan[i] == 'P' && scan[i + 1] == 'K' &&
           scan[i + 2] == 5 && scan[i + 3] == 6) {
            eocd = (uint32_t)i;
            break;
        }
    }
    if(!eocd) {
        rc = 0;                                /* no ZIP tail; not an error */
        goto done;
    }

    cd_count = rd16le(scan + eocd + 10);
    cd_size = rd32le(scan + eocd + 12);
    cd_off = rd32le(scan + eocd + 16);
    if(cd_count == 0 || cd_size == 0 || cd_off == 0) { rc = 0; goto done; }

    /*
     * cd_off is relative to the start of the ZIP archive, which is not the
     * start of the file. Resolve the archive base by locating the first
     * central directory record (PK\x01\x02) and subtracting the relative
     * offset the EOCD reports.
     */
    {
        unsigned char *tail = (unsigned char *)malloc(scan_len);
        long          got;
        size_t        k;
        uint64_t      cd_abs = 0;
        int           found = 0;

        if(!tail) { set_err(err, err_size, "out of memory"); goto done; }
        got = read_at(p, base, tail, scan_len);
        if(got != (long)scan_len) { free(tail); set_err(err, err_size, "cannot read package tail"); goto done; }

        for(k = 0; k + 4 <= scan_len; k++) {
            if(rd32le(tail + k) == 0x02014b50u) {
                cd_abs = base + k;
                found = 1;
                break;
            }
        }
        free(tail);
        if(!found || cd_abs < cd_off) { rc = 0; goto done; }

        p->si_offset = cd_abs - cd_off;
        p->si_size = p->file_size - p->si_offset;
        FDBG("zip: archive=0x%llx cd_abs=0x%llx cd_rel=0x%x count=%u size=%u\n",
             (unsigned long long)p->si_offset, (unsigned long long)cd_abs,
             cd_off, cd_count, cd_size);
    }

    {
        unsigned char *cd = (unsigned char *)malloc(cd_size);
        if(!cd) { set_err(err, err_size, "out of memory"); goto done; }
        if(read_at(p, p->si_offset + cd_off, cd, cd_size) != (long)cd_size) {
            free(cd);
            set_err(err, err_size, "cannot read ZIP central directory");
            goto done;
        }

        i = 0;
        while(i + 46 <= cd_size) {
            uint16_t      nlen, elen, clen;
            uint32_t      method;
            uint64_t      comp_size, uncomp_size, local_off;
            fpkg_entry_t *entry;

            if(rd32le(cd + i) != 0x02014b50u) break;

            method = rd16le(cd + i + 10);
            comp_size = rd32le(cd + i + 20);
            uncomp_size = rd32le(cd + i + 24);
            nlen = rd16le(cd + i + 28);
            elen = rd16le(cd + i + 30);
            clen = rd16le(cd + i + 32);
            local_off = rd32le(cd + i + 42);

            if(i + 46 + nlen > cd_size) break;
            if(nlen >= sizeof(entry->name)) break;

            if(uncomp_size == 0xFFFFFFFFu || local_off == 0xFFFFFFFFu ||
               comp_size == 0xFFFFFFFFu) {
                zip64_extra(cd + i + 46 + nlen, elen, &uncomp_size, &local_off);
            }

            /* Directory records carry a trailing slash and no payload. */
            if(nlen > 0 && cd[i + 46 + nlen - 1] == '/' && uncomp_size == 0) {
                i += 46u + nlen + elen + clen;
                continue;
            }

            if(p->si_offset + local_off >= p->file_size) {
                i += 46u + nlen + elen + clen;
                continue;
            }

            entry = entries_push(p, err, err_size);
            if(!entry) { free(cd); goto done; }
            entry->source = FPKG_SRC_ZIP;
            entry->method = method == 8 ? FPKG_METHOD_DEFLATE : FPKG_METHOD_STORE;
            entry->size = uncomp_size;
            entry->comp_size = comp_size;
            /*
             * For ZIP entries `offset` holds the absolute local file header
             * offset; the payload offset is derived from that header in
             * fpkg_read_entry.
             */
            entry->offset = p->si_offset + local_off;
            memcpy(entry->name, cd + i + 46, nlen);
            entry->name[nlen] = 0;
            snprintf(entry->path, sizeof(entry->path), "%s/%s", SI_ROOT, entry->name);

            i += 46u + nlen + elen + clen;
        }
        free(cd);
    }

    rc = 0;

done:
    free(scan);
    return rc;
}

/* ---------------------------------------------------------------- probe */

/*
 * The outer PFS superblock, the volume helper files, and the inner superblock.
 *
 * The outer superblock sits at FIH+0x20. Its uroot directory names the two
 * files the volume is built from. The inner superblock lives inside
 * pfs_image.dat, so a plain scan of the package only proves whether the image
 * was stored contiguously - it is not, for current builders - which is exactly
 * what this probe is meant to show.
 */

static void
probe_dirents(fpkg_t *p, uint64_t data_off, uint64_t size,
              fpkg_probe_t *out) {
    unsigned char *buf;
    uint64_t       off = 0;

    if(size == 0 || size > 1024 * 1024) return;
    buf = (unsigned char *)malloc((size_t)size);
    if(!buf) return;
    if(read_at(p, data_off, buf, (size_t)size) != (long)size) {
        free(buf);
        return;
    }

    while(off + 16 <= size) {
        uint32_t type = rd32le(buf + off + 4);
        uint32_t nlen = rd32le(buf + off + 8);
        uint32_t esz = rd32le(buf + off + 12);
        char     name[256];

        if(esz == 0 || esz < 16 || (esz & 7) || nlen == 0 || nlen >= sizeof(name)) break;
        if(16u + nlen > esz || off + esz > size) break;
        memcpy(name, buf + off + 16, nlen);
        name[nlen] = 0;

        if(type == 2) {
            if(strcmp(name, "pfs_image.dat") == 0) {
                out->have_pfs_image_dat = 1;
                out->pfs_image_dat_size = esz - 16;
            } else if(strcmp(name, "naps_pkg_layout.dat") == 0) {
                out->have_naps_layout_dat = 1;
                out->naps_layout_size = esz - 16;
            }
        }
        off += esz;
    }
    free(buf);
}

void
fpkg_probe_volume(const fpkg_t *p, fpkg_probe_t *out) {
    unsigned char h[0x100];
    uint64_t       blk;

    if(!p || !out) return;
    memset(out, 0, sizeof(*out));
    out->pfs_offset = p->pfs_offset;
    out->pfs_size = p->pfs_size;
    out->outer_superblock = p->pfs_size ? p->fih_offset + 0 : 0;

    /* FIH+0x20 carries the outer superblock offset (already validated by the
     * metadata layer); read and describe it. */
    if(read_at(p, 0, h, sizeof(h)) == (long)sizeof(h) &&
       memcmp(h, "\x7f" "FIH", 4) == 0) {
        out->outer_superblock = rd64le(h + 0x20);
    }

    if(out->outer_superblock && read_at(p, out->outer_superblock, h, 0x48) == 0x48) {
        out->outer_version = rd64le(h + 0x00);
        out->outer_magic = rd64le(h + 0x08);
        out->outer_block_size = rd32le(h + 0x18);
        out->outer_inode_count = rd64le(h + 0x30);
    }

    /* The volume helper files are named in the outer superblock's directory
     * blocks, which sit in the small container that follows the superblock.
     * That container runs to the end of the PFS region, so bound the scan by
     * the region rather than assuming a fixed block count. */
    if(out->outer_superblock && out->pfs_size) {
        uint64_t end = out->pfs_offset + out->pfs_size;
        uint64_t limit = 256;                 /* generous cap on container size */

        if(out->outer_superblock + limit * 0x10000 > end) {
            limit = (end - out->outer_superblock) / 0x10000;
        }
        for(blk = 0; blk < limit; blk++) {
            uint64_t base = out->outer_superblock + blk * 0x10000;
            if(base + 0x10000 > end) break;
            probe_dirents((fpkg_t *)p, base, 0x10000, out);
            if(out->have_pfs_image_dat && out->have_naps_layout_dat) break;
        }
    }

    /* Try to reach an inner superblock by scanning the volume region. */
    if(out->pfs_size) {
        uint64_t start = out->pfs_offset;
        uint64_t end = out->pfs_offset + out->pfs_size;
        uint64_t limit = end;

        /* The superblock sits near the end of the region, so probe backwards. */
        for(blk = 0; blk < 4096; blk++) {
            uint64_t at;
            if(blk < limit / 0x10000) break;
            at = end - (blk + 1) * 0x10000;
            if(at < start) break;
            if(read_at(p, at, h, 0x48) != 0x48) continue;
            if(rd64le(h + 0x00) == 2 && rd64le(h + 0x08) == 20130315LL &&
               rd32le(h + 0x18) == 0x10000) {
                out->inner_super_found = 1;
                out->inner_super_offset = at;
                out->inner_dinode_count = rd64le(h + 0x30);
                break;
            }
        }
    }
}

void
fpkg_init(fpkg_t *p) {
    memset(p, 0, sizeof(*p));
}

static int
locate_cnt(fpkg_t *p, char *err, size_t err_size) {
    unsigned char hdr[0x100];
    long         got;
    uint64_t     cand;
    size_t       off;

    got = read_at(p, 0, hdr, sizeof(hdr));
    if(got < 16) {
        set_err(err, err_size, "file is too small to be a package");
        return -1;
    }

    if(memcmp(hdr, "\x7f" "CNT", 4) == 0) {
        p->cnt_offset = 0;
        return 0;
    }

    if(memcmp(hdr, "\x7f" "LIH", 4) == 0) {
        cand = rd64le(hdr + 0x30);
        if(cand > 0 && cand + 4 <= p->file_size) {
            unsigned char magic[4];
            if(read_at(p, cand, magic, 4) == 4 && memcmp(magic, "\x7f" "CNT", 4) == 0) {
                p->cnt_offset = cand;
                return 0;
            }
        }
    }

    if(memcmp(hdr, "\x7f" "FIH", 4) == 0) {
        /* Standard slot for the content block in the FIH table. */
        cand = rd64le(hdr + 0x58);
        if(cand > 0 && cand + 4 <= p->file_size) {
            unsigned char magic[4];
            if(read_at(p, cand, magic, 4) == 4 && memcmp(magic, "\x7f" "CNT", 4) == 0) {
                p->cnt_offset = cand;
                return 0;
            }
        }
        /* Otherwise scan the table entries for an aligned block that starts
         * with the CNT magic. */
        for(off = 0x10; off + 8 <= (size_t)got; off += 8) {
            cand = rd64le(hdr + off);
            if(cand >= 0x10000 && cand + 4 <= p->file_size && (cand & 0xFFF) == 0) {
                unsigned char magic[4];
                if(read_at(p, cand, magic, 4) == 4 &&
                   memcmp(magic, "\x7f" "CNT", 4) == 0) {
                    p->cnt_offset = cand;
                    return 0;
                }
            }
        }
    }

    set_err(err, err_size, "unsupported package format (no CNT block located)");
    return -1;
}

static void
read_fih_fields(fpkg_t *p) {
    unsigned char hdr[0x100];
    if(read_at(p, 0, hdr, sizeof(hdr)) == (long)sizeof(hdr) &&
       memcmp(hdr, "\x7f" "FIH", 4) == 0) {
        p->fih_offset = 0;
        p->pfs_offset = rd64le(hdr + 0x10);
        p->pfs_size = rd64le(hdr + 0x18);
    }
}

int
fpkg_open(fpkg_t *p, const char *path, char *err, size_t err_size) {
    FILE *f;

    if(err && err_size) err[0] = 0;
    if(!p || !path) { set_err(err, err_size, "bad arguments"); return -1; }

    f = fopen(path, "rb");
    if(!f) {
        set_err(err, err_size, "cannot open package: %s", path);
        return -1;
    }
    fpkg_close(p);
    p->handle = f;

    if(fseek(f, 0, SEEK_END) != 0) {
        set_err(err, err_size, "cannot seek package");
        fpkg_close(p);
        return -1;
    }
    {
        int64_t n = pkg_tell(f);
        if(n <= 0) {
            set_err(err, err_size, "package is empty");
            fpkg_close(p);
            return -1;
        }
        p->file_size = (uint64_t)n;
    }

    read_fih_fields(p);

    if(locate_cnt(p, err, err_size) != 0) {
        fpkg_close(p);
        return -1;
    }
    if(parse_cnt(p, err, err_size) != 0) {
        fpkg_close(p);
        return -1;
    }
    /* ZIP parsing is best effort: a package without one still yields sce_sys. */
    parse_zip_tail(p, err, err_size);

    if(p->title_name[0] == 0 && p->content_id[0]) {
        /* Fall back to something meaningful when param.json was absent. */
        copy_bounded(p->title_name, sizeof(p->title_name),
                     p->content_id, strlen(p->content_id));
    }
    return 0;
}

void
fpkg_close(fpkg_t *p) {
    if(!p) return;
    if(p->handle) fclose((FILE *)p->handle);
    free(p->entries);
    memset(p, 0, sizeof(*p));
}

int
fpkg_refresh_info(fpkg_t *p, char *err, size_t err_size) {
    uint64_t    file_size;
    FILE       *saved;
    char        content_id[sizeof(p->content_id)];
    uint64_t    cnt_offset, pfs_offset, pfs_size, fih_offset;

    if(!p || !p->handle) {
        set_err(err, err_size, "package is not open");
        return -1;
    }
    if(err && err_size) err[0] = 0;

    saved = (FILE *)p->handle;
    file_size = p->file_size;
    cnt_offset = p->cnt_offset;
    pfs_offset = p->pfs_offset;
    pfs_size = p->pfs_size;
    fih_offset = p->fih_offset;
    memcpy(content_id, p->content_id, sizeof(content_id));

    free(p->entries);
    memset(p, 0, sizeof(*p));
    p->handle = saved;
    p->file_size = file_size;
    p->cnt_offset = cnt_offset;
    p->pfs_offset = pfs_offset;
    p->pfs_size = pfs_size;
    p->fih_offset = fih_offset;
    memcpy(p->content_id, content_id, sizeof(content_id));

    if(parse_cnt(p, err, err_size) != 0) return -1;
    parse_zip_tail(p, err, err_size);
    return 0;
}

const fpkg_entry_t *
fpkg_find(const fpkg_t *p, const char *path) {
    int i;
    if(!p || !path) return 0;
    for(i = 0; i < p->entry_count; i++) {
        if(strcmp(p->entries[i].path, path) == 0) return &p->entries[i];
    }
    return 0;
}

const fpkg_entry_t *
fpkg_find_name(const fpkg_t *p, const char *name) {
    int i;
    if(!p || !name) return 0;
    for(i = 0; i < p->entry_count; i++) {
        if(strcmp(p->entries[i].name, name) == 0) return &p->entries[i];
    }
    return 0;
}

long
fpkg_read_entry(const fpkg_t *p, const fpkg_entry_t *e,
                unsigned char *buf, size_t cap) {
    unsigned char local[30];

    if(!p || !e || !buf) return -1;
    if(e->size > cap) return -2;

    if(e->source == FPKG_SRC_CNT) {
        if(e->offset + e->size > p->file_size) return -1;
        return read_at(p, e->offset, buf, (size_t)e->size);
    }

    /* ZIP: the payload offset lives in the local file header. */
    if(read_at(p, e->offset, local, sizeof(local)) != (long)sizeof(local)) return -1;
    if(rd32le(local) != 0x04034b50u) return -1;
    {
        uint16_t nlen = rd16le(local + 26);
        uint16_t elen = rd16le(local + 28);
        uint64_t data_off = e->offset + 30u + nlen + elen;

        if(e->method == FPKG_METHOD_DEFLATE) {
            unsigned char *comp = (unsigned char *)malloc((size_t)e->comp_size);
            size_t         written = 0;
            int            rc;
            if(!comp) return -1;
            if(data_off + e->comp_size > p->file_size ||
               read_at(p, data_off, comp, (size_t)e->comp_size) != (long)e->comp_size) {
                free(comp);
                return -1;
            }
            rc = fpkg_inflate_raw(comp, (size_t)e->comp_size, buf, (size_t)e->size, &written);
            free(comp);
            return rc == 0 ? (long)written : -1;
        }
        if(data_off + e->size > p->file_size) return -1;
        return read_at(p, data_off, buf, (size_t)e->size);
    }
}

uint64_t
fpkg_total_uncompressed(const fpkg_t *p) {
    uint64_t total = 0;
    int      i;
    if(!p) return 0;
    for(i = 0; i < p->entry_count; i++) total += p->entries[i].size;
    return total;
}

/* Splits "a/b/c.png" into "a/b" and "c.png". Returns the name. */
static const char *
split_path(const char *path, char *dir, size_t dir_size) {
    const char *slash = strrchr(path, '/');
    if(!slash) {
        if(dir_size) dir[0] = 0;
        return path;
    }
    {
        size_t n = (size_t)(slash - path);
        if(n >= dir_size) n = dir_size - 1;
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    return slash + 1;
}

int
fpkg_extract_all(const fpkg_t *p, const fpkg_sink_t *sink,
                 char *err, size_t err_size) {
    unsigned char *buf = 0;
    size_t         buf_cap = 0;
    uint64_t       done = 0;
    int            i, rc = 0;

    if(!p || !sink || !sink->write) {
        set_err(err, err_size, "bad arguments to extract");
        return -1;
    }

    for(i = 0; i < p->entry_count; i++) {
        const fpkg_entry_t *e = &p->entries[i];
        const char         *base;
        char                dir[FPKG_PATH_MAX];
        long                got;

        if(e->size == 0) continue;

        if(e->size > buf_cap) {
            unsigned char *grown = (unsigned char *)realloc(buf, (size_t)e->size);
            if(!grown) {
                set_err(err, err_size, "out of memory extracting %s", e->path);
                rc = -1;
                goto done;
            }
            buf = grown;
            buf_cap = (size_t)e->size;
        }

        got = fpkg_read_entry(p, e, buf, buf_cap);
        if(got < 0) {
            set_err(err, err_size, "failed to read %s", e->path);
            rc = -1;
            goto done;
        }

        base = split_path(e->path, dir, sizeof(dir));
        if(sink->write(sink->user, dir, base, buf, (size_t)got) != 0) {
            set_err(err, err_size, "failed to write %s", e->path);
            rc = -1;
            goto done;
        }

        done += e->size;
        if(sink->progress) {
            if(sink->progress(sink->user, e->path, done,
                              fpkg_total_uncompressed(p))) {
                set_err(err, err_size, "extraction cancelled");
                rc = -2;
                goto done;
            }
        }
    }

done:
    free(buf);
    return rc;
}
