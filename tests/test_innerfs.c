/*
 * test_innerfs.c - Host test for the inner-PFS reader.
 *
 * Opens a decoded inner PFS image, locates the superblock and walks the whole
 * tree, printing a summary and optionally a file list.
 *
 * Build (any C11 compiler):
 *   cc -std=c11 -o test_innerfs test_innerfs.c ../src/innerfs.c
 * Usage:
 *   test_innerfs <inner.pfs> [--list] [--limit N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/innerfs.h"

static FILE *g_file;

static int
file_read(void *ud, uint64_t off, void *buf, size_t len) {
    (void)ud;
    if(fseek(g_file, (long)off, SEEK_SET) != 0) return -1;
    if(fread(buf, 1, len, g_file) != len) return -1;
    return 0;
}

static int   g_limit = 0;
static long  g_count = 0;
static long  g_dirs = 0;
static long  g_files = 0;
static long long g_bytes = 0;

static int
on_entry(void *ud, const if_entry_t *e) {
    (void)ud;
    g_count++;
    if(e->type == IF_TYPE_DIR) g_dirs++;
    else { g_files++; g_bytes += (long long)e->size; }

    if(g_limit > 0 && g_count <= g_limit) {
        printf("%12llu  %s%s\n", (unsigned long long)e->size,
               e->type == IF_TYPE_DIR ? "D " : "  ",
               e->path);
    }
    return 0;
}

int
main(int argc, char **argv) {
    if_reader_t *r = 0;
    if_super_t  sup;
    if_entry_t  root;
    int          limit = 0;
    char         errbuf[256] = { 0 };
    int          i;

    if(argc < 2) {
        fprintf(stderr, "usage: %s <inner.pfs> [--list] [--limit N]\n", argv[0]);
        return 2;
    }
    for(i = 2; i < argc; i++) {
        if(strcmp(argv[i], "--list") == 0) limit = 1000000;
        else if(strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
    }

    g_file = fopen(argv[1], "rb");
    if(!g_file) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    if(if_open(&r, file_read, g_file, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "if_open failed\n");
        return 1;
    }

    if(if_find_super(r, &sup) != 0) {
        fprintf(stderr, "no inner-PFS superblock found\n");
        if_close(r);
        fclose(g_file);
        return 1;
    }

    printf("=== inner PFS ===\n");
    printf("  superblock          0x%llx (block %llu)\n",
           (unsigned long long)sup.superblock,
           (unsigned long long)(sup.superblock / 0x10000));
    printf("  block size          0x%llx\n", (unsigned long long)sup.block_size);
    printf("  dinode count        %llu\n", (unsigned long long)sup.dinode_count);
    printf("  dinode block count  %llu\n", (unsigned long long)sup.dinode_block_count);

    if(if_inode(r, 4, &root) != 0) {
        fprintf(stderr, "inode 4 unreadable\n");
        if_close(r);
        fclose(g_file);
        return 1;
    }
    printf("  root inode 4        type=%u size=%llu data_off=0x%llx\n",
           root.type, (unsigned long long)root.size,
           (unsigned long long)root.data_off);

    g_limit = limit;
    printf("\n=== walking tree ===\n");
    if(if_walk(r, 4, on_entry, 0, errbuf, sizeof(errbuf)) != 0 && errbuf[0]) {
        fprintf(stderr, "walk error: %s\n", errbuf);
    }

    printf("\nentries : %ld  (dirs %ld, files %ld)\n", g_count, g_dirs, g_files);
    printf("bytes   : %lld\n", g_bytes);

    if_close(r);
    fclose(g_file);
    return 0;
}
