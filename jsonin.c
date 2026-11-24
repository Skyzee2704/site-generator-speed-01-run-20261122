/* jsonin.c — streaming zero-copy/fused JSON parser for the fixed
 * site-generator input schema. Input bytes live in a read-only buffer
 * (`c->in`, typically mmap'd); decoded strings are written into the
 * working pool (`c->pool`), which rewinds between cases to stay L2-hot.
 * For `.md` files, JSON unescape + CRLF normalization + front-matter
 * boundary detection + draft/refused body skipping + SIMD word_count
 * are fused into a single pass. */
#include "zg.h"
#include <immintrin.h>

static inline uint8_t cur_peek(Cur *c) {
    return c->pos < c->end ? c->in[c->pos] : 0;
}

static inline void cur_ws(Cur *c) {
    const uint8_t *in = c->in;
    size_t pos = c->pos, end = c->end;
    while (pos < end) {
        uint8_t ch = in[pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') pos++;
        else break;
    }
    c->pos = pos;
}

static inline bool cur_byte(Cur *c, uint8_t b) {
    cur_ws(c);
    if (c->pos < c->end && c->in[c->pos] == b) { c->pos++; return true; }
    return false;
}

static bool cur_lit(Cur *c, const char *lit, size_t n) {
    if (c->end - c->pos >= n && cmp_bytes(c->in + c->pos, lit, n) == 0) {
        c->pos += n;
        return true;
    }
    return false;
}

static bool hex4_buf(const uint8_t *buf, size_t end, size_t *pos, uint32_t *out) {
    if (*pos + 4 > end) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t ch = buf[(*pos)++];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static inline void put_utf8_dst(uint8_t **wp, uint32_t cp) {
    uint8_t tmp[4];
    int n = u8_encode(cp, tmp);
    for (int k = 0; k < n; k++) *(*wp)++ = tmp[k];
}

/* Decode one escape at in[*rp] == '\\', writing to *wp. */
static bool unescape_one_to(const uint8_t *in, size_t end, size_t *rp, uint8_t **wp, uint8_t *has_cr) {
    size_t r = *rp;
    r++; /* skip '\\' */
    if (r >= end) return false;
    uint8_t e = in[r++];
    switch (e) {
        case '"': *(*wp)++ = '"'; break;
        case '\\': *(*wp)++ = '\\'; break;
        case '/': *(*wp)++ = '/'; break;
        case 'b': *(*wp)++ = 0x08; break;
        case 'f': *(*wp)++ = 0x0c; break;
        case 'n': *(*wp)++ = '\n'; break;
        case 'r': *(*wp)++ = '\r'; if (has_cr) *has_cr = 1; break;
        case 't': *(*wp)++ = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!hex4_buf(in, end, &r, &cp)) return false;
            if (cp == 0x0d && has_cr) *has_cr = 1;
            if (cp >= 0xd800 && cp < 0xdc00) {
                if (r + 1 < end && in[r] == '\\' && in[r + 1] == 'u') {
                    r += 2;
                    uint32_t lo;
                    if (!hex4_buf(in, end, &r, &lo)) return false;
                    if (lo == 0x0d && has_cr) *has_cr = 1;
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        put_utf8_dst(wp, 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00));
                    } else {
                        put_utf8_dst(wp, 0xfffd);
                        if (lo >= 0xd800 && lo < 0xe000) put_utf8_dst(wp, 0xfffd);
                        else put_utf8_dst(wp, lo);
                    }
                } else {
                    put_utf8_dst(wp, 0xfffd);
                }
            } else if (cp >= 0xdc00 && cp < 0xe000) {
                put_utf8_dst(wp, 0xfffd);
            } else {
                put_utf8_dst(wp, cp);
            }
            break;
        }
        default: return false;
    }
    *rp = r;
    return true;
}

/* Fast-skip from `r` (inside a JSON string) to just past its closing `"`. */
static bool skip_json_string_tail(const uint8_t *in, size_t end, size_t *rp) {
    size_t r = *rp;
    for (;;) {
        int j = scan_two(in, end, r, '"', '\\');
        if (j < 0) return false;
        r = (size_t)j;
        if (in[r] == '"') { *rp = r + 1; return true; }
        r += 2;
        if (r > end) return false;
    }
}

static const uint8_t POP8[256] = {
    0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8
};

static inline unsigned popcount16_sw(uint16_t x) {
    return (unsigned)POP8[x & 0xffu] + (unsigned)POP8[x >> 8];
}

static inline unsigned popcount32_sw(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (unsigned)((((x + (x >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24);
}

/* General JSON string unescape into pool. */
__attribute__((target("avx2,bmi")))
static bool parse_string_escaped_avx2(Cur *c, size_t start, size_t first_esc, Sp *out, uint8_t *has_cr_out) {
    const uint8_t *in = c->in;
    size_t end = c->end;
    Pool *p = c->pool;
    uint32_t out_off = (uint32_t)p->len;
    size_t prefix = first_esc - start;
    uint8_t *dst = p->base + out_off;
    uint8_t has_cr = 0;
    if (prefix > 0) {
        copy_fast(dst, in + start, prefix);
        if (has_cr_out && scan_byte(in, first_esc, start, '\r') >= 0) has_cr = 1;
        dst += prefix;
    }
    size_t r = first_esc;
    __m256i vq = _mm256_set1_epi8('"');
    __m256i vbs = _mm256_set1_epi8('\\');
    __m256i vcr = _mm256_set1_epi8('\r');
    __m256i any_cr = _mm256_setzero_si256();
    for (;;) {
        while (r + 32 <= end) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(in + r));
            if (has_cr_out) any_cr = _mm256_or_si256(any_cr, _mm256_cmpeq_epi8(v, vcr));
            uint32_t m = (uint32_t)_mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vbs)));
            _mm256_storeu_si256((__m256i *)dst, v);
            if (m == 0) {
                dst += 32; r += 32;
                continue;
            }
            size_t k = (size_t)__builtin_ctz(m);
            dst += k; r += k;
            if (in[r] == '"') {
                c->pos = r + 1;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                if (has_cr_out) *has_cr_out = (uint8_t)(has_cr | (_mm256_movemask_epi8(any_cr) != 0));
                return true;
            }
            do {
                if (__builtin_expect(r + 1 < end, 1)) {
                    uint8_t e = in[r + 1];
                    if (e == 'n') { *dst++ = '\n'; r += 2; continue; }
                    if (e == '"') { *dst++ = '"'; r += 2; continue; }
                    if (e == '\\') { *dst++ = '\\'; r += 2; continue; }
                    if (e == 't') { *dst++ = '\t'; r += 2; continue; }
                    if (e == 'r') { *dst++ = '\r'; has_cr = 1; r += 2; continue; }
                }
                if (!unescape_one_to(in, end, &r, &dst, &has_cr)) return false;
            } while (r < end && in[r] == '\\');
        }
        while (r < end) {
            uint8_t ch = in[r];
            if (ch == '"') {
                c->pos = r + 1;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                if (has_cr_out) *has_cr_out = (uint8_t)(has_cr | (_mm256_movemask_epi8(any_cr) != 0));
                return true;
            }
            if (ch == '\\') {
                if (!unescape_one_to(in, end, &r, &dst, &has_cr)) return false;
            } else {
                if (ch == '\r') has_cr = 1;
                *dst++ = ch; r++;
            }
        }
        return false;
    }
}

