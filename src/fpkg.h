/*
 * fpkg.h - Portable PlayStation fPKG reader.
 *
 * Parses the documented container layers of a PS4/PS5 package:
 *
 *   \x7fFIH / \x7fLIH  outer header      -> locates the CNT block
 *   \x7fCNT           content header    -> entry table, string table,
 *                                          sce_sys payloads (param.json,
 *                                          icon0.png, pic0.png, ...)
 *   ZIP (PK\x03\x04)  segment info tail  -> sce_suppl payloads
 *
 * This translation unit is deliberately free of platform dependencies so it
 * can be unit tested on a desktop host against real packages.
 */
#ifndef FPKG_H
#define FPKG_H

#include <stddef.h>
#include <stdint.h>

#define FPKG_PATH_MAX    512
#define FPKG_STR_MAX     192
#define FPKG_MAX_FILES   8192

/* Compression method for one stored file. */
enum {
    FPKG_METHOD_STORE   = 0,
    FPKG_METHOD_DEFLATE = 8
};

/* Where a payload came from, for UI reporting. */
enum {
    FPKG_SRC_CNT = 0,   /* under sce_sys - stored in the CNT block */
    FPKG_SRC_ZIP = 1    /* under sce_suppl - stored in the segment-info ZIP */
};

typedef struct {
    char     path[FPKG_PATH_MAX];   /* "sce_sys/icon0.png"                */
    char     name[FPKG_STR_MAX];    /* bare name, e.g. "icon0.png"        */
    uint64_t offset;                /* absolute offset of stored bytes    */
    uint64_t comp_size;             /* bytes on disk                      */
    uint64_t size;                  /* bytes after decompression          */
    uint32_t method;                /* FPKG_METHOD_*                      */
    uint32_t source;                /* FPKG_SRC_*                         */
} fpkg_entry_t;

typedef struct {
    char     content_id[FPKG_STR_MAX];
    char     title_id[FPKG_STR_MAX];
    char     title_name[FPKG_STR_MAX];
    char     version[FPKG_STR_MAX];
    char     master_version[FPKG_STR_MAX];
    char     category[FPKG_STR_MAX];
    char     drm[FPKG_STR_MAX];
    char     default_language[32];
    char     required_system_sw[FPKG_STR_MAX];

    uint64_t file_size;
    uint64_t fih_offset;
    uint64_t cnt_offset;
    uint64_t cnt_size;
    uint64_t pfs_offset;
    uint64_t pfs_size;
    uint64_t si_offset;
    uint64_t si_size;

    /*
     * Set when the package carries a large embedded application volume that
     * this reader does not decode. The layout is builder specific and is not
     * documented, so it is reported rather than silently skipped.
     */
    int      has_opaque_volume;
    uint64_t opaque_volume_size;

    fpkg_entry_t *entries;
    int           entry_count;
    int           entry_cap;

    void         *handle;   /* opaque; FILE* on host */
} fpkg_t;

/* Progress callback. Return non-zero to abort. */
typedef int (*fpkg_progress_fn)(void *user, const char *path,
                                uint64_t done, uint64_t total);

/*
 * Write callback. Receives the directory part and the file name separately so
 * the sink can create directories and sanitise names. Must return 0 on success.
 */
typedef int (*fpkg_write_fn)(void *user, const char *dir, const char *name,
                             const unsigned char *data, size_t len);

typedef struct {
    fpkg_write_fn    write;       /* required */
    fpkg_progress_fn progress;    /* optional */
    void            *user;
    int              cancelled;
} fpkg_sink_t;

void fpkg_init(fpkg_t *p);

/*
 * Opens `path`, reads metadata and builds the extractable file list.
 * Returns 0 on success, -1 on failure with a message in `err`.
 */
int  fpkg_open(fpkg_t *p, const char *path, char *err, size_t err_size);
void fpkg_close(fpkg_t *p);

/* Refreshes metadata only (cheap; no full file list rebuild). */
int  fpkg_refresh_info(fpkg_t *p, char *err, size_t err_size);

const fpkg_entry_t *fpkg_find(const fpkg_t *p, const char *path);
const fpkg_entry_t *fpkg_find_name(const fpkg_t *p, const char *name);

/* Reads one entry into `buf`. Returns bytes written, or -1. */
long fpkg_read_entry(const fpkg_t *p, const fpkg_entry_t *e,
                     unsigned char *buf, size_t cap);

/*
 * Extracts every discovered file through `sink`. Nothing is written by this
 * module itself, which keeps it free of filesystem dependencies.
 * Returns 0 on success.
 */
int fpkg_extract_all(const fpkg_t *p, const fpkg_sink_t *sink,
                     char *err, size_t err_size);

/* Total uncompressed bytes across all entries. */
uint64_t fpkg_total_uncompressed(const fpkg_t *p);

/*
 * Diagnostic probe for the embedded application volume.
 *
 * Reports what the reader can see beyond the metadata layer: the outer PFS
 * superblock, whether the volume's helper files are present, and whether an
 * inner PFS superblock is reachable. Used by the web UI so a console run
 * reports exactly how far parsing got.
 */
typedef struct {
    uint64_t pfs_offset;
    uint64_t pfs_size;
    uint64_t outer_superblock;      /* 0 when not found */
    uint64_t outer_version;
    uint64_t outer_magic;
    uint32_t outer_block_size;
    uint64_t outer_inode_count;

    int      have_pfs_image_dat;
    int      have_naps_layout_dat;
    uint64_t pfs_image_dat_off;
    uint64_t pfs_image_dat_size;
    uint64_t naps_layout_off;
    uint64_t naps_layout_size;

    int      inner_super_found;
    uint64_t inner_super_offset;
    uint64_t inner_dinode_count;
} fpkg_probe_t;

void fpkg_probe_volume(const fpkg_t *p, fpkg_probe_t *out);

#endif /* FPKG_H */
