/*
 * fsops.c - Filesystem helpers used by the payload.
 */
#include "fsops.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

void
fs_path_join(char *out, size_t out_size, const char *a, const char *b) {
    size_t n;

    if(out_size == 0) return;
    out[0] = 0;
    if(a && a[0]) {
        n = strlen(a);
        if(n >= out_size) n = out_size - 1;
        memcpy(out, a, n);
        out[n] = 0;
    }
    if(b && b[0]) {
        n = strlen(out);
        if(n > 0 && out[n - 1] == '/') n--;
        if(n + 1 < out_size) {
            out[n++] = '/';
            {
                size_t m = strlen(b);
                if(m > out_size - n - 1) m = out_size - n - 1;
                memcpy(out + n, b, m);
                out[n + m] = 0;
            }
        }
    }
}

int
fs_relpath_is_safe(const char *rel) {
    size_t i;

    if(!rel || !rel[0]) return 0;
    if(rel[0] == '/') return 0;

    for(i = 0; rel[i]; i++) {
        unsigned char c = (unsigned char)rel[i];
        if(c < 0x20 || c == 0x7F) return 0;
        if(c == '\\') return 0;
    }

    /* Reject any ".." path component. */
    {
        const char *p = rel;
        while(*p) {
            const char *slash = strchr(p, '/');
            size_t      len = slash ? (size_t)(slash - p) : strlen(p);
            if(len == 2 && p[0] == '.' && p[1] == '.') return 0;
            if(len == 0 && p != rel) {
                /* empty component such as "a//b" is tolerated but ignored */
            }
            if(!slash) break;
            p = slash + 1;
        }
    }
    return 1;
}

int
fs_mkdir_p(const char *path) {
    char   tmp[FS_PATH_MAX];
    size_t i, n;

    if(!path || !path[0]) return -1;
    n = strlen(path);
    if(n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    /* Strip trailing separators. */
    while(n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;

    for(i = 1; i < n; i++) {
        if(tmp[i] == '/') {
            tmp[i] = 0;
            if(mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;

    {
        struct stat st;
        if(stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }
    return 0;
}

int
fs_write_file(const char *path, const void *data, size_t len) {
    FILE  *f;
    char   dir[FS_PATH_MAX];
    size_t n;

    if(!path || !path[0]) return -1;

    n = strlen(path);
    if(n >= sizeof(dir)) return -1;
    memcpy(dir, path, n + 1);
    {
        char *slash = strrchr(dir, '/');
        if(slash && slash != dir) {
            *slash = 0;
            if(fs_mkdir_p(dir) != 0) return -1;
        }
    }

    f = fopen(path, "wb");
    if(!f) return -1;
    if(len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if(fclose(f) != 0) return -1;
    return 0;
}

int64_t
fs_file_size(const char *path) {
    struct stat st;
    if(!path || stat(path, &st) != 0) return -1;
    if(!S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

int
fs_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

int
fs_space(const char *path, uint64_t *total, uint64_t *available) {
    struct statvfs vfs;
    if(!path || statvfs(path, &vfs) != 0) return -1;
    if(total)      *total = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
    if(available) *available = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    return 0;
}

static int
cmp_dirent(const void *a, const void *b) {
    const fs_dirent_t *x = (const fs_dirent_t *)a;
    const fs_dirent_t *y = (const fs_dirent_t *)b;
    if(x->is_dir != y->is_dir) return y->is_dir - x->is_dir;  /* dirs first */
    return strcmp(x->name, y->name);
}

int
fs_list_dir(const char *path, fs_dirent_t *out, int max) {
    DIR           *d;
    struct dirent *de;
    int            n = 0;
    /*
     * Stat'ing every entry is cheap on internal storage but a large external
     * drive can spend minutes in per-file metadata round trips. Size only the
     * first few hundred entries and report the rest as unknown, so listing a
     * folder stays responsive.
     */
    const int      stat_budget = 256;

    if(!path || !out || max <= 0) return -1;
    d = opendir(path);
    if(!d) return -1;

    while((de = readdir(d)) != NULL && n < max) {
        size_t len = strlen(de->d_name);
        if(len == 0 || len >= sizeof(out[n].name)) continue;
        if(strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        memcpy(out[n].name, de->d_name, len + 1);
        out[n].is_dir = (de->d_type == DT_DIR) ? 1 : 0;
        out[n].size = -1;

        if(out[n].is_dir) {
            out[n].size = 0;
        } else if(n < stat_budget) {
            char full[FS_PATH_MAX];
            fs_path_join(full, sizeof(full), path, de->d_name);
            out[n].size = fs_file_size(full);
        }
        n++;
    }
    closedir(d);
    if(n > 1) qsort(out, (size_t)n, sizeof(out[0]), cmp_dirent);
    return n;
}

int
fs_list_volumes(fs_volume_t *out, int max) {
    static const char *candidates[] = {
        "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
        "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
        "/mnt/ext0", "/mnt/ext1",
        "/mnt/sandbox", "/mnt/pfs", "/data"
    };
    int n = 0;
    size_t i;

    if(!out || max <= 0) return 0;
    for(i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && n < max; i++) {
        struct stat st;
        fs_volume_t *v = &out[n];

        if(stat(candidates[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        memset(v, 0, sizeof(*v));
        snprintf(v->path, sizeof(v->path), "%s", candidates[i]);
        if(fs_space(v->path, &v->total, &v->available) != 0) {
            v->usable = 0;
        } else {
            v->usable = 1;
        }
        n++;
    }
    return n;
}