static bool parse_string_escaped(Cur *c, size_t start, size_t first_esc, Sp *out, uint8_t *has_cr_out) {
    if (g_has_avx2) return parse_string_escaped_avx2(c, start, first_esc, out, has_cr_out);
    const uint8_t *in = c->in;
    size_t end = c->end;
    Pool *p = c->pool;
    uint32_t out_off = (uint32_t)p->len;
    size_t prefix = first_esc - start;
    uint8_t *dst = p->base + out_off;
    uint8_t has_cr = 0;
    if (prefix > 0) {
        copy_fast(dst, in + start, prefix);
        if (has_cr_out && scan_byte(in, first_esc, start, '\r') >= 0) has_cr = 1;
        dst += prefix;
    }
    size_t r = first_esc;
    __m128i vq = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    __m128i vcr = _mm_set1_epi8('\r');
    __m128i any_cr = _mm_setzero_si128();
    for (;;) {
        while (r + 16 <= end) {
            __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
            if (has_cr_out) any_cr = _mm_or_si128(any_cr, _mm_cmpeq_epi8(v, vcr));
            uint32_t m = (uint32_t)_mm_movemask_epi8(
                _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)));
            _mm_storeu_si128((__m128i *)dst, v);
            if (m == 0) {
                dst += 16; r += 16;
                continue;
            }
            size_t k = (size_t)__builtin_ctz(m);
            dst += k; r += k;
            if (in[r] == '"') {
                c->pos = r + 1;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                if (has_cr_out) *has_cr_out = (uint8_t)(has_cr | (_mm_movemask_epi8(any_cr) != 0));
                return true;
            }
            do {
                if (__builtin_expect(r + 1 < end, 1)) {
                    uint8_t e = in[r + 1];
                    if (e == 'n') { *dst++ = '\n'; r += 2; continue; }
                    if (e == '"') { *dst++ = '"'; r += 2; continue; }
                    if (e == '\\') { *dst++ = '\\'; r += 2; continue; }
                    if (e == 't') { *dst++ = '\t'; r += 2; continue; }
                    if (e == 'r') { *dst++ = '\r'; has_cr = 1; r += 2; continue; }
                }
                if (!unescape_one_to(in, end, &r, &dst, &has_cr)) return false;
            } while (r < end && in[r] == '\\');
        }
        while (r < end) {
            uint8_t ch = in[r];
            if (ch == '"') {
                c->pos = r + 1;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                if (has_cr_out) *has_cr_out = (uint8_t)(has_cr | (_mm_movemask_epi8(any_cr) != 0));
                return true;
            }
            if (ch == '\\') {
                if (!unescape_one_to(in, end, &r, &dst, &has_cr)) return false;
            } else {
                if (ch == '\r') has_cr = 1;
                *dst++ = ch; r++;
            }
        }
        return false;
    }
}

static bool parse_string_ex(Cur *c, Sp *out, uint8_t *has_cr_out) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    size_t start = c->pos;
    int j = scan_two(c->in, c->end, start, '"', '\\');
    if (j < 0) return false;
    size_t abs = (size_t)j;
    if (c->in[abs] == '"') {
        size_t slen = abs - start;
        *out = pool_store(c->pool, c->in + start, slen);
        if (has_cr_out) *has_cr_out = (scan_byte(c->in, abs, start, '\r') >= 0) ? 1 : 0;
        c->pos = abs + 1;
        return true;
    }
    return parse_string_escaped(c, start, abs, out, has_cr_out);
}

static inline bool parse_string(Cur *c, Sp *out) {
    return parse_string_ex(c, out, 0);
}

/* Fast JSON escape lookup table: 0 means slow path (\r, \u, or invalid). */
static const uint8_t JSON_ESC_BYTE[256] = {
    ['"']  = '"',
    ['\\'] = '\\',
    ['/']  = '/',
    ['b']  = 0x08,
    ['f']  = 0x0c,
    ['n']  = '\n',
    ['t']  = '\t',
};

