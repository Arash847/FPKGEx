/*
 * test_fpkg.c - Host-side test harness for the package reader.
 *
 * Not part of the payload. Builds with any C11 compiler:
 *   cc -std=c11 -o test_fpkg test_fpkg.c ../src/fpkg.c ../src/inflate.c \
 *      ../src/jsonutil.c
 *
 * Usage: test_fpkg <package.pkg> <extract-to-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  define pf_mkdir(path) _mkdir(path)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define pf_mkdir(path) mkdir(path, 0755)
#endif

#include "../src/fpkg.h"

#define CHUNK (1024 * 1024)

static char out_root[1024];

static void
mkdirs(const char *path) {
    char tmp[2048];
    size_t i, n = strlen(path);
    if(n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for(i = 1; i < n; i++) {
        if(tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = 0;
            pf_mkdir(tmp);
            tmp[i] = path[i];
        }
    }
    pf_mkdir(tmp);
}

static int
sink_write(void *user, const char *dir, const char *name,
           const unsigned char *data, size_t len) {
    char path[2048];
    char sub[2048];
    FILE *f;
    (void)user;

    if(dir && dir[0]) {
        snprintf(sub, sizeof(sub), "%s/%s", out_root, dir);
        mkdirs(sub);
        snprintf(path, sizeof(path), "%s/%s/%s", out_root, dir, name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", out_root, name);
    }

    f = fopen(path, "wb");
    if(!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    if(len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int
on_progress(void *user, const char *path, uint64_t done, uint64_t total) {
    (void)user;
    printf("  %-12llu / %-12llu  %s\n",
           (unsigned long long)done, (unsigned long long)total, path);
    return 0;
}

int
main(int argc, char **argv) {
    fpkg_t   p;
    char     err[512];
    int      i;

    if(argc < 2) {
        fprintf(stderr, "usage: %s <package.pkg> [extract-dir]\n", argv[0]);
        return 2;
    }

    fpkg_init(&p);
    if(fpkg_open(&p, argv[1], err, sizeof(err)) != 0) {
        fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }

    printf("== metadata ==\n");
    printf("  titleName       : %s\n", p.title_name);
    printf("  titleId         : %s\n", p.title_id);
    printf("  contentId       : %s\n", p.content_id);
    printf("  contentVersion  : %s\n", p.version);
    printf("  masterVersion   : %s\n", p.master_version);
    printf("  category        : %s\n", p.category);
    printf("  drm             : %s\n", p.drm);
    printf("  defaultLanguage : %s\n", p.default_language);
    printf("  requiredSysSw   : %s\n", p.required_system_sw);
    printf("  fileSize        : %llu\n", (unsigned long long)p.file_size);
    printf("  cntOffset       : 0x%llx\n", (unsigned long long)p.cnt_offset);
    printf("  pfsOffset/Size  : 0x%llx / %llu\n",
           (unsigned long long)p.pfs_offset, (unsigned long long)p.pfs_size);
    printf("  siOffset/Size   : 0x%llx / %llu\n",
           (unsigned long long)p.si_offset, (unsigned long long)p.si_size);
    printf("  opaqueVolume    : %s (%llu bytes)\n",
           p.has_opaque_volume ? "yes" : "no",
           (unsigned long long)p.opaque_volume_size);

    printf("\n== files (%d) ==\n", p.entry_count);
    for(i = 0; i < p.entry_count; i++) {
        const fpkg_entry_t *e = &p.entries[i];
        printf("  %-12llu  %-10llu  %-8s %s\n",
               (unsigned long long)e->size,
               (unsigned long long)e->comp_size,
               e->source == FPKG_SRC_CNT ? "cnt" :
               e->method == FPKG_METHOD_DEFLATE ? "zip/defl" : "zip/store",
               e->path);
    }

    printf("\n== volume probe ==\n");
    {
        fpkg_probe_t pr;
        fpkg_probe_volume(&p, &pr);
        printf("  pfsOffset        : 0x%llx\n", (unsigned long long)pr.pfs_offset);
        printf("  pfsSize          : %llu\n", (unsigned long long)pr.pfs_size);
        printf("  outerSuperblock  : 0x%llx\n", (unsigned long long)pr.outer_superblock);
        printf("  outer version    : %llu (want 2)\n", (unsigned long long)pr.outer_version);
        printf("  outer magic      : %llu (want 20130315)\n", (unsigned long long)pr.outer_magic);
        printf("  outer blockSize  : 0x%x\n", pr.outer_block_size);
        printf("  outer inodes     : %llu\n", (unsigned long long)pr.outer_inode_count);
        printf("  pfs_image.dat    : %s\n", pr.have_pfs_image_dat ? "found" : "not found");
        printf("  naps_pkg_layout  : %s\n", pr.have_naps_layout_dat ? "found" : "not found");
        printf("  inner superblock : %s",
               pr.inner_super_found ? "found" : "not reached (needs assembly)");
        if(pr.inner_super_found) {
            printf(" @0x%llx, dinodes=%llu\n",
                   (unsigned long long)pr.inner_super_offset,
                   (unsigned long long)pr.inner_dinode_count);
        } else {
            printf("\n");
        }
    }

    if(argc >= 3) {
        fpkg_sink_t sink;
        snprintf(out_root, sizeof(out_root), "%s", argv[2]);
        mkdirs(out_root);
        memset(&sink, 0, sizeof(sink));
        sink.write = sink_write;
        sink.progress = on_progress;
        printf("\n== extracting to %s ==\n", out_root);
        if(fpkg_extract_all(&p, &sink, err, sizeof(err)) != 0) {
            fprintf(stderr, "extract failed: %s\n", err);
            fpkg_close(&p);
            return 1;
        }
        printf("extract complete\n");
    }

    fpkg_close(&p);
    return 0;
}
