/*
 * jobs.c - Extraction job runner.
 *
 * A single extraction runs on its own thread; the API only reads status under
 * a mutex. All container parsing lives in fpkg.c, which knows nothing about
 * the destination filesystem.
 */
#include "jobs.h"

#include "fpkg.h"
#include "fsops.h"
#include "httpd.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static job_t            g_job;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t        g_thread;
static volatile int     g_cancel = 0;

static const char *
state_name(job_state_t s) {
    switch(s) {
        case JOB_IDLE:      return "idle";
        case JOB_RUNNING:   return "running";
        case JOB_DONE:      return "done";
        case JOB_FAILED:    return "failed";
        case JOB_CANCELLED: return "cancelled";
        default:            return "idle";
    }
}

void
jobs_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(&g_job, 0, sizeof(g_job));
    g_job.state = JOB_IDLE;
    pthread_mutex_unlock(&g_lock);
}

int
jobs_is_busy(void) {
    int busy;
    pthread_mutex_lock(&g_lock);
    busy = (g_job.state == JOB_RUNNING);
    pthread_mutex_unlock(&g_lock);
    return busy;
}

static void
set_message(job_state_t state, const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&g_lock);
    g_job.state = state;
    va_start(ap, fmt);
    vsnprintf(g_job.message, sizeof(g_job.message), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_lock);
}

/* --- destination sink ---------------------------------------------------- */

typedef struct {
    char root[1024];
} sink_ctx_t;

static int
sink_write(void *user, const char *dir, const char *name,
           const unsigned char *data, size_t len) {
    sink_ctx_t *ctx = (sink_ctx_t *)user;
    char        rel[1024];
    char        full[2048];

    if(!fs_relpath_is_safe(name)) return -1;
    if(dir && dir[0]) {
        if(!fs_relpath_is_safe(dir)) return -1;
        snprintf(rel, sizeof(rel), "%s/%s", dir, name);
    } else {
        snprintf(rel, sizeof(rel), "%s", name);
    }

    fs_path_join(full, sizeof(full), ctx->root, rel);
    return fs_write_file(full, data, len);
}

static int
sink_progress(void *user, const char *path, uint64_t done, uint64_t total) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    g_job.done_bytes = done;
    g_job.total_bytes = total;
    g_job.file_index++;
    snprintf(g_job.current_file, sizeof(g_job.current_file), "%s", path);
    pthread_mutex_unlock(&g_lock);
    return g_cancel ? 1 : 0;
}

/* --- worker -------------------------------------------------------------- */

static void *
worker(void *arg) {
    fpkg_t      pkg;
    char        err[512];
    fpkg_sink_t sink;
    sink_ctx_t  ctx;
    char        pkg_path[1024];
    uint64_t    needed;
    uint64_t    avail = 0;

    (void)arg;
    fpkg_init(&pkg);

    pthread_mutex_lock(&g_lock);
    snprintf(pkg_path, sizeof(pkg_path), "%s", g_job.package);
    snprintf(ctx.root, sizeof(ctx.root), "%s", g_job.dest);
    pthread_mutex_unlock(&g_lock);

    if(fpkg_open(&pkg, pkg_path, err, sizeof(err)) != 0) {
        set_message(JOB_FAILED, "%s", err);
        goto out;
    }

    needed = fpkg_total_uncompressed(&pkg);

    pthread_mutex_lock(&g_lock);
    g_job.total_bytes = needed;
    g_job.file_total = pkg.entry_count;
    g_job.file_index = 0;
    pthread_mutex_unlock(&g_lock);

    if(fs_space(ctx.root, NULL, &avail) == 0 && avail < needed) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "not enough space in %s: needs %llu bytes, %llu available",
                 ctx.root, (unsigned long long)needed,
                 (unsigned long long)avail);
        set_message(JOB_FAILED, "%s", msg);
        goto out;
    }

    memset(&sink, 0, sizeof(sink));
    sink.write = sink_write;
    sink.progress = sink_progress;
    sink.user = &ctx;

    if(fpkg_extract_all(&pkg, &sink, err, sizeof(err)) != 0) {
        set_message(g_cancel ? JOB_CANCELLED : JOB_FAILED, "%s", err);
        goto out;
    }

    set_message(g_cancel ? JOB_CANCELLED : JOB_DONE,
                "extracted %d files (%llu bytes) to %s",
                pkg.entry_count, (unsigned long long)needed, ctx.root);

out:
    pthread_mutex_lock(&g_lock);
    g_job.files_written = pkg.entry_count;
    pthread_mutex_unlock(&g_lock);

    fpkg_close(&pkg);
    return 0;
}

int
jobs_start(const char *package_path, const char *dest_dir) {
    if(!package_path || !package_path[0] || !dest_dir || !dest_dir[0]) return -1;

    pthread_mutex_lock(&g_lock);
    if(g_job.state == JOB_RUNNING) {
        pthread_mutex_unlock(&g_lock);
        return -2;
    }
    memset(&g_job, 0, sizeof(g_job));
    snprintf(g_job.package, sizeof(g_job.package), "%s", package_path);
    snprintf(g_job.dest, sizeof(g_job.dest), "%s", dest_dir);
    g_job.state = JOB_RUNNING;
    g_cancel = 0;
    pthread_mutex_unlock(&g_lock);

    if(fs_mkdir_p(dest_dir) != 0) {
        set_message(JOB_FAILED, "cannot create destination directory %s", dest_dir);
        return -1;
    }

    if(pthread_create(&g_thread, 0, worker, 0) != 0) {
        set_message(JOB_FAILED, "cannot start worker thread");
        return -1;
    }
    pthread_detach(g_thread);
    return 0;
}

void
jobs_cancel(void) {
    g_cancel = 1;
}

void
jobs_status_json(char *out, size_t out_size) {
    job_t      snap;
    char       esc_pkg[1200];
    char       esc_dest[1200];
    char       esc_msg[1024];
    char       esc_cur[1024];
    const char *st;
    int        percent;

    pthread_mutex_lock(&g_lock);
    memcpy(&snap, &g_job, sizeof(snap));
    pthread_mutex_unlock(&g_lock);

    st = state_name(snap.state);
    percent = snap.total_bytes
        ? (int)((snap.done_bytes * 100) / snap.total_bytes)
        : (snap.state == JOB_DONE ? 100 : 0);
    if(percent > 100) percent = 100;

    httpd_json_escape(snap.package, esc_pkg, sizeof(esc_pkg));
    httpd_json_escape(snap.dest, esc_dest, sizeof(esc_dest));
    httpd_json_escape(snap.message, esc_msg, sizeof(esc_msg));
    httpd_json_escape(snap.current_file, esc_cur, sizeof(esc_cur));

    snprintf(out, out_size,
             "{\"ok\":true,\"state\":\"%s\",\"busy\":%s,"
             "\"package\":\"%s\",\"dest\":\"%s\",\"message\":\"%s\","
             "\"doneBytes\":%llu,\"totalBytes\":%llu,\"percent\":%d,"
             "\"filesWritten\":%d,\"fileTotal\":%d,\"currentFile\":\"%s\"}",
             st,
             snap.state == JOB_RUNNING ? "true" : "false",
             esc_pkg, esc_dest, esc_msg,
             (unsigned long long)snap.done_bytes,
             (unsigned long long)snap.total_bytes,
             percent,
             snap.files_written, snap.file_total, esc_cur);
}
