/*
 * api.c - JSON endpoints for the web UI.
 *
 * Routes:
 *   GET  /api/status            tool identity and capabilities
 *   GET  /api/volumes           candidate extraction roots with free space
 *   GET  /api/browse?path=...   directory listing, used to pick a package
 *   POST /api/open              {"path":"/mnt/usb0/game.pkg"} -> metadata + files
 *   GET  /api/art/<name>        icon0.png / pic0.png / pic1.png / pic2.png
 *   POST /api/extract           {"path":...,"dest":...}
 *   GET  /api/job               current extraction status
 *   POST /api/cancel            cancel the running extraction
 */
#include "api.h"

#include "fpkg.h"
#include "fsops.h"
#include "httpd.h"
#include "jobs.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef FPKEX_VERSION
#define FPKEX_VERSION "dev"
#endif
#ifndef FPKEX_PORT
#define FPKEX_PORT 5911
#endif

/* The opened package stays resident so artwork requests can be served. */
static fpkg_t g_pkg;
static int    g_pkg_open = 0;
static char   g_pkg_path[1024];
static time_t g_started = 0;

static const char *const ART_NAMES[] = {
    "icon0.png", "pic0.png", "pic1.png", "pic2.png", "save_data.png"
};
#define ART_COUNT ((int)(sizeof(ART_NAMES) / sizeof(ART_NAMES[0])))

static const char *
mime_for_art(const char *name) {
    return strcmp(name, "save_data.png") == 0 ? "image/png" : "image/png";
}

static int
looks_like_package(const char *path) {
    size_t n = strlen(path);
    if(n < 4) return 0;
    return strcasecmp(path + n - 4, ".pkg") == 0;
}

/* ------------------------------------------------------------------ */

void
api_detect_lan_ip(char *out, size_t out_size) {
    struct ifaddrs *ifaddr = NULL;

    if(out_size == 0) return;
    snprintf(out, out_size, "<PS5_IP>");
    if(getifaddrs(&ifaddr) != 0) return;

    for(struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if(ifa->ifa_flags & IFF_LOOPBACK) continue;
        {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            const char *ip = inet_ntop(AF_INET, &sa->sin_addr, out, out_size);
            if(ip && strncmp(out, "169.254.", 8) != 0) {
                freeifaddrs(ifaddr);
                return;
            }
        }
    }
    freeifaddrs(ifaddr);
}

static int
handle_status(const httpd_request_t *req, void *user) {
    char body[512];
    char ip[64];
    (void)user;

    api_detect_lan_ip(ip, sizeof(ip));

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"name\":\"fPKGEx\",\"version\":\"%s\","
             "\"port\":%d,\"ip\":\"%s\",\"pid\":%ld,\"uptime\":%ld,"
             "\"packageOpen\":%s}",
             FPKEX_VERSION, FPKEX_PORT, ip, (long)getpid(),
             (long)(time(NULL) - g_started),
             g_pkg_open ? "true" : "false");
    return httpd_send_text(req->fd, 200, "application/json", body) == 0 ? 200 : 500;
}

static int
handle_volumes(const httpd_request_t *req, void *user) {
    fs_volume_t vols[16];
    int         n, i;
    char       *out;
    size_t      cap = 16 * 1024;
    size_t      used;
    (void)user;

    n = fs_list_volumes(vols, 16);
    out = (char *)malloc(cap);
    if(!out) return httpd_send_error(req->fd, 500, "out of memory") == 0 ? 500 : 500;

    used = (size_t)snprintf(out, cap, "{\"ok\":true,\"volumes\":[");
    for(i = 0; i < n && used + 1024 < cap; i++) {
        char esc[1100];
        httpd_json_escape(vols[i].path, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used,
                                 "%s{\"path\":\"%s\",\"total\":%llu,"
                                 "\"available\":%llu,\"usable\":%s}",
                                 i ? "," : "", esc,
                                 (unsigned long long)vols[i].total,
                                 (unsigned long long)vols[i].available,
                                 vols[i].usable ? "true" : "false");
    }
    snprintf(out + used, cap - used, "]}");

    httpd_send_text(req->fd, 200, "application/json", out);
    free(out);
    return 200;
}