/* Slow handler for \r, \u, or raw '\r' at in[*rp]; updates *rp and *dstp. */
static bool unescape_md_slow(const uint8_t *in, size_t end, size_t *rp, uint8_t **dstp) {
    size_t r = *rp;
    uint8_t *dst = *dstp;
    uint8_t ch = in[r];
    if (ch == '\r') {
        *dst++ = '\n';
        r++;
        if (r < end && in[r] == '\n') r++;
        *rp = r; *dstp = dst;
        return true;
    }
    /* ch == '\\' */
    if (r + 1 < end && in[r + 1] == 'r') {
        *dst++ = '\n';
        r += 2;
        if (r + 1 < end && in[r] == '\\' && in[r + 1] == 'n') r += 2;
        else if (r < end && in[r] == '\n') r++;
        else if (r + 1 < end && in[r] == '\\' && in[r + 1] == 'u') {
            uint8_t tb[4], *tp = tb;
            if (!unescape_one_to(in, end, &r, &tp, 0)) return false;
            size_t nb = (size_t)(tp - tb);
            if (!(nb == 1 && tb[0] == '\n')) {
                for (size_t q = 0; q < nb; q++) *dst++ = tb[q];
            }
        }
        *rp = r; *dstp = dst;
        return true;
    }
    uint8_t *d0 = dst;
    if (!unescape_one_to(in, end, &r, &dst, 0)) return false;
    if (dst - d0 == 1 && *d0 == '\r') {
        *d0 = '\n';
        if (r + 1 < end && in[r] == '\\' && in[r + 1] == 'n') r += 2;
        else if (r < end && in[r] == '\n') r++;
        else if (r + 1 < end && in[r] == '\\' && in[r + 1] == 'u') {
            uint8_t tb[4], *tp = tb;
            if (!unescape_one_to(in, end, &r, &tp, 0)) return false;
            size_t nb = (size_t)(tp - tb);
            if (!(nb == 1 && tb[0] == '\n')) {
                for (size_t q = 0; q < nb; q++) *dst++ = tb[q];
            }
        }
    }
    *rp = r; *dstp = dst;
    return true;
}

/* Helper: check if any 0xC2 or 0xE0..0xEF lead byte in clean slice b[0..len)
 * could be a Unicode whitespace or truncated UTF-8 sequence.
 * Caller guarantees b[0..len+2] is readable (since r + 34 <= end / r + 18 <= end).
 * Continuation bytes 0x80..0xBF can never be '"', '\\', or '\r', so valid
 * multi-byte UTF-8 sequences crossing the vector boundary are safe to inspect! */
static inline bool has_unicode_space_or_edge(const uint8_t *b, size_t len, uint32_t spm) {
    while (spm) {
        size_t k = (size_t)__builtin_ctz(spm);
        spm &= spm - 1;
        if (k >= len) break;
        uint8_t c0 = b[k];
        if (c0 == 0xc2) {
            uint8_t c1 = b[k + 1];
            if ((c1 & 0xc0) != 0x80 || c1 == 0xa0) return true;
        } else {
            /* 0xE0..0xEF: 3-byte UTF-8 */
            uint8_t c1 = b[k + 1], c2 = b[k + 2];
            if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80) return true;
            uint32_t cp = ((uint32_t)(c0 & 0x0f) << 12) | ((uint32_t)(c1 & 0x3f) << 6) | (uint32_t)(c2 & 0x3f);
            if (u_is_space(cp)) return true;
        }
    }
    return false;
}

