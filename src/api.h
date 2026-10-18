/*
 * api.h - JSON endpoints for the web UI.
 */
#ifndef FPKEX_API_H
#define FPKEX_API_H

#include "httpd.h"

void api_init(void);

/*
 * Handles one request. Returns the status code that was sent, or 404 to let
 * the static asset server try the path.
 */
int  api_route(const httpd_request_t *req, void *user);

/* Fills `out` with the console's LAN address, or a placeholder. */
void api_detect_lan_ip(char *out, size_t out_size);

#endif /* FPKEX_API_H */