static int
handle_browse(const httpd_request_t *req, void *user) {
    char          path[1024] = "/mnt/usb0";
    fs_dirent_t   entries[512];
    int           n, i;
    char         *out;
    size_t        cap = 128 * 1024;
    size_t        used;
    (void)user;

    httpd_query_arg(req, "path", path, sizeof(path));
    if(path[0] == 0) snprintf(path, sizeof(path), "/mnt/usb0");

    n = fs_list_dir(path, entries, 512);
    if(n < 0) {
        char esc[1200];
        char msg[1400];
        httpd_json_escape(path, esc, sizeof(esc));
        snprintf(msg, sizeof(msg), "cannot read directory %s", esc);
        httpd_send_error(req->fd, 404, msg);
        return 404;
    }

    out = (char *)malloc(cap);
    if(!out) { httpd_send_error(req->fd, 500, "out of memory"); return 500; }

    used = (size_t)snprintf(out, cap, "{\"ok\":true,\"path\":\"");
    {
        char esc[1200];
        httpd_json_escape(path, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "%s\",entries:[", esc);
    }

    for(i = 0; i < n && used + 1024 < cap; i++) {
        char esc[512];
        char full[2048];
        int  is_pkg;

        httpd_json_escape(entries[i].name, esc, sizeof(esc));
        fs_path_join(full, sizeof(full), path, entries[i].name);
        is_pkg = !entries[i].is_dir && looks_like_package(entries[i].name);

        used += (size_t)snprintf(out + used, cap - used,
                                 "%s{\"name\":\"%s\",\"dir\":%s,"
                                 "\"size\":%lld,\"package\":%s}",
                                 i ? "," : "", esc,
                                 entries[i].is_dir ? "true" : "false",
                                 (long long)entries[i].size,
                                 is_pkg ? "true" : "false");
    }
    snprintf(out + used, cap - used, "]}");

    httpd_send_text(req->fd, 200, "application/json", out);
    free(out);
    return 200;
}