/* Fused jump-after-escape body unescape + CRLF norm + SIMD word_count (AVX2+POPCNT). */
__attribute__((target("avx2,bmi,popcnt")))
static bool unescape_md_body_wc_avx2(const uint8_t *in, size_t end, size_t *rp,
                                     uint8_t **dstp, bool prev_cr, uint32_t *wc_out) {
    size_t r = *rp;
    uint8_t *body_start = *dstp;
    uint8_t *dst = body_start;
    bool need_fallback_wc = false;

    if (prev_cr && r < end) {
        if (in[r] == '\n') { r++; }
        else if (in[r] == '\\' && r + 1 < end && in[r + 1] == 'n') { r += 2; }
        else if (in[r] == '\\' && r + 5 < end && in[r + 1] == 'u') {
            uint8_t tmpb[4], *tp = tmpb;
            if (!unescape_one_to(in, end, &r, &tp, 0)) return false;
            size_t nb = (size_t)(tp - tmpb);
            if (!(nb == 1 && tmpb[0] == '\n')) {
                for (size_t q = 0; q < nb; q++) *dst++ = tmpb[q];
                need_fallback_wc = true;
            }
        }
    }

    size_t count = 0;
    bool in_word = false;

    __m256i vq  = _mm256_set1_epi8('"');
    __m256i vbs = _mm256_set1_epi8('\\');
    __m256i vcr = _mm256_set1_epi8('\r');
    __m256i vsp = _mm256_set1_epi8(' ');
    __m256i v8  = _mm256_set1_epi8(8);
    __m256i v14 = _mm256_set1_epi8(14);
    __m256i maskf0 = _mm256_set1_epi8((char)0xf0);
    __m256i ve0 = _mm256_set1_epi8((char)0xe0);
    __m256i vc2 = _mm256_set1_epi8((char)0xc2);

    while (r + 34 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(in + r));
        __m256i spc = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vbs)),
            _mm256_cmpeq_epi8(v, vcr));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(spc);
        _mm256_storeu_si256((__m256i *)dst, v);

        __m256i wv = _mm256_or_si256(_mm256_cmpeq_epi8(v, vsp),
            _mm256_and_si256(_mm256_cmpgt_epi8(v, v8), _mm256_cmpgt_epi8(v14, v)));
        uint32_t ws = (uint32_t)_mm256_movemask_epi8(wv);

        if (__builtin_expect(m == 0, 1)) {
            if (__builtin_expect(_mm256_movemask_epi8(v) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm256_movemask_epi8(
                    _mm256_or_si256(_mm256_cmpeq_epi8(v, vc2),
                                    _mm256_cmpeq_epi8(_mm256_and_si256(v, maskf0), ve0)));
                if (spm && has_unicode_space_or_edge(in + r, 32, spm)) need_fallback_wc = true;
            }
            uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
            count += (size_t)__builtin_popcount((~ws) & prev);
            in_word = (ws >> 31) == 0;
            dst += 32; r += 32;
            continue;
        }

        size_t pos = (size_t)__builtin_ctz(m);
        if (pos > 0) {
            uint32_t valid = (1u << pos) - 1u;
            if (__builtin_expect((_mm256_movemask_epi8(v) & (int)valid) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm256_movemask_epi8(
                    _mm256_or_si256(_mm256_cmpeq_epi8(v, vc2),
                                    _mm256_cmpeq_epi8(_mm256_and_si256(v, maskf0), ve0))) & valid;
                if (spm && has_unicode_space_or_edge(in + r, pos, spm)) need_fallback_wc = true;
            }
            uint32_t wsv = ws & valid;
            uint32_t nonws = (~wsv) & valid;
            uint32_t prev = (wsv << 1) | (in_word ? 0u : 1u);
            count += (size_t)__builtin_popcount(nonws & prev);
            in_word = ((wsv >> (pos - 1)) & 1u) == 0;
            dst += pos;
            r += pos;
        }

        uint8_t ch = in[r];
        if (__builtin_expect(ch == '\\', 1)) {
            do {
                uint8_t e = in[r + 1];
                if (__builtin_expect(e == 'n', 1)) {
                    *dst++ = '\n';
                    r += 2;
                    in_word = false;
                    continue;
                }
                uint8_t dec = JSON_ESC_BYTE[e];
                if (__builtin_expect(dec != 0, 1)) {
                    *dst++ = dec;
                    r += 2;
                    if (dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
                if (!unescape_md_slow(in, end, &r, &dst)) return false;
                need_fallback_wc = true;
                break;
            } while (r + 1 < end && in[r] == '\\');
            continue;
        }
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        /* raw '\r' */
        if (!unescape_md_slow(in, end, &r, &dst)) return false;
        need_fallback_wc = true;
    }

    /* Tail scalar loop */
    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t dec = JSON_ESC_BYTE[in[r + 1]];
                if (dec != 0) {
                    *dst++ = dec; r += 2;
                    if (dec == '\n' || dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
            }
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else if (ch == '\r') {
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else {
            *dst++ = ch; r++;
            if (ch >= 0x80) {
                if (ch == 0xc2 || (ch & 0xf0) == 0xe0) need_fallback_wc = true;
                else if (!in_word) { in_word = true; count++; }
            } else if (ch == ' ' || (ch >= 9 && ch <= 13)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true; count++;
            }
        }
    }
    return false;
}

/* Fused jump-after-escape body unescape + CRLF norm + SIMD word_count (SSE2+POPCNT). */
__attribute__((target("popcnt")))
static bool unescape_md_body_wc_sse2_popcnt(const uint8_t *in, size_t end, size_t *rp,
                                            uint8_t **dstp, bool prev_cr, uint32_t *wc_out) {
    size_t r = *rp;
    uint8_t *body_start = *dstp;
    uint8_t *dst = body_start;
    bool need_fallback_wc = false;

    if (prev_cr && r < end) {
        if (in[r] == '\n') { r++; }
        else if (in[r] == '\\' && r + 1 < end && in[r + 1] == 'n') { r += 2; }
        else if (in[r] == '\\' && r + 5 < end && in[r + 1] == 'u') {
            uint8_t tmpb[4], *tp = tmpb;
            if (!unescape_one_to(in, end, &r, &tp, 0)) return false;
            size_t nb = (size_t)(tp - tmpb);
            if (!(nb == 1 && tmpb[0] == '\n')) {
                for (size_t q = 0; q < nb; q++) *dst++ = tmpb[q];
                need_fallback_wc = true;
            }
        }
    }

    size_t count = 0;
    bool in_word = false;

    __m128i vq  = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    __m128i vcr = _mm_set1_epi8('\r');
    __m128i vsp = _mm_set1_epi8(' ');
    __m128i v8  = _mm_set1_epi8(8);
    __m128i v14 = _mm_set1_epi8(14);
    __m128i maskf0 = _mm_set1_epi8((char)0xf0);
    __m128i ve0 = _mm_set1_epi8((char)0xe0);
    __m128i vc2 = _mm_set1_epi8((char)0xc2);

    while (r + 18 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
        __m128i spc = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)),
            _mm_cmpeq_epi8(v, vcr));
        uint32_t m = (uint32_t)_mm_movemask_epi8(spc);
        _mm_storeu_si128((__m128i *)dst, v);

        __m128i wv = _mm_or_si128(_mm_cmpeq_epi8(v, vsp),
            _mm_and_si128(_mm_cmpgt_epi8(v, v8), _mm_cmpgt_epi8(v14, v)));
        uint32_t ws = (uint32_t)_mm_movemask_epi8(wv);

        if (__builtin_expect(m == 0, 1)) {
            if (__builtin_expect(_mm_movemask_epi8(v) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm_movemask_epi8(
                    _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                 _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0)));
                if (spm && has_unicode_space_or_edge(in + r, 16, spm)) need_fallback_wc = true;
            }
            uint32_t nonws = (~ws) & 0xffffu;
            uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
            count += (size_t)__builtin_popcount(nonws & prev);
            in_word = (ws & 0x8000u) == 0;
            dst += 16; r += 16;
            continue;
        }

        size_t pos = (size_t)__builtin_ctz(m);
        if (pos > 0) {
            uint32_t valid = (1u << pos) - 1u;
            if (__builtin_expect((_mm_movemask_epi8(v) & (int)valid) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm_movemask_epi8(
                    _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                 _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0))) & valid;
                if (spm && has_unicode_space_or_edge(in + r, pos, spm)) need_fallback_wc = true;
            }
            uint32_t wsv = ws & valid;
            uint32_t nonws = (~wsv) & valid;
            uint32_t prev = (wsv << 1) | (in_word ? 0u : 1u);
            count += (size_t)__builtin_popcount(nonws & prev);
            in_word = ((wsv >> (pos - 1)) & 1u) == 0;
            dst += pos;
            r += pos;
        }

        uint8_t ch = in[r];
        if (__builtin_expect(ch == '\\', 1)) {
            do {
                uint8_t e = in[r + 1];
                if (__builtin_expect(e == 'n', 1)) {
                    *dst++ = '\n';
                    r += 2;
                    in_word = false;
                    continue;
                }
                uint8_t dec = JSON_ESC_BYTE[e];
                if (__builtin_expect(dec != 0, 1)) {
                    *dst++ = dec;
                    r += 2;
                    if (dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
                if (!unescape_md_slow(in, end, &r, &dst)) return false;
                need_fallback_wc = true;
                break;
            } while (r + 1 < end && in[r] == '\\');
            continue;
        }
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        if (!unescape_md_slow(in, end, &r, &dst)) return false;
        need_fallback_wc = true;
    }

    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t dec = JSON_ESC_BYTE[in[r + 1]];
                if (dec != 0) {
                    *dst++ = dec; r += 2;
                    if (dec == '\n' || dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
            }
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else if (ch == '\r') {
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else {
            *dst++ = ch; r++;
            if (ch >= 0x80) {
                if (ch == 0xc2 || (ch & 0xf0) == 0xe0) need_fallback_wc = true;
                else if (!in_word) { in_word = true; count++; }
            } else if (ch == ' ' || (ch >= 9 && ch <= 13)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true; count++;
            }
        }
    }
    return false;
}

/* Fused jump-after-escape body unescape + CRLF norm + SIMD word_count (pure SSE2). */
static bool unescape_md_body_wc_sse2(const uint8_t *in, size_t end, size_t *rp,
                                     uint8_t **dstp, bool prev_cr, uint32_t *wc_out) {
    size_t r = *rp;
    uint8_t *body_start = *dstp;
    uint8_t *dst = body_start;
    bool need_fallback_wc = false;

    if (prev_cr && r < end) {
        if (in[r] == '\n') { r++; }
        else if (in[r] == '\\' && r + 1 < end && in[r + 1] == 'n') { r += 2; }
        else if (in[r] == '\\' && r + 5 < end && in[r + 1] == 'u') {
            uint8_t tmpb[4], *tp = tmpb;
            if (!unescape_one_to(in, end, &r, &tp, 0)) return false;
            size_t nb = (size_t)(tp - tmpb);
            if (!(nb == 1 && tmpb[0] == '\n')) {
                for (size_t q = 0; q < nb; q++) *dst++ = tmpb[q];
                need_fallback_wc = true;
            }
        }
    }

    size_t count = 0;
    bool in_word = false;

    __m128i vq  = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    __m128i vcr = _mm_set1_epi8('\r');
    __m128i vsp = _mm_set1_epi8(' ');
    __m128i v8  = _mm_set1_epi8(8);
    __m128i v14 = _mm_set1_epi8(14);
    __m128i maskf0 = _mm_set1_epi8((char)0xf0);
    __m128i ve0 = _mm_set1_epi8((char)0xe0);
    __m128i vc2 = _mm_set1_epi8((char)0xc2);

    while (r + 18 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
        __m128i spc = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)),
            _mm_cmpeq_epi8(v, vcr));
        uint32_t m = (uint32_t)_mm_movemask_epi8(spc);
        _mm_storeu_si128((__m128i *)dst, v);

        __m128i wv = _mm_or_si128(_mm_cmpeq_epi8(v, vsp),
            _mm_and_si128(_mm_cmpgt_epi8(v, v8), _mm_cmpgt_epi8(v14, v)));
        uint32_t ws = (uint32_t)_mm_movemask_epi8(wv);

        if (__builtin_expect(m == 0, 1)) {
            if (__builtin_expect(_mm_movemask_epi8(v) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm_movemask_epi8(
                    _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                 _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0)));
                if (spm && has_unicode_space_or_edge(in + r, 16, spm)) need_fallback_wc = true;
            }
            uint16_t nonws = (uint16_t)(~ws);
            uint16_t prev = (uint16_t)((ws << 1) | (in_word ? 0u : 1u));
            count += (size_t)popcount16_sw((uint16_t)(nonws & prev));
            in_word = (ws & 0x8000u) == 0;
            dst += 16; r += 16;
            continue;
        }

        size_t pos = (size_t)__builtin_ctz(m);
        if (pos > 0) {
            uint32_t valid = (1u << pos) - 1u;
            if (__builtin_expect((_mm_movemask_epi8(v) & (int)valid) != 0, 0)) {
                uint32_t spm = (uint32_t)_mm_movemask_epi8(
                    _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                 _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0))) & valid;
                if (spm && has_unicode_space_or_edge(in + r, pos, spm)) need_fallback_wc = true;
            }
            uint32_t wsv = ws & valid;
            uint16_t nonws = (uint16_t)((~wsv) & valid);
            uint16_t prev = (uint16_t)((wsv << 1) | (in_word ? 0u : 1u));
            count += (size_t)popcount16_sw((uint16_t)(nonws & prev));
            in_word = ((wsv >> (pos - 1)) & 1u) == 0;
            dst += pos;
            r += pos;
        }

        uint8_t ch = in[r];
        if (__builtin_expect(ch == '\\', 1)) {
            do {
                uint8_t e = in[r + 1];
                if (__builtin_expect(e == 'n', 1)) {
                    *dst++ = '\n';
                    r += 2;
                    in_word = false;
                    continue;
                }
                uint8_t dec = JSON_ESC_BYTE[e];
                if (__builtin_expect(dec != 0, 1)) {
                    *dst++ = dec;
                    r += 2;
                    if (dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
                if (!unescape_md_slow(in, end, &r, &dst)) return false;
                need_fallback_wc = true;
                break;
            } while (r + 1 < end && in[r] == '\\');
            continue;
        }
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        if (!unescape_md_slow(in, end, &r, &dst)) return false;
        need_fallback_wc = true;
    }

    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            *rp = r + 1; *dstp = dst;
            *wc_out = need_fallback_wc
                ? (uint32_t)word_count(body_start, (size_t)(dst - body_start))
                : (uint32_t)count;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t dec = JSON_ESC_BYTE[in[r + 1]];
                if (dec != 0) {
                    *dst++ = dec; r += 2;
                    if (dec == '\n' || dec == '\t' || dec == 0x0c) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
                    continue;
                }
            }
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else if (ch == '\r') {
            if (!unescape_md_slow(in, end, &r, &dst)) return false;
            need_fallback_wc = true;
        } else {
            *dst++ = ch; r++;
            if (ch >= 0x80) {
                if (ch == 0xc2 || (ch & 0xf0) == 0xe0) need_fallback_wc = true;
                else if (!in_word) { in_word = true; count++; }
            } else if (ch == ' ' || (ch >= 9 && ch <= 13)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true; count++;
            }
        }
    }
    return false;
}

