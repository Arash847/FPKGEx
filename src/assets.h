/*
 * assets.h - Web UI assets embedded into the payload at build time.
 *
 * The table itself lives in gen/assets.c, produced by tools/embed_assets.py.
 */
#ifndef FPKEX_ASSETS_H
#define FPKEX_ASSETS_H

#include <stddef.h>

typedef struct {
    const char          *path;   /* request path, e.g. "/app.js" */
    const char          *mime;
    const unsigned char *data;
    size_t               size;
} asset_t;

/* Returns NULL when no asset matches the path. */
const asset_t *asset_find(const char *path);

#endif /* FPKEX_ASSETS_H */