static int
handle_open(const httpd_request_t *req, void *user) {
    char     path[1024];
    char     err[512];
    char    *out;
    size_t   cap = 256 * 1024;
    size_t   used;
    int      i;
    (void)user;

    if(!httpd_body_string(req, "path", path, sizeof(path)) || !path[0]) {
        httpd_send_error(req->fd, 400, "missing path");
        return 400;
    }
    if(!fs_exists(path)) {
        char esc[1200];
        char msg[1400];
        httpd_json_escape(path, esc, sizeof(esc));
        snprintf(msg, sizeof(msg), "package not found: %s", esc);
        httpd_send_error(req->fd, 404, msg);
        return 404;
    }

    if(g_pkg_open) {
        fpkg_close(&g_pkg);
        g_pkg_open = 0;
    }
    fpkg_init(&g_pkg);
    if(fpkg_open(&g_pkg, path, err, sizeof(err)) != 0) {
        httpd_send_error(req->fd, 400, err);
        return 400;
    }
    g_pkg_open = 1;
    snprintf(g_pkg_path, sizeof(g_pkg_path), "%s", path);

    out = (char *)malloc(cap);
    if(!out) { httpd_send_error(req->fd, 500, "out of memory"); return 500; }

    {
        char esc[4096];
        char esc_path[1200];

        httpd_json_escape(g_pkg.title_name, esc, sizeof(esc));
        used = (size_t)snprintf(out, cap,
                                "{\"ok\":true,\"titleName\":\"%s\",", esc);

        httpd_json_escape(g_pkg.title_id, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"titleId\":\"%s\",", esc);

        httpd_json_escape(g_pkg.content_id, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"contentId\":\"%s\",", esc);

        httpd_json_escape(g_pkg.version, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"version\":\"%s\",", esc);

        httpd_json_escape(g_pkg.master_version, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"masterVersion\":\"%s\",", esc);

        httpd_json_escape(g_pkg.category, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"category\":\"%s\",", esc);

        httpd_json_escape(g_pkg.drm, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used, "\"drm\":\"%s\",", esc);

        httpd_json_escape(g_pkg.default_language, esc, sizeof(esc));
        used += (size_t)snprintf(out + used, cap - used,
                                 "\"defaultLanguage\":\"%s\",", esc);

        httpd_json_escape(path, esc_path, sizeof(esc_path));
        used += (size_t)snprintf(out + used, cap - used,
                                 "\"path\":\"%s\",", esc_path);

        used += (size_t)snprintf(out + used, cap - used,
                                 "\"fileSize\":%llu,\"fileCount\":%d,"
                                 "\"extractBytes\":%llu,"
                                 "\"hasOpaqueVolume\":%s,\"opaqueBytes\":%llu,"
                                 "\"art\":[",
                                 (unsigned long long)g_pkg.file_size,
                                 g_pkg.entry_count,
                                 (unsigned long long)fpkg_total_uncompressed(&g_pkg),
                                 g_pkg.has_opaque_volume ? "true" : "false",
                                 (unsigned long long)g_pkg.opaque_volume_size);

        for(i = 0; i < ART_COUNT && used + 512 < cap; i++) {
            const fpkg_entry_t *e = fpkg_find_name(&g_pkg, ART_NAMES[i]);
            if(!e) continue;
            used += (size_t)snprintf(out + used, cap - used,
                                     "%s{\"name\":\"%s\",\"size\":%llu}",
                                     i ? "," : "", ART_NAMES[i],
                                     (unsigned long long)e->size);
        }
        used += (size_t)snprintf(out + used, cap - used, "],\"files\":[");

        for(i = 0; i < g_pkg.entry_count && used + 1024 < cap; i++) {
            const fpkg_entry_t *e = &g_pkg.entries[i];
            char esc_name[600];
            httpd_json_escape(e->path, esc_name, sizeof(esc_name));
            used += (size_t)snprintf(out + used, cap - used,
                                     "%s{\"path\":\"%s\",\"size\":%llu,"
                                     "\"source\":\"%s\"}",
                                     i ? "," : "", esc_name,
                                     (unsigned long long)e->size,
                                     e->source == FPKG_SRC_CNT ? "sce_sys" : "sce_suppl");
        }
        snprintf(out + used, cap - used, "]}");
    }

    httpd_send_text(req->fd, 200, "application/json", out);
    free(out);
    return 200;
}

static int
handle_art(const httpd_request_t *req, void *user) {
    const char        *name;
    const fpkg_entry_t *e;
    unsigned char     *buf;
    (void)user;

    if(!g_pkg_open) {
        httpd_send_error(req->fd, 404, "no package open");
        return 404;
    }
    name = req->path + strlen("/api/art/");
    if(!name[0]) {
        httpd_send_error(req->fd, 400, "missing artwork name");
        return 400;
    }

    e = fpkg_find_name(&g_pkg, name);
    if(!e || e->size == 0 || e->size > 32u * 1024 * 1024) {
        httpd_send_error(req->fd, 404, "artwork not available");
        return 404;
    }

    buf = (unsigned char *)malloc((size_t)e->size);
    if(!buf) { httpd_send_error(req->fd, 500, "out of memory"); return 500; }

    if(fpkg_read_entry(&g_pkg, e, buf, (size_t)e->size) < 0) {
        free(buf);
        httpd_send_error(req->fd, 500, "cannot read artwork");
        return 500;
    }

    httpd_send(req->fd, 200, mime_for_art(name), buf, (size_t)e->size);
    free(buf);
    return 200;
}

static int
handle_extract(const httpd_request_t *req, void *user) {
    char pkg_path[1024];
    char dest[1024];
    char body[512];
    int  rc;
    (void)user;

    if(!httpd_body_string(req, "path", pkg_path, sizeof(pkg_path)) || !pkg_path[0]) {
        httpd_send_error(req->fd, 400, "missing path");
        return 400;
    }
    if(!httpd_body_string(req, "dest", dest, sizeof(dest)) || !dest[0]) {
        httpd_send_error(req->fd, 400, "missing dest");
        return 400;
    }
    if(!fs_exists(pkg_path)) {
        httpd_send_error(req->fd, 404, "package not found");
        return 404;
    }

    rc = jobs_start(pkg_path, dest);
    if(rc == -2) {
        httpd_send_error(req->fd, 409, "an extraction is already running");
        return 409;
    }
    if(rc != 0) {
        httpd_send_error(req->fd, 500, "cannot start extraction");
        return 500;
    }

    snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"running\"}");
    httpd_send_text(req->fd, 202, "application/json", body);
    return 202;
}