/* Fused `.md` JSON string parser:
 * 1) Unescapes front matter with CRLF->LF normalization.
 * 2) If first line != "---" -> fast-skips rest of JSON string (refused).
 * 3) If front matter closes and contains `draft: true` (or reserved dir) -> fast-skips body.
 * 4) Otherwise unescapes body with fused CRLF->LF + SIMD word_count in 1 pass. */
static bool parse_string_md(Cur *c, bool reserved_dir, Sp *out,
                            uint32_t *fm_close_out, uint32_t *body_rel_out,
                            uint32_t *wc_out, uint8_t *flags_out) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    const uint8_t *in = c->in;
    size_t r = c->pos, end = c->end;
    Pool *p = c->pool;
    uint32_t out_off = (uint32_t)p->len;
    uint8_t *out_base = p->base + out_off;
    uint8_t *dst = out_base;

    *fm_close_out = 0; *body_rel_out = 0; *wc_out = 0; *flags_out = FF_CRLF_OK;

    /* Fast check: does string start with "---" followed by \n or \r?
     * (Handle rare \u002d or unescaped quickly via line-by-line unescape.) */
    bool prev_cr = false;
    bool first_line = true;
    bool is_draft = false;
    uint8_t *line_start = dst;

    /* Decode front matter lines until closing "---" or end of string. */
    __m128i fm_vq  = _mm_set1_epi8('"');
    __m128i fm_vbs = _mm_set1_epi8('\\');
    __m128i fm_vcr = _mm_set1_epi8('\r');
    __m128i fm_vnl = _mm_set1_epi8('\n');
    while (r < end) {
        uint8_t ch = in[r];
        if (__builtin_expect(ch != '\\' && ch != '"' && ch != '\r' && ch != '\n', 1)) {
            /* Fast SSE2 speculative copy for clean runs within front matter line */
            while (r + 16 <= end) {
                __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
                __m128i h = _mm_or_si128(
                    _mm_or_si128(_mm_cmpeq_epi8(v, fm_vq), _mm_cmpeq_epi8(v, fm_vbs)),
                    _mm_or_si128(_mm_cmpeq_epi8(v, fm_vcr), _mm_cmpeq_epi8(v, fm_vnl)));
                uint32_t m = (uint32_t)_mm_movemask_epi8(h);
                _mm_storeu_si128((__m128i *)dst, v);
                if (m == 0) { dst += 16; r += 16; continue; }
                size_t k = (size_t)__builtin_ctz(m);
                dst += k; r += k;
                goto fm_run_done;
            }
            while (r < end) {
                uint8_t c2 = in[r];
                if (c2 == '\\' || c2 == '"' || c2 == '\r' || c2 == '\n') break;
                *dst++ = c2;
                r++;
            }
        fm_run_done:
            prev_cr = false;
            continue;
        }
        if (ch == '"') {
            c->pos = r + 1;
            /* Check if final line is closing "---" */
            if (!first_line && (dst - line_start) == 3
                && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-') {
                *fm_close_out = (uint32_t)(line_start - out_base);
                *body_rel_out = (uint32_t)(dst - out_base);
                *wc_out = 0;
                *flags_out = FF_CRLF_OK | FF_HAS_FM | FF_HAS_WC;
            }
            p->len = (size_t)(dst - p->base);
            *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
            return true;
        }
        uint8_t emit_c = 0;
        bool has_c = false;
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t e = in[r + 1];
                if (e == 'n') { r += 2; if (prev_cr) { prev_cr = false; continue; } emit_c = '\n'; has_c = true; }
                else if (e == 'r') { r += 2; prev_cr = true; emit_c = '\n'; has_c = true; }
                else if (e == '"') { r += 2; prev_cr = false; emit_c = '"'; has_c = true; }
                else if (e == '\\') { r += 2; prev_cr = false; emit_c = '\\'; has_c = true; }
                else if (e == 't') { r += 2; prev_cr = false; emit_c = '\t'; has_c = true; }
                else if (e == '/') { r += 2; prev_cr = false; emit_c = '/'; has_c = true; }
                else if (e == 'b') { r += 2; prev_cr = false; emit_c = 0x08; has_c = true; }
                else if (e == 'f') { r += 2; prev_cr = false; emit_c = 0x0c; has_c = true; }
                else {
                    uint8_t *d0 = dst;
                    if (!unescape_one_to(in, end, &r, &dst, 0)) return false;
                    size_t nb = (size_t)(dst - d0);
                    if (nb == 1 && *d0 == '\n') {
                        dst = d0;
                        if (prev_cr) { prev_cr = false; continue; }
                        emit_c = '\n'; has_c = true;
                    } else if (nb == 1 && *d0 == '\r') {
                        dst = d0;
                        prev_cr = true; emit_c = '\n'; has_c = true;
                    } else {
                        prev_cr = false;
                    }
                }
            } else {
                return false;
            }
        } else if (ch == '\r') {
            r++; prev_cr = true; emit_c = '\n'; has_c = true;
        } else if (ch == '\n') {
            r++; if (prev_cr) { prev_cr = false; continue; } emit_c = '\n'; has_c = true;
        } else {
            r++; prev_cr = false; emit_c = ch; has_c = true;
        }

        if (!has_c) continue;
        if (emit_c != '\n') {
            *dst++ = emit_c;
            continue;
        }
        /* Completed a line [line_start .. dst) */
        size_t llen = (size_t)(dst - line_start);
        *dst++ = '\n';
        if (first_line) {
            first_line = false;
            if (llen != 3 || line_start[0] != '-' || line_start[1] != '-' || line_start[2] != '-') {
                /* Guaranteed "no front matter": skip rest of JSON string! */
                if (!skip_json_string_tail(in, end, &r)) return false;
                c->pos = r;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                return true;
            }
            line_start = dst;
            continue;
        }
        /* Check closing "---" */
        if (llen == 3 && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-') {
            *fm_close_out = (uint32_t)(line_start - out_base);
            *body_rel_out = (uint32_t)(dst - out_base);
            *flags_out = FF_CRLF_OK | FF_HAS_FM;
            if (is_draft || reserved_dir) {
                if (!skip_json_string_tail(in, end, &r)) return false;
                c->pos = r;
                p->len = (size_t)(dst - p->base);
                *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
                return true;
            }
            uint32_t wc = 0;
            bool ok = (g_has_avx2 && g_has_popcnt)
                ? unescape_md_body_wc_avx2(in, end, &r, &dst, prev_cr, &wc)
                : (g_has_popcnt
                    ? unescape_md_body_wc_sse2_popcnt(in, end, &r, &dst, prev_cr, &wc)
                    : unescape_md_body_wc_sse2(in, end, &r, &dst, prev_cr, &wc));
            if (!ok) return false;
            c->pos = r;
            *wc_out = wc;
            *flags_out |= FF_HAS_WC;
            p->len = (size_t)(dst - p->base);
            *out = (Sp){out_off, (uint32_t)(p->len - out_off)};
            return true;
        }
        /* Detect `draft: true[ \t]*` line in front matter to skip body unescape */
        if (llen >= 11 && line_start[0] == 'd' && eq_bytes(line_start, "draft: true", 11)) {
            bool tail_ws = true;
            for (size_t q = 11; q < llen; q++) {
                if (line_start[q] != ' ' && line_start[q] != '\t') { tail_ws = false; break; }
            }
            if (tail_ws) is_draft = true;
        }
        line_start = dst;
    }
    return false;
}

