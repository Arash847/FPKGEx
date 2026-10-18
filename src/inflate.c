/*
 * inflate.c - Minimal raw DEFLATE / zlib decompressor.
 *
 * Canonical-Huffman decoder following the structure of Mark Adler's "puff"
 * reference decoder: per-symbol bit counts plus a sorted symbol table, which
 * keeps the state small enough to live comfortably in a payload heap.
 */
#include "inflate.h"
#include <string.h>

#ifdef FPKG_INFLATE_DEBUG
#include <stdio.h>
#define IDBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define IDBG(...) do { } while(0)
#endif

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    const unsigned char *in;
    size_t               in_len;
    size_t               in_pos;
    int                  bit_buf;
    int                  bit_cnt;
    unsigned char       *out;
    size_t               out_len;
    size_t               out_pos;
    int                  overflow;   /* output did not fit */
} inflate_state_t;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[MAXCODES];
} huffman_t;

/* Bit reader: returns -1 when the input is exhausted. */
static int
bits(inflate_state_t *s, int need) {
    long val = s->bit_buf;
    while(s->bit_cnt < need) {
        if(s->in_pos == s->in_len) return -1;
        val |= (long)(s->in[s->in_pos++]) << s->bit_cnt;
        s->bit_cnt += 8;
    }
    s->bit_buf = (int)(val >> need);
    s->bit_cnt -= need;
    return (int)(val & ((1L << need) - 1));
}

