/*
 * httpd.h - Tiny blocking HTTP/1.1 server for the payload's web UI.
 */
#ifndef FPKEX_HTTPD_H
#define FPKEX_HTTPD_H

#include <stddef.h>

#define HTTPD_PATH_MAX 1024

typedef struct {
    int         fd;
    char        method[8];
    char        path[HTTPD_PATH_MAX];
    char        query[HTTPD_PATH_MAX];
    const char *body;
    size_t      body_len;
    char        client_ip[64];
} httpd_request_t;

/* Route handler. Return the HTTP status code to send. */
typedef int (*httpd_handler_fn)(const httpd_request_t *req, void *user);

int  httpd_send(int fd, int status, const char *mime,
                const void *data, size_t len);
int  httpd_send_text(int fd, int status, const char *mime, const char *body);
int  httpd_send_json(int fd, int status, const char *json);
int  httpd_send_error(int fd, int status, const char *message);

void httpd_json_escape(const char *in, char *out, size_t out_size);
void httpd_url_decode(const char *in, char *out, size_t out_size);

/* Looks up a query argument. Returns 1 when found. */
int  httpd_query_arg(const httpd_request_t *req, const char *name,
                     char *out, size_t out_size);

/* Reads and JSON-decodes a string field from the request body. */
int  httpd_body_string(const httpd_request_t *req, const char *key,
                       char *out, size_t out_size);

const char *httpd_status_text(int status);

/*
 * Serves until httpd_stop() is called. `handler` decides every request;
 * returning 404 lets the built-in static file server try the web/ assets.
 */
int  httpd_serve(unsigned short port, httpd_handler_fn handler, void *user,
                 void (*on_ready)(unsigned short port, void *arg), void *ready_arg);

void httpd_stop(void);

#endif /* FPKEX_HTTPD_H */