static bool parse_i64(Cur *c, int64_t *out) {
    cur_ws(c);
    bool neg = false;
    if (cur_peek(c) == '-') { neg = true; c->pos++; }
    int64_t v = 0;
    bool any = false;
    const uint8_t *in = c->in;
    while (c->pos < c->end) {
        uint8_t ch = in[c->pos];
        if (ch >= '0' && ch <= '9') {
            if (v > (int64_t)9223372036854775807LL / 10) v = (int64_t)9223372036854775807LL;
            else {
                v = v * 10 + (int64_t)(ch - '0');
                if (v < 0) v = (int64_t)9223372036854775807LL;
            }
            c->pos++;
            any = true;
        } else break;
    }
    if (!any) return false;
    *out = neg ? -v : v;
    return true;
}

static bool skip_value(Cur *c) {
    cur_ws(c);
    uint8_t ch = cur_peek(c);
    switch (ch) {
        case '"': {
            c->pos++;
            return skip_json_string_tail(c->in, c->end, &c->pos);
        }
        case '{': {
            c->pos++;
            cur_ws(c);
            if (cur_peek(c) == '}') { c->pos++; return true; }
            for (;;) {
                if (!cur_byte(c, '"')) return false;
                if (!skip_json_string_tail(c->in, c->end, &c->pos)) return false;
                if (!cur_byte(c, ':')) return false;
                if (!skip_value(c)) return false;
                cur_ws(c);
                if (cur_peek(c) == ',') { c->pos++; continue; }
                if (cur_peek(c) == '}') { c->pos++; return true; }
                return false;
            }
        }
        case '[': {
            c->pos++;
            cur_ws(c);
            if (cur_peek(c) == ']') { c->pos++; return true; }
            for (;;) {
                if (!skip_value(c)) return false;
                cur_ws(c);
                if (cur_peek(c) == ',') { c->pos++; continue; }
                if (cur_peek(c) == ']') { c->pos++; return true; }
                return false;
            }
        }
        case 't': return cur_lit(c, "true", 4);
        case 'f': return cur_lit(c, "false", 5);
        case 'n': return cur_lit(c, "null", 4);
        default: {
            if (ch == '-' || (ch >= '0' && ch <= '9')) {
                int64_t v;
                return parse_i64(c, &v);
            }
            return false;
        }
    }
}