/* Builds a canonical Huffman decoding table from code lengths. */
static int
construct(huffman_t *h, const short *length, int n) {
    int   symbol, len, left;
    short offs[MAXBITS + 1];

    for(len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for(symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if(h->count[0] == n) return 0;   /* no symbols: incomplete but unused */

    left = 1;
    for(len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if(left < 0) return -1;       /* over-subscribed */
    }

    offs[1] = 0;
    for(len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for(symbol = 0; symbol < n; symbol++) {
        if(length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    }
    return left;
}

/* Walks the Huffman tree one bit at a time. */
static int
decode(inflate_state_t *s, const huffman_t *h) {
    int len, code = 0, first = 0, index = 0, count;

    for(len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if(b < 0) return -1;
        code |= b;
        count = h->count[len];
        if(code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int
out_byte(inflate_state_t *s, unsigned char b) {
    if(s->out_pos == s->out_len) {
        s->overflow = 1;
        return -1;
    }
    s->out[s->out_pos++] = b;
    return 0;
}

static int
stored_block(inflate_state_t *s) {
    unsigned len, nlen;
    int b;

    s->bit_buf = 0;
    s->bit_cnt = 0;
    if(s->in_pos + 4 > s->in_len) return -1;
    len = s->in[s->in_pos] | ((unsigned)s->in[s->in_pos + 1] << 8);
    nlen = s->in[s->in_pos + 2] | ((unsigned)s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if((len ^ 0xFFFFu) != nlen) return -1;
    if(s->in_pos + len > s->in_len) return -1;
    if(s->out_pos + len > s->out_len) {
        s->overflow = 1;
        return -2;
    }
    if(len) memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->out_pos += len;
    s->in_pos += len;
    (void)b;
    return 0;
}

static int
codes(inflate_state_t *s, const huffman_t *lencode, const huffman_t *distcode) {
    static const short lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const short lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const short dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const short dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for(;;) {
        int symbol = decode(s, lencode);
        if(symbol < 0) return -1;

        if(symbol < 256) {
            if(out_byte(s, (unsigned char)symbol) != 0) return s->overflow ? -2 : -1;
        } else if(symbol == 256) {
            return 0;                 /* end of block */
        } else {
            int len, dist, extra;
            size_t src;

            symbol -= 257;
            if(symbol >= 29) return -1;
            extra = bits(s, lext[symbol]);
            if(extra < 0) return -1;
            len = lens[symbol] + extra;

            symbol = decode(s, distcode);
            if(symbol < 0 || symbol >= 30) return -1;
            extra = bits(s, dext[symbol]);
            if(extra < 0) return -1;
            dist = dists[symbol] + extra;

            if((size_t)dist > s->out_pos) return -1;       /* before start */
            src = s->out_pos - (size_t)dist;
            if(s->out_pos + (size_t)len > s->out_len) {
                s->overflow = 1;
                return -2;
            }
            /* Byte-at-a-time on purpose: overlapping runs rely on this. */
            while(len-- > 0) {
                s->out[s->out_pos] = s->out[src++];
                s->out_pos++;
            }
        }
    }
}

static int
fixed_block(inflate_state_t *s) {
    static int   virgin = 1;
    static huffman_t lencode, distcode;

    if(virgin) {
        short lengths[FIXLCODES];
        int   symbol;

        for(symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for(; symbol < 256; symbol++) lengths[symbol] = 9;
        for(; symbol < 280; symbol++) lengths[symbol] = 7;
        for(; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);

        for(symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);
        virgin = 1;   /* tables stay valid for the process lifetime */
    }
    return codes(s, &lencode, &distcode);
}

static int
dynamic_block(inflate_state_t *s) {
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int      nlen, ndist, ncode, index, symbol, err;
    short    lengths[MAXCODES];
    huffman_t lencode, distcode;

    if((nlen = bits(s, 5)) < 0) return -1;
    nlen += 257;
    if((ndist = bits(s, 5)) < 0) return -1;
    ndist += 1;
    if((ncode = bits(s, 4)) < 0) return -1;
    ncode += 4;
    if(nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    for(index = 0; index < ncode; index++) {
        if((symbol = bits(s, 3)) < 0) return -1;
        lengths[order[index]] = (short)symbol;
    }
    for(; index < 19; index++) lengths[order[index]] = 0;

    err = construct(&lencode, lengths, 19);
    IDBG("dyn: nlen=%d ndist=%d ncode=%d clenode_err=%d\n", nlen, ndist, ncode, err);
    if(err != 0) return -1;

    index = 0;
    while(index < nlen + ndist) {
        symbol = decode(s, &lencode);
        IDBG("  cl[sym=%d idx=%d]\n", symbol, index);
        if(symbol < 0) return -1;

        if(symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int rep, value = 0;

            if(symbol == 16) {
                /* Repeat the previous code length 3..6 times. */
                if(index == 0) return -1;
                value = lengths[index - 1];
                if((rep = bits(s, 2)) < 0) return -1;
                rep += 3;
            } else if(symbol == 17) {
                /* Repeat zero 3..10 times. */
                if((rep = bits(s, 3)) < 0) return -1;
                rep += 3;
            } else {
                /* Repeat zero 11..138 times. */
                if((rep = bits(s, 7)) < 0) return -1;
                rep += 11;
            }
            if(index + rep > nlen + ndist) return -1;
            while(rep-- > 0) lengths[index++] = (short)value;
        }
    }
    IDBG("dyn: filled=%d want=%d\n", index, nlen + ndist);

    if(lengths[256] == 0) return -1;   /* no end-of-block code */

    err = construct(&lencode, lengths, nlen);
    IDBG("dyn: litlen_err=%d sym256_len=%d\n", err, (int)lengths[256]);
    if(err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    IDBG("dyn: dist_err=%d\n", err);
    if(err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -1;

    return codes(s, &lencode, &distcode);
}

int
fpkg_inflate_raw(const unsigned char *in, size_t in_len,
                 unsigned char *out, size_t out_len, size_t *out_written) {
    inflate_state_t s;
    int             last, type, err = 0;

    memset(&s, 0, sizeof(s));
    s.in = in;
    s.in_len = in_len;
    s.out = out;
    s.out_len = out_len;

    do {
        last = bits(&s, 1);
        if(last < 0) { err = -1; break; }
        type = bits(&s, 2);
        if(type < 0) { err = -1; break; }

        if(type == 0)      err = stored_block(&s);
        else if(type == 1) err = fixed_block(&s);
        else if(type == 2) err = dynamic_block(&s);
        else               err = -1;

        IDBG("block type=%d last=%d err=%d out=%zu\n", type, last, err, s.out_pos);
        if(err) break;
    } while(!last);

    if(out_written) *out_written = s.out_pos;
    if(err) return err;
    if(s.out_pos != out_len) return -3;   /* short stream */
    return 0;
}

int
fpkg_inflate_auto(const unsigned char *in, size_t in_len,
                  unsigned char *out, size_t out_len, size_t *out_written) {
    int rc;

    /* zlib header: CMF/FLG with CM == 8 and a valid FCHECK. */
    if(in_len >= 2 && (in[0] & 0x0F) == 8 &&
       ((((unsigned)in[0] << 8) | in[1]) % 31u) == 0) {
        return fpkg_inflate_raw(in + 2, in_len - 2, out, out_len, out_written);
    }
    rc = fpkg_inflate_raw(in, in_len, out, out_len, out_written);
    if(rc == 0) return rc;

    /* Fall back to raw when the zlib assumption produced a short stream. */
    if(rc == -3) {
        rc = fpkg_inflate_raw(in + 2, in_len - 2, out, out_len, out_written);
        if(rc == 0) return rc;
    }
    return rc;
}
