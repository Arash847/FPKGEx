/*
 * httpd.c - Tiny blocking HTTP/1.1 server for the payload's web UI.
 *
 * One thread per connection with a bounded stack: the console is happy with a
 * handful of concurrent browser tabs and this keeps request handling trivial.
 */
#include "httpd.h"
#include "assets.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HEADER_MAX      (64 * 1024)
#define BODY_MAX        (1024 * 1024)
#define CLIENT_STACK    (512 * 1024)
#define IDLE_TIMEOUT_S  60
#define READ_TIMEOUT_S  300
#define HANDLER_TIMEOUT_S 600

static int              g_srvfd = -1;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int     g_stop = 0;

const char *
httpd_status_text(int status) {
    switch(status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

static int
write_all(int fd, const void *data, size_t size) {
    const char *p = (const char *)data;
    while(size > 0) {
        ssize_t n = send(fd, p, size, 0);
        if(n < 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        if(n == 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

int
httpd_send(int fd, int status, const char *mime, const void *data, size_t len) {
    char header[1024];
    int  n = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Connection: close\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
                      "Pragma: no-cache\r\n"
                      "Expires: 0\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n"
                      "\r\n",
                      status, httpd_status_text(status),
                      mime ? mime : "application/octet-stream",
                      (unsigned long)len);
    if(n < 0 || (size_t)n >= sizeof(header)) return -1;
    if(write_all(fd, header, (size_t)n) != 0) return -1;
    if(len == 0) return 0;
    return write_all(fd, data, len);
}

int
httpd_send_text(int fd, int status, const char *mime, const char *body) {
    return httpd_send(fd, status, mime, body ? body : "",
                      body ? strlen(body) : 0);
}

int
httpd_send_json(int fd, int status, const char *json) {
    return httpd_send_text(fd, status, "application/json", json);
}

void
httpd_json_escape(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    if(out_size == 0) return;
    out[0] = 0;
    for(; in && *in && o + 7 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        switch(c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if(c < 0x20) {
                    o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    out[o] = 0;
}

int
httpd_send_error(int fd, int status, const char *message) {
    char escaped[512];
    char body[640];
    httpd_json_escape(message ? message : "error", escaped, sizeof(escaped));
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    return httpd_send_json(fd, status, body);
}

static int
hex_value(int ch) {
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void
httpd_url_decode(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    if(out_size == 0) return;
    for(; in && *in && o + 1 < out_size; in++) {
        if(*in == '%' && isxdigit((unsigned char)in[1]) &&
           isxdigit((unsigned char)in[2])) {
            out[o++] = (char)((hex_value(in[1]) << 4) | hex_value(in[2]));
            in += 2;
        } else if(*in == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

int
httpd_query_arg(const httpd_request_t *req, const char *name,
                char *out, size_t out_size) {
    char copy[HTTPD_PATH_MAX];
    char *save = NULL;
    char *part;

    if(!req || !name || !out || out_size == 0) return 0;
    out[0] = 0;
    snprintf(copy, sizeof(copy), "%s", req->query);

    for(part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(part, '=');
        char  key[256];
        if(!eq) continue;
        *eq = 0;
        httpd_url_decode(part, key, sizeof(key));
        if(strcmp(key, name) != 0) continue;
        httpd_url_decode(eq + 1, out, out_size);
        return 1;
    }
    return 0;
}

int
httpd_body_string(const httpd_request_t *req, const char *key,
                  char *out, size_t out_size) {
    char   pattern[128];
    const char *p;

    if(!req || !req->body || !key || !out || out_size == 0) return 0;
    out[0] = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(req->body, pattern);
    if(!p) return 0;
    p += strlen(pattern);
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if(*p != ':') return 0;
    p++;
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if(*p != '"') return 0;
    p++;

    {
        size_t o = 0;
        while(*p && *p != '"' && o + 1 < out_size) {
            if(*p == '\\' && p[1]) {
                p++;
                switch(*p) {
                    case 'n': out[o++] = '\n'; break;
                    case 'r': out[o++] = '\r'; break;
                    case 't': out[o++] = '\t'; break;
                    default:  out[o++] = *p;   break;
                }
                p++;
                continue;
            }
            out[o++] = *p++;
        }
        out[o] = 0;
    }
    return 1;
}

/* Case-insensitive search for a header name; returns NULL when absent. */
static const char *
find_header(const char *header, const char *name) {
    size_t nlen = strlen(name);
    const char *p = header;

    while(*p) {
        if((size_t)(strchr(p, '\n') - p) >= nlen) {
            size_t i;
            for(i = 0; i < nlen; i++) {
                if(tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) break;
            }
            if(i == nlen) return p;
        }
        p = strchr(p, '\n');
        if(!p) break;
        p++;
    }
    return 0;
}

static char *
read_request(int fd, size_t *header_len_out) {
    static char header[HEADER_MAX];
    size_t      used = 0;

    for(;;) {
        ssize_t n = recv(fd, header + used, HEADER_MAX - 1 - used, 0);
        if(n < 0) {
            if(errno == EINTR) continue;
            return 0;
        }
        if(n == 0) {
            if(used == 0) return 0;
            break;
        }
        used += (size_t)n;
        header[used] = 0;

        if(strstr(header, "\r\n\r\n")) break;
        if(used >= HEADER_MAX - 1) return 0;
    }

    *header_len_out = used;
    return header;
}

static int
serve_static(int fd, const char *path) {
    const asset_t *a = asset_find(path);
    if(!a) return 404;
    return httpd_send(fd, 200, a->mime, a->data, a->size) == 0 ? 200 : 500;
}

typedef struct {
    int               fd;
    httpd_handler_fn  handler;
    void             *user;
} client_arg_t;

static void *
client_thread(void *arg) {
    client_arg_t       *ca = (client_arg_t *)arg;
    int                 fd = ca->fd;
    httpd_handler_fn    handler = ca->handler;
    void               *user = ca->user;
    char               *header;
    size_t              header_len = 0;
    httpd_request_t     req;
    struct sockaddr_in  peer;
    socklen_t           peer_len = sizeof(peer);
    struct timeval      tv;
    char               *body = 0;
    size_t              content_length = 0;
    char               *line_end;
    char                target[HTTPD_PATH_MAX];
    char                decoded[HTTPD_PATH_MAX];

    free(ca);

    tv.tv_sec = IDLE_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    header = read_request(fd, &header_len);
    if(!header) { close(fd); return 0; }

    memset(&req, 0, sizeof(req));
    req.fd = fd;
    if(getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, req.client_ip, sizeof(req.client_ip));
    } else {
        snprintf(req.client_ip, sizeof(req.client_ip), "0.0.0.0");
    }

    /* Request line: METHOD SP TARGET SP VERSION */
    if(sscanf(header, "%7s %1023s", req.method, target) != 2) {
        httpd_send_error(fd, 400, "malformed request line");
        close(fd);
        return 0;
    }

    line_end = strchr(target, '?');
    if(line_end) {
        *line_end = 0;
        snprintf(req.query, sizeof(req.query), "%s", line_end + 1);
    } else {
        req.query[0] = 0;
    }

    httpd_url_decode(target, decoded, sizeof(decoded));
    snprintf(req.path, sizeof(req.path), "%s", decoded);

    /* Content-Length body. */
    {
        const char *cl = find_header(header, "content-length:");
        if(cl) {
            const char *v = cl + strlen("content-length:");
            while(*v == ' ' || *v == '\t') v++;
            content_length = (size_t)strtoul(v, 0, 10);
        }
    }
    if(content_length > BODY_MAX) {
        httpd_send_error(fd, 413, "request body too large");
        close(fd);
        return 0;
    }
    if(content_length > 0) {
        const char *sep = strstr(header, "\r\n\r\n");
        size_t      header_only = sep ? (size_t)(sep - header) + 4 : header_len;
        size_t      have = header_len > header_only ? header_len - header_only : 0;
        size_t      got;

        body = (char *)malloc(content_length + 1);
        if(!body) {
            httpd_send_error(fd, 500, "out of memory");
            close(fd);
            return 0;
        }
        if(have > content_length) have = content_length;
        if(have) memcpy(body, header + header_only, have);

        tv.tv_sec = READ_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        got = have;
        while(got < content_length) {
            ssize_t n = recv(fd, body + got, content_length - got, 0);
            if(n < 0) {
                if(errno == EINTR) continue;
                break;
            }
            if(n == 0) break;
            got += (size_t)n;
        }
        body[got] = 0;
        req.body = body;
        req.body_len = got;
    }

    if(handler) {
        /*
         * Handlers that walk an external drive can spend a long time before
         * they produce their first byte, and the client is blocked reading
         * meanwhile. Give the socket a longer leash than the request read
         * above, otherwise a large USB directory trips the idle timeout and the
         * browser sees an empty reply.
         */
        struct timeval htv;
        htv.tv_sec = HANDLER_TIMEOUT_S;
        htv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &htv, sizeof(htv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &htv, sizeof(htv));

        int status = handler(&req, user);
        if(status != 404) {
            close(fd);
            free(body);
            return 0;
        }
    }

    serve_static(fd, req.path);
    close(fd);
    free(body);
    return 0;
}

void
httpd_stop(void) {
    int fd;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    fd = g_srvfd;
    g_srvfd = -1;
    pthread_mutex_unlock(&g_lock);
    if(fd >= 0) close(fd);
}

int
httpd_serve(unsigned short port, httpd_handler_fn handler, void *user,
            void (*on_ready)(unsigned short port, void *arg), void *ready_arg) {
    struct sockaddr_in addr;
    int                opt = 1;

    signal(SIGPIPE, SIG_IGN);

    g_srvfd = socket(AF_INET, SOCK_STREAM, 0);
    if(g_srvfd < 0) return -1;

    setsockopt(g_srvfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(g_srvfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
    if(listen(g_srvfd, 16) != 0) return -1;

    if(on_ready) on_ready(port, ready_arg);

    while(!g_stop) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int                fd = accept(g_srvfd, (struct sockaddr *)&peer, &peer_len);
        pthread_t          tid;
        pthread_attr_t     attr;
        client_arg_t      *ca;

        if(fd < 0) {
            if(errno == EINTR) continue;
            break;
        }

        ca = (client_arg_t *)calloc(1, sizeof(*ca));
        if(!ca) { close(fd); continue; }
        ca->fd = fd;
        ca->handler = handler;
        ca->user = user;

        if(pthread_attr_init(&attr) != 0) { free(ca); close(fd); continue; }
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, CLIENT_STACK);
        if(pthread_create(&tid, &attr, client_thread, ca) != 0) {
            pthread_attr_destroy(&attr);
            free(ca);
            close(fd);
            continue;
        }
        pthread_attr_destroy(&attr);
    }

    return 0;
}