static bool parse_site(Cur *c, Site *out) {
    if (!cur_byte(c, '{')) return false;
    out->title = SP_NONE; out->base_url = SP_NONE; out->per_page = 0; out->recent = 0;
    for (;;) {
        cur_ws(c);
        if (cur_peek(c) == '}') { c->pos++; return true; }
        Sp key;
        if (!parse_string(c, &key)) return false;
        if (!cur_byte(c, ':')) return false;
        const uint8_t *kb = pool_at(c->pool, key);
        if (key.len == 5 && cmp_bytes(kb, "title", 5) == 0) {
            if (!parse_string(c, &out->title)) return false;
        } else if (key.len == 8 && cmp_bytes(kb, "base_url", 8) == 0) {
            if (!parse_string(c, &out->base_url)) return false;
        } else if (key.len == 8 && cmp_bytes(kb, "per_page", 8) == 0) {
            if (!parse_i64(c, &out->per_page)) return false;
        } else if (key.len == 6 && cmp_bytes(kb, "recent", 6) == 0) {
            if (!parse_i64(c, &out->recent)) return false;
        } else {
            if (!skip_value(c)) return false;
        }
        cur_ws(c);
        if (cur_peek(c) == ',') { c->pos++; continue; }
        if (cur_peek(c) == '}') { c->pos++; return true; }
        return false;
    }
}

