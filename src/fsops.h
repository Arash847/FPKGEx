/*
 * fsops.h - Filesystem helpers used by the payload.
 *
 * Everything here is plain POSIX so the whole tree can be compile-checked
 * against a hosted libc as well as the PS5 one.
 */
#ifndef FPKEX_FSOPS_H
#define FPKEX_FSOPS_H

#include <stddef.h>
#include <stdint.h>

#define FS_PATH_MAX 1024

/* Joins `a` and `b` with a single separator. Truncates safely. */
void fs_path_join(char *out, size_t out_size, const char *a, const char *b);

/*
 * Rejects anything that could escape the destination root: absolute paths,
 * "..", and control characters. Returns 1 when `rel` is safe.
 */
int fs_relpath_is_safe(const char *rel);

/* Creates `path` and every missing parent. Returns 0 on success. */
int fs_mkdir_p(const char *path);

/* Writes `len` bytes to `path`, creating parent directories. */
int fs_write_file(const char *path, const void *data, size_t len);

/* Size of a regular file, or -1. */
int64_t fs_file_size(const char *path);

/* Non-zero when the path exists. */
int fs_exists(const char *path);

/* Total and available bytes on the filesystem holding `path`. */
int fs_space(const char *path, uint64_t *total, uint64_t *available);

/*
 * Fills `out` with up to `max` entries in `path`. Names are stored without a
 * trailing slash; `is_dir` flags directories. Returns the count or -1.
 */
typedef struct {
    char name[256];
    int  is_dir;
    int64_t size;
} fs_dirent_t;

int fs_list_dir(const char *path, fs_dirent_t *out, int max);

/*
 * Candidate extraction roots on the console, in preference order, with their
 * free space. Returns the number written to `out`.
 */
typedef struct {
    char     path[FS_PATH_MAX];
    uint64_t total;
    uint64_t available;
    int      usable;
} fs_volume_t;

int fs_list_volumes(fs_volume_t *out, int max);

#endif /* FPKEX_FSOPS_H */
