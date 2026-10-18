/*
 * jsonutil.c - Tiny read-only JSON helpers for package metadata.
 */
#include "jsonutil.h"

#include <string.h>

/* Locates `"key"` and returns a pointer just past the colon. */
static const char *
find_key_impl(const char *json, size_t len, const char *key) {
    size_t klen = strlen(key);
    size_t i;

    if(klen + 2 >= 128) return 0;

    for(i = 0; i + klen + 2 <= len; i++) {
        if(json[i] != '"') continue;
        if(memcmp(json + i + 1, key, klen) != 0) continue;
        if(json[i + 1 + klen] != '"') continue;

        /* Skip whitespace, then the colon. */
        i = i + klen + 2;
        while(i < len && (json[i] == ' ' || json[i] == '\t' ||
                          json[i] == '\n' || json[i] == '\r')) i++;
        if(i < len && json[i] == ':') return json + i + 1;
        return 0;
    }
    return 0;
}

int
ju_get_string(const char *json, size_t len, const char *key,
              char *out, size_t out_size) {
    const char *v;
    size_t      n = 0;

    if(!out || out_size == 0) return 0;
    out[0] = 0;
    if(!json) return 0;

    v = find_key_impl(json, len, key);
    if(!v) return 0;
    while((size_t)(v - json) < len &&
          (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
    if(*v != '"') return 0;
    v++;

    while((size_t)(v - json) < len && *v != '"') {
        char c = *v++;
        if(c == '\\' && (size_t)(v - json) < len) {
            char e = *v++;
            switch(e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = e;   break;   /* \" \\ \/ \uXXXX handled below */
            }
            if(e == 'u' && (size_t)(v - json) + 3 < len) {
                /* Keep it simple: drop the code point rather than emit UTF-8. */
                v += 3;
                c = '?';
            }
        }
        if(n + 1 < out_size) out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

int
ju_get_number(const char *json, size_t len, const char *key,
              char *out, size_t out_size) {
    const char *v;
    size_t      n = 0;

    if(!out || out_size == 0) return 0;
    out[0] = 0;
    if(!json) return 0;

    v = find_key_impl(json, len, key);
    if(!v) return 0;
    while((size_t)(v - json) < len &&
          (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;

    while((size_t)(v - json) < len && n + 1 < out_size) {
        char c = *v;
        if((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
           c == 'e' || c == 'E') {
            out[n++] = c;
            v++;
        } else {
            break;
        }
    }
    out[n] = 0;
    return n > 0;
}

int
ju_has_key(const char *json, size_t len, const char *key) {
    return find_key_impl(json, len, key) != 0;
}