bool json_read_case(Cur *c, CaseInput *out) {
    if (!cur_byte(c, '{')) return false;
    arr_new(&out->templates, sizeof(SpPair));
    arr_new(&out->files, sizeof(SpPair));
    out->site.title = SP_NONE; out->site.base_url = SP_NONE;
    out->site.per_page = 0; out->site.recent = 0;
    for (;;) {
        cur_ws(c);
        if (cur_peek(c) == '}') { c->pos++; return true; }
        Sp key;
        if (!parse_string(c, &key)) return false;
        if (!cur_byte(c, ':')) return false;
        const uint8_t *kb = pool_at(c->pool, key);
        if (key.len == 4 && cmp_bytes(kb, "site", 4) == 0) {
            if (!parse_site(c, &out->site)) return false;
        } else if (key.len == 9 && cmp_bytes(kb, "templates", 9) == 0) {
            if (!cur_byte(c, '{')) return false;
            for (;;) {
                cur_ws(c);
                if (cur_peek(c) == '}') { c->pos++; break; }
                Sp name, src;
                if (!parse_string(c, &name)) return false;
                if (!cur_byte(c, ':')) return false;
                if (!parse_string(c, &src)) return false;
                SpPair *pr = arr_slot(&out->templates, c->pool);
                pr->a = name; pr->b = src;
                cur_ws(c);
                if (cur_peek(c) == ',') { c->pos++; continue; }
                if (cur_peek(c) == '}') { c->pos++; break; }
                return false;
            }
        } else if (key.len == 5 && cmp_bytes(kb, "files", 5) == 0) {
            if (!cur_byte(c, '[')) return false;
            for (;;) {
                cur_ws(c);
                if (cur_peek(c) == ']') { c->pos++; break; }
                if (!cur_byte(c, '{')) return false;
                Sp path = SP_NONE, content = SP_NONE;
                uint32_t fm_close = 0, body_rel = 0, wc = 0;
                uint8_t flags = 0;
                bool has_path = false;
                bool is_md = false;
                bool reserved_dir = false;
                for (;;) {
                    cur_ws(c);
                    if (cur_peek(c) == '}') { c->pos++; break; }
                    int fkind = 0; /* 1=path, 2=content, 0=other */
                    if (__builtin_expect(c->pos + 6 < c->end && c->in[c->pos] == '"', 1)) {
                        if (eq_bytes(c->in + c->pos, "\"path\"", 6)) { c->pos += 6; fkind = 1; }
                        else if (c->pos + 9 < c->end && eq_bytes(c->in + c->pos, "\"content\"", 9)) { c->pos += 9; fkind = 2; }
                    }
                    if (__builtin_expect(fkind == 0, 0)) {
                        Sp fk;
                        if (!parse_string(c, &fk)) return false;
                        const uint8_t *fkb = pool_at(c->pool, fk);
                        if (fk.len == 4 && eq_bytes(fkb, "path", 4)) fkind = 1;
                        else if (fk.len == 7 && eq_bytes(fkb, "content", 7)) fkind = 2;
                    }
                    if (!cur_byte(c, ':')) return false;
                    if (fkind == 1) {
                        if (!parse_string(c, &path)) return false;
                        has_path = true;
                        const uint8_t *pb = pool_at(c->pool, path);
                        is_md = path.len >= 3 && pb[path.len - 3] == '.' && pb[path.len - 2] == 'm' && pb[path.len - 1] == 'd';
                        reserved_dir = (path.len > 5 && ((eq_bytes(pb, "tags/", 5)) || (eq_bytes(pb, "page/", 5))));
                    } else if (fkind == 2) {
                        if (has_path && is_md) {
                            if (!parse_string_md(c, reserved_dir, &content, &fm_close, &body_rel, &wc, &flags)) return false;
                        } else {
                            uint8_t has_cr = 0;
                            if (!parse_string_ex(c, &content, &has_cr)) return false;
                            flags = has_cr ? FF_HAS_CR : 0;
                        }
                    } else {
                        if (!skip_value(c)) return false;
                    }
                    cur_ws(c);
                    if (cur_peek(c) == ',') { c->pos++; continue; }
                    if (cur_peek(c) == '}') { c->pos++; break; }
                    return false;
                }
                SpPair *pr = arr_slot(&out->files, c->pool);
                pr->a = path; pr->b = content;
                pr->fm_close_off = fm_close; pr->body_rel_off = body_rel;
                pr->word_cnt = wc; pr->flags = flags;
                cur_ws(c);
                if (cur_peek(c) == ',') { c->pos++; continue; }
                if (cur_peek(c) == ']') { c->pos++; break; }
                return false;
            }
        } else {
            if (!skip_value(c)) return false;
        }
        cur_ws(c);
        if (cur_peek(c) == ',') { c->pos++; continue; }
        if (cur_peek(c) == '}') { c->pos++; return true; }
        return false;
    }
}

bool json_all_begin(Cur *c) {
    return cur_byte(c, '[');
}

int json_all_next(Cur *c, CaseInput *out) {
    cur_ws(c);
    if (cur_peek(c) == ']') { c->pos++; return 0; }
    if (!cur_byte(c, '{')) return -1;
    bool has_input = false;
    for (;;) {
        cur_ws(c);
        if (cur_peek(c) == '}') { c->pos++; break; }
        Sp ck;
        if (!parse_string(c, &ck)) return -1;
        if (!cur_byte(c, ':')) return -1;
        const uint8_t *ckb = pool_at(c->pool, ck);
        if (ck.len == 5 && cmp_bytes(ckb, "input", 5) == 0) {
            if (!json_read_case(c, out)) return -1;
            has_input = true;
        } else {
            if (!skip_value(c)) return -1;
        }
        cur_ws(c);
        if (cur_peek(c) == ',') { c->pos++; continue; }
        if (cur_peek(c) == '}') { c->pos++; break; }
        return -1;
    }
    if (!has_input) return -1;
    cur_ws(c);
    if (cur_peek(c) == ',') { c->pos++; return 1; }
    if (cur_peek(c) == ']') { return 1; }
    return -1;
}
