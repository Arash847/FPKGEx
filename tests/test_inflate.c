/* Host-only test harness for the DEFLATE decoder. Not part of the payload. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/inflate.h"

static unsigned char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if(!buf) { fclose(f); return NULL; }
    if(fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    size_t clen = 0, plen = 0, out_written = 0;
    unsigned char *comp, *plain;
    size_t expect;
    int rc;
    const char *mode = argc > 2 ? argv[2] : "raw";

    if(argc < 3) { fprintf(stderr, "usage: %s <plain> <comp> [raw|zlib]\n", argv[0]); return 2; }

    plain = slurp(argv[1], &plen);
    comp  = slurp(argv[2], &clen);
    if(!plain || !comp) { fprintf(stderr, "read failed\n"); return 2; }

    expect = plen;
    {
        unsigned char *out = (unsigned char *)malloc(expect ? expect : 1);
        if(!out) { fprintf(stderr, "oom\n"); return 2; }

        if(strcmp(mode, "zlib") == 0) rc = fpkg_inflate_auto(comp, clen, out, expect, &out_written);
        else                        rc = fpkg_inflate_raw(comp, clen, out, expect, &out_written);

        printf("rc=%d written=%zu expect=%zu\n", rc, out_written, expect);
        if(rc != 0) { printf("FAIL: inflate rc=%d\n", rc); return 1; }
        if(out_written != expect) { printf("FAIL: length %zu != %zu\n", out_written, expect); return 1; }
        if(expect && memcmp(out, plain, expect) != 0) {
            size_t i;
            for(i = 0; i < expect; i++) {
                if(out[i] != plain[i]) { printf("FAIL: first diff at byte %zu\n", i); break; }
            }
            return 1;
        }
        printf("PASS\n");
        free(out);
    }
    free(plain);
    free(comp);
    return 0;
}