static int
handle_job(const httpd_request_t *req, void *user) {
    char body[4096];
    (void)user;
    jobs_status_json(body, sizeof(body));
    httpd_send_text(req->fd, 200, "application/json", body);
    return 200;
}

static int
handle_cancel(const httpd_request_t *req, void *user) {
    (void)user;
    jobs_cancel();
    httpd_send_text(req->fd, 200, "application/json",
                    "{\"ok\":true,\"state\":\"cancelling\"}");
    return 200;
}

/* ------------------------------------------------------------------ */

/*
 * Reports how far parsing reached for the embedded application volume, so a
 * console run tells us exactly which stage works without guesswork.
 */
static int
handle_diagnose(const httpd_request_t *req, void *user) {
    fpkg_probe_t probe;
    char        body[2048];
    char        esc_path[1200];
    (void)user;

    if(!g_pkg_open) {
        httpd_send_error(req->fd, 404, "no package open");
        return 404;
    }

    fpkg_probe_volume(&g_pkg, &probe);
    httpd_json_escape(g_pkg_path, esc_path, sizeof(esc_path));

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"path\":\"%s\","
             "\"pfsOffset\":%llu,\"pfsSize\":%llu,"
             "\"outerSuperblock\":%llu,\"outerVersion\":%llu,"
             "\"outerMagic\":%llu,\"outerBlockSize\":%u,"
             "\"outerInodeCount\":%llu,"
             "\"hasPfsImageDat\":%s,\"hasNapsLayoutDat\":%s,"
             "\"innerSuperFound\":%s,\"innerSuperOffset\":%llu,"
             "\"innerDinodeCount\":%llu}",
             esc_path,
             (unsigned long long)probe.pfs_offset,
             (unsigned long long)probe.pfs_size,
             (unsigned long long)probe.outer_superblock,
             (unsigned long long)probe.outer_version,
             (unsigned long long)probe.outer_magic,
             probe.outer_block_size,
             (unsigned long long)probe.outer_inode_count,
             probe.have_pfs_image_dat ? "true" : "false",
             probe.have_naps_layout_dat ? "true" : "false",
             probe.inner_super_found ? "true" : "false",
             (unsigned long long)probe.inner_super_offset,
             (unsigned long long)probe.inner_dinode_count);
    httpd_send_text(req->fd, 200, "application/json", body);
    return 200;
}

int
api_route(const httpd_request_t *req, void *user) {
    if(strncmp(req->path, "/api/", 5) != 0) return 404;

    if(strcmp(req->path, "/api/status") == 0)   return handle_status(req, user);
    if(strcmp(req->path, "/api/volumes") == 0)  return handle_volumes(req, user);
    if(strcmp(req->path, "/api/browse") == 0)   return handle_browse(req, user);
    if(strcmp(req->path, "/api/open") == 0)     return handle_open(req, user);
    if(strcmp(req->path, "/api/extract") == 0)  return handle_extract(req, user);
    if(strcmp(req->path, "/api/job") == 0)      return handle_job(req, user);
    if(strcmp(req->path, "/api/cancel") == 0)   return handle_cancel(req, user);
    if(strcmp(req->path, "/api/diagnose") == 0) return handle_diagnose(req, user);
    if(strncmp(req->path, "/api/art/", 9) == 0) return handle_art(req, user);

    return httpd_send_error(req->fd, 404, "unknown endpoint") == 0 ? 404 : 500;
}

void
api_init(void) {
    fpkg_init(&g_pkg);
    g_pkg_open = 0;
    g_started = time(NULL);
    jobs_init();
}
