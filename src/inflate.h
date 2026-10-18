/*
 * inflate.h - Minimal raw DEFLATE / zlib decompressor.
 *
 * Only what is needed to read ZIP entries inside a package's segment-info
 * tail. Written to be freestanding: no libc beyond string.h and stddef.h.
 */
#ifndef FPKG_INFLATE_H
#define FPKG_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Inflates a raw DEFLATE stream (RFC 1951) from `in` into `out`.
 * Returns 0 on success, -1 on malformed input, -2 if `out` is too small.
 */
int fpkg_inflate_raw(const unsigned char *in, size_t in_len,
                     unsigned char *out, size_t out_len, size_t *out_written);

/*
 * Inflates a stream that may carry a 2-byte zlib header (RFC 1950) or be a
 * raw DEFLATE stream. Handles both transparently.
 */
int fpkg_inflate_auto(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_len, size_t *out_written);

#endif /* FPKG_INFLATE_H */
