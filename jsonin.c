/* jsonin.c — streaming 1-pass zero-copy mmap JSON parser for the site-generator
 * input schema. For .md files, decodes JSON escapes, normalizes CRLF->LF,
 * locates front-matter closing "---", and computes body word_count in a single
 * fused AVX2/SSE2 register pass directly into the L2-resident per-case pool. */
#include "zg.h"
#include <immintrin.h>

static inline uint8_t cur_peek(Cur *c) {
    return c->pos < c->end ? c->in[c->pos] : 0;
}

static inline void cur_ws(Cur *c) {
    const uint8_t *in = c->in;
    size_t p = c->pos, e = c->end;
    while (p < e) {
        uint8_t ch = in[p];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') p++;
        else break;
    }
    c->pos = p;
}

static bool cur_byte(Cur *c, uint8_t b) {
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

static inline bool hex4_raw(const uint8_t *buf, size_t end, size_t *pos, uint32_t *out) {
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

static inline void put_utf8_ptr(uint8_t **wp, uint32_t cp) {
    uint8_t tmp[4];
    int n = u8_encode(cp, tmp);
    uint8_t *w = *wp;
    for (int k = 0; k < n; k++) *w++ = tmp[k];
    *wp = w;
}

static bool unescape_one_to(const uint8_t *raw, size_t end, size_t *rp, uint8_t **wp) {
    size_t r = *rp;
    uint8_t *w = *wp;
    r++; /* skip '\\' */
    if (r >= end) return false;
    uint8_t e = raw[r++];
    switch (e) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = 0x08; break;
        case 'f': *w++ = 0x0c; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!hex4_raw(raw, end, &r, &cp)) return false;
            if (cp >= 0xd800 && cp < 0xdc00) {
                if (r + 1 < end && raw[r] == '\\' && raw[r + 1] == 'u') {
                    r += 2;
                    uint32_t lo;
                    if (!hex4_raw(raw, end, &r, &lo)) return false;
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        put_utf8_ptr(&w, 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00));
                    } else {
                        put_utf8_ptr(&w, 0xfffd);
                        if (lo >= 0xd800 && lo < 0xe000) put_utf8_ptr(&w, 0xfffd);
                        else put_utf8_ptr(&w, lo);
                    }
                } else {
                    put_utf8_ptr(&w, 0xfffd);
                }
            } else if (cp >= 0xdc00 && cp < 0xe000) {
                put_utf8_ptr(&w, 0xfffd);
            } else {
                put_utf8_ptr(&w, cp);
            }
            break;
        }
        default: return false;
    }
    *rp = r;
    *wp = w;
    return true;
}

static inline bool consume_json_lf(const uint8_t *raw, size_t end, size_t *rp) {
    size_t r = *rp;
    if (r >= end) return false;
    if (raw[r] == '\n') { *rp = r + 1; return true; }
    if (raw[r] == '\\' && r + 1 < end) {
        if (raw[r + 1] == 'n') { *rp = r + 2; return true; }
        if (raw[r + 1] == 'u' && r + 6 <= end) {
            size_t t = r + 2;
            uint32_t cp;
            if (hex4_raw(raw, end, &t, &cp) && cp == 0x0a) { *rp = t; return true; }
        }
    }
    return false;
}

static inline unsigned popcount32_swar(uint32_t x) {
    x -= (x >> 1) & 0x55555555u;
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}

__attribute__((target("popcnt")))
static inline unsigned popcount32_hw(uint32_t x) {
    return (unsigned)__builtin_popcount(x);
}

static inline bool ws_ascii(uint8_t b) {
    return b == ' ' || (b >= 9 && b <= 13);
}

/* Fast AVX2 decode of a standard JSON string (no CRLF norm). */
__attribute__((target("avx2,bmi")))
static bool parse_string_escaped_avx2(Cur *c, uint32_t pstart, size_t r, Sp *out, uint8_t *flags_out) {
    const uint8_t *in = c->in;
    size_t end = c->end;
    uint8_t *w = c->pool->base + c->pool->len;
    uint8_t has_cr = 0;
    __m256i vq = _mm256_set1_epi8('"');
    __m256i vbs = _mm256_set1_epi8('\\');
    for (;;) {
        while (r + 32 <= end) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(in + r));
            uint32_t m = (uint32_t)_mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vbs)));
            _mm256_storeu_si256((__m256i *)w, v);
            if (m == 0) { r += 32; w += 32; continue; }
            size_t k = (size_t)__builtin_ctz(m);
            r += k; w += k;
            if (in[r] == '"') {
                c->pool->len = (size_t)(w - c->pool->base);
                *out = pool_span(c->pool, pstart);
                if (flags_out) *flags_out = has_cr;
                c->pos = r + 1;
                return true;
            }
            do {
                if (__builtin_expect(r + 1 < end, 1)) {
                    uint8_t e = in[r + 1];
                    if (e == 'n') { *w++ = '\n'; r += 2; continue; }
                    if (e == '"') { *w++ = '"'; r += 2; continue; }
                    if (e == '\\') { *w++ = '\\'; r += 2; continue; }
                    if (e == 't') { *w++ = '\t'; r += 2; continue; }
                    if (e == 'r') { *w++ = '\r'; has_cr = RF_HAS_CR; r += 2; continue; }
                }
                uint8_t *prev = w;
                if (!unescape_one_to(in, end, &r, &w)) return false;
                if (w - prev == 1 && prev[0] == '\r') has_cr = RF_HAS_CR;
            } while (r < end && in[r] == '\\');
        }
        while (r < end) {
            uint8_t ch = in[r];
            if (ch == '"') {
                c->pool->len = (size_t)(w - c->pool->base);
                *out = pool_span(c->pool, pstart);
                if (flags_out) *flags_out = has_cr;
                c->pos = r + 1;
                return true;
            }
            if (ch == '\\') {
                uint8_t *prev = w;
                if (!unescape_one_to(in, end, &r, &w)) return false;
                if (w - prev == 1 && prev[0] == '\r') has_cr = RF_HAS_CR;
            } else {
                if (ch == '\r') has_cr = RF_HAS_CR;
                *w++ = ch; r++;
            }
        }
        return false;
    }
}

/* Parse a JSON string from c->in and store decoded bytes into c->pool. */
static bool parse_string_flags(Cur *c, Sp *out, uint8_t *flags_out) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    const uint8_t *in = c->in;
    size_t start = c->pos, end = c->end;
    int j = scan_two(in + start, end - start, 0, '"', '\\');
    if (j < 0) return false;
    size_t abs = start + (size_t)j;
    if (in[abs] == '"') {
        *out = pool_store(c->pool, in + start, abs - start);
        if (flags_out) *flags_out = RF_NO_ESC;
        c->pos = abs + 1;
        return true;
    }
    uint32_t pstart = pool_mark(c->pool);
    pool_put(c->pool, in + start, abs - start);
    if (g_has_avx2 && end - abs >= 32) {
        return parse_string_escaped_avx2(c, pstart, abs, out, flags_out);
    }
    uint8_t *w = c->pool->base + c->pool->len;
    size_t r = abs;
    uint8_t has_cr = 0;
    __m128i vq = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    for (;;) {
        while (r + 16 <= end) {
            __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
            uint32_t m = (uint32_t)_mm_movemask_epi8(
                _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)));
            _mm_storeu_si128((__m128i *)w, v);
            if (m == 0) { r += 16; w += 16; continue; }
            size_t k = (size_t)__builtin_ctz(m);
            r += k; w += k;
            if (in[r] == '"') {
                c->pool->len = (size_t)(w - c->pool->base);
                *out = pool_span(c->pool, pstart);
                if (flags_out) *flags_out = has_cr;
                c->pos = r + 1;
                return true;
            }
            do {
                if (__builtin_expect(r + 1 < end, 1)) {
                    uint8_t e = in[r + 1];
                    if (e == 'n') { *w++ = '\n'; r += 2; continue; }
                    if (e == '"') { *w++ = '"'; r += 2; continue; }
                    if (e == '\\') { *w++ = '\\'; r += 2; continue; }
                    if (e == 't') { *w++ = '\t'; r += 2; continue; }
                    if (e == 'r') { *w++ = '\r'; has_cr = RF_HAS_CR; r += 2; continue; }
                }
                uint8_t *prev = w;
                if (!unescape_one_to(in, end, &r, &w)) return false;
                if (w - prev == 1 && prev[0] == '\r') has_cr = RF_HAS_CR;
            } while (r < end && in[r] == '\\');
        }
        while (r < end) {
            uint8_t ch = in[r];
            if (ch == '"') {
                c->pool->len = (size_t)(w - c->pool->base);
                *out = pool_span(c->pool, pstart);
                if (flags_out) *flags_out = has_cr;
                c->pos = r + 1;
                return true;
            }
            if (ch == '\\') {
                uint8_t *prev = w;
                if (!unescape_one_to(in, end, &r, &w)) return false;
                if (w - prev == 1 && prev[0] == '\r') has_cr = RF_HAS_CR;
            } else {
                if (ch == '\r') has_cr = RF_HAS_CR;
                *w++ = ch; r++;
            }
        }
        return false;
    }
}

static inline bool parse_string(Cur *c, Sp *out) {
    return parse_string_flags(c, out, 0);
}

/* Fused AVX2 Markdown body JSON unescape + CRLF->LF + word_count. */
__attribute__((target("avx2,bmi,popcnt")))
static bool md_body_decode_avx2(const uint8_t *in, size_t end, size_t *rp, uint8_t **wp,
                                bool count_words, size_t *wc_out, bool *wc_ok_out) {
    size_t r = *rp;
    uint8_t *w = *wp;
    size_t wc = 0;
    bool in_word = false;
    bool wc_ok = count_words;

    __m256i vq = _mm256_set1_epi8('"');
    __m256i vbs = _mm256_set1_epi8('\\');
    __m256i vcr = _mm256_set1_epi8('\r');
    __m256i vsp = _mm256_set1_epi8(' ');
    __m256i v8 = _mm256_set1_epi8(8);
    __m256i v14 = _mm256_set1_epi8(14);
    __m256i maskf0 = _mm256_set1_epi8((char)0xf0);
    __m256i ve0 = _mm256_set1_epi8((char)0xe0);
    __m256i vc2 = _mm256_set1_epi8((char)0xc2);

    while (r + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(in + r));
        __m256i hit = _mm256_or_si256(_mm256_cmpeq_epi8(v, vq),
                                      _mm256_or_si256(_mm256_cmpeq_epi8(v, vbs), _mm256_cmpeq_epi8(v, vcr)));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(hit);
        _mm256_storeu_si256((__m256i *)w, v);
        if (m == 0) {
            if (wc_ok) {
                if (__builtin_expect(_mm256_movemask_epi8(v) != 0, 0)) {
                    __m256i sp = _mm256_or_si256(_mm256_cmpeq_epi8(v, vc2),
                                                 _mm256_cmpeq_epi8(_mm256_and_si256(v, maskf0), ve0));
                    if (_mm256_movemask_epi8(sp) != 0) wc_ok = false;
                }
                if (wc_ok) {
                    __m256i wv = _mm256_or_si256(_mm256_cmpeq_epi8(v, vsp),
                        _mm256_and_si256(_mm256_cmpgt_epi8(v, v8), _mm256_cmpgt_epi8(v14, v)));
                    uint32_t ws = (uint32_t)_mm256_movemask_epi8(wv);
                    uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
                    wc += (size_t)__builtin_popcount((~ws) & prev);
                    in_word = (ws & 0x80000000u) == 0;
                }
            }
            r += 32; w += 32;
            continue;
        }
        size_t k = (size_t)__builtin_ctz(m);
        if (wc_ok && k > 0) {
            uint32_t kmask = (1u << k) - 1u;
            if (__builtin_expect(((uint32_t)_mm256_movemask_epi8(v) & kmask) != 0, 0)) {
                __m256i sp = _mm256_or_si256(_mm256_cmpeq_epi8(v, vc2),
                                             _mm256_cmpeq_epi8(_mm256_and_si256(v, maskf0), ve0));
                if (((uint32_t)_mm256_movemask_epi8(sp) & kmask) != 0) wc_ok = false;
            }
            if (wc_ok) {
                __m256i wv = _mm256_or_si256(_mm256_cmpeq_epi8(v, vsp),
                    _mm256_and_si256(_mm256_cmpgt_epi8(v, v8), _mm256_cmpgt_epi8(v14, v)));
                uint32_t ws = (uint32_t)_mm256_movemask_epi8(wv);
                uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
                wc += (size_t)__builtin_popcount((~ws) & prev & kmask);
                in_word = ((ws >> (k - 1)) & 1u) == 0;
            }
        }
        r += k; w += k;
        if (in[r] == '"') {
            *rp = r + 1; *wp = w; *wc_out = wc; *wc_ok_out = wc_ok;
            return true;
        }
        do {
            if (in[r] == '\r') {
                *w++ = '\n'; r++;
                consume_json_lf(in, end, &r);
                in_word = false;
                continue;
            }
            if (__builtin_expect(r + 1 < end, 1)) {
                uint8_t e = in[r + 1];
                if (e == 'n') { *w++ = '\n'; r += 2; in_word = false; continue; }
                if (e == '"') { *w++ = '"'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == '\\') { *w++ = '\\'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == 't') { *w++ = '\t'; r += 2; in_word = false; continue; }
                if (e == 'r') { *w++ = '\n'; r += 2; consume_json_lf(in, end, &r); in_word = false; continue; }
                if (e == '/') { *w++ = '/'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
            }
            uint8_t *prev_w = w;
            if (!unescape_one_to(in, end, &r, &w)) return false;
            if (w - prev_w == 1 && prev_w[0] == '\r') {
                prev_w[0] = '\n';
                consume_json_lf(in, end, &r);
            }
            wc_ok = false;
        } while (r < end && (in[r] == '\\' || in[r] == '\r'));
    }
    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            *rp = r + 1; *wp = w; *wc_out = wc; *wc_ok_out = wc_ok;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t e = in[r + 1];
                if (e == 'n') { *w++ = '\n'; r += 2; in_word = false; continue; }
                if (e == '"') { *w++ = '"'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == '\\') { *w++ = '\\'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == 't') { *w++ = '\t'; r += 2; in_word = false; continue; }
                if (e == 'r') { *w++ = '\n'; r += 2; consume_json_lf(in, end, &r); in_word = false; continue; }
                if (e == '/') { *w++ = '/'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
            }
            uint8_t *prev_w = w;
            if (!unescape_one_to(in, end, &r, &w)) return false;
            if (w - prev_w == 1 && prev_w[0] == '\r') {
                prev_w[0] = '\n';
                consume_json_lf(in, end, &r);
            }
            wc_ok = false;
        } else if (ch == '\r') {
            *w++ = '\n'; r++;
            consume_json_lf(in, end, &r);
            in_word = false;
        } else {
            *w++ = ch; r++;
            if (wc_ok) {
                if (ch < 0x80) {
                    if (ws_ascii(ch)) in_word = false;
                    else if (!in_word) { in_word = true; wc++; }
                } else if (ch == 0xc2 || (ch & 0xf0) == 0xe0) {
                    wc_ok = false;
                } else {
                    if (!in_word) { in_word = true; wc++; }
                }
            }
        }
    }
    return false;
}

/* Fused SSE2 Markdown body JSON unescape + CRLF->LF + word_count. */
static bool md_body_decode_sse2(const uint8_t *in, size_t end, size_t *rp, uint8_t **wp,
                                bool count_words, size_t *wc_out, bool *wc_ok_out) {
    size_t r = *rp;
    uint8_t *w = *wp;
    size_t wc = 0;
    bool in_word = false;
    bool wc_ok = count_words;

    __m128i vq = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    __m128i vcr = _mm_set1_epi8('\r');
    __m128i vsp = _mm_set1_epi8(' ');
    __m128i v8 = _mm_set1_epi8(8);
    __m128i v14 = _mm_set1_epi8(14);
    __m128i maskf0 = _mm_set1_epi8((char)0xf0);
    __m128i ve0 = _mm_set1_epi8((char)0xe0);
    __m128i vc2 = _mm_set1_epi8((char)0xc2);
    uint8_t hw_pop = g_has_popcnt;

    while (r + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(in + r));
        __m128i hit = _mm_or_si128(_mm_cmpeq_epi8(v, vq),
                                   _mm_or_si128(_mm_cmpeq_epi8(v, vbs), _mm_cmpeq_epi8(v, vcr)));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        _mm_storeu_si128((__m128i *)w, v);
        if (m == 0) {
            if (wc_ok) {
                if (__builtin_expect(_mm_movemask_epi8(v) != 0, 0)) {
                    __m128i sp = _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                              _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0));
                    if (_mm_movemask_epi8(sp) != 0) wc_ok = false;
                }
                if (wc_ok) {
                    __m128i wv = _mm_or_si128(_mm_cmpeq_epi8(v, vsp),
                        _mm_and_si128(_mm_cmpgt_epi8(v, v8), _mm_cmpgt_epi8(v14, v)));
                    uint32_t ws = (uint32_t)_mm_movemask_epi8(wv);
                    uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
                    uint32_t starts = (~ws) & prev & 0xffffu;
                    wc += (size_t)(hw_pop ? popcount32_hw(starts) : popcount32_swar(starts));
                    in_word = (ws & 0x8000u) == 0;
                }
            }
            r += 16; w += 16;
            continue;
        }
        size_t k = (size_t)__builtin_ctz(m);
        if (wc_ok && k > 0) {
            uint32_t kmask = (1u << k) - 1u;
            if (__builtin_expect(((uint32_t)_mm_movemask_epi8(v) & kmask) != 0, 0)) {
                __m128i sp = _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                          _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0));
                if (((uint32_t)_mm_movemask_epi8(sp) & kmask) != 0) wc_ok = false;
            }
            if (wc_ok) {
                __m128i wv = _mm_or_si128(_mm_cmpeq_epi8(v, vsp),
                    _mm_and_si128(_mm_cmpgt_epi8(v, v8), _mm_cmpgt_epi8(v14, v)));
                uint32_t ws = (uint32_t)_mm_movemask_epi8(wv);
                uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
                uint32_t starts = (~ws) & prev & kmask;
                wc += (size_t)(hw_pop ? popcount32_hw(starts) : popcount32_swar(starts));
                in_word = ((ws >> (k - 1)) & 1u) == 0;
            }
        }
        r += k; w += k;
        if (in[r] == '"') {
            *rp = r + 1; *wp = w; *wc_out = wc; *wc_ok_out = wc_ok;
            return true;
        }
        do {
            if (in[r] == '\r') {
                *w++ = '\n'; r++;
                consume_json_lf(in, end, &r);
                in_word = false;
                continue;
            }
            if (__builtin_expect(r + 1 < end, 1)) {
                uint8_t e = in[r + 1];
                if (e == 'n') { *w++ = '\n'; r += 2; in_word = false; continue; }
                if (e == '"') { *w++ = '"'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == '\\') { *w++ = '\\'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == 't') { *w++ = '\t'; r += 2; in_word = false; continue; }
                if (e == 'r') { *w++ = '\n'; r += 2; consume_json_lf(in, end, &r); in_word = false; continue; }
                if (e == '/') { *w++ = '/'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
            }
            uint8_t *prev_w = w;
            if (!unescape_one_to(in, end, &r, &w)) return false;
            if (w - prev_w == 1 && prev_w[0] == '\r') {
                prev_w[0] = '\n';
                consume_json_lf(in, end, &r);
            }
            wc_ok = false;
        } while (r < end && (in[r] == '\\' || in[r] == '\r'));
    }
    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            *rp = r + 1; *wp = w; *wc_out = wc; *wc_ok_out = wc_ok;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t e = in[r + 1];
                if (e == 'n') { *w++ = '\n'; r += 2; in_word = false; continue; }
                if (e == '"') { *w++ = '"'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == '\\') { *w++ = '\\'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
                if (e == 't') { *w++ = '\t'; r += 2; in_word = false; continue; }
                if (e == 'r') { *w++ = '\n'; r += 2; consume_json_lf(in, end, &r); in_word = false; continue; }
                if (e == '/') { *w++ = '/'; r += 2; if (!in_word) { in_word = true; wc++; } continue; }
            }
            uint8_t *prev_w = w;
            if (!unescape_one_to(in, end, &r, &w)) return false;
            if (w - prev_w == 1 && prev_w[0] == '\r') {
                prev_w[0] = '\n';
                consume_json_lf(in, end, &r);
            }
            wc_ok = false;
        } else if (ch == '\r') {
            *w++ = '\n'; r++;
            consume_json_lf(in, end, &r);
            in_word = false;
        } else {
            *w++ = ch; r++;
            if (wc_ok) {
                if (ch < 0x80) {
                    if (ws_ascii(ch)) in_word = false;
                    else if (!in_word) { in_word = true; wc++; }
                } else if (ch == 0xc2 || (ch & 0xf0) == 0xe0) {
                    wc_ok = false;
                } else {
                    if (!in_word) { in_word = true; wc++; }
                }
            }
        }
    }
    return false;
}

/* Parse a .md file's "content" string in 1 pass: decode JSON escapes +
 * normalize CRLF->LF + locate closing "---" + compute body word_count. */
static bool parse_string_md(Cur *c, RawFile *rf) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    const uint8_t *in = c->in;
    size_t r = c->pos, end = c->end;
    uint32_t pstart = pool_mark(c->pool);
    uint8_t *base = c->pool->base + pstart;
    uint8_t *w = base;
    uint8_t *line_start = base;
    size_t line_no = 1;
    bool has_fm = false;
    uint32_t fm_close_off = 0xffffffffu;
    uint32_t body_rel_off = 0;

    /* Phase 1: decode front matter lines until closing "---" or non-FM line 1. */
    while (r < end) {
        uint8_t ch = in[r];
        if (ch == '"') {
            r++;
            if (line_no > 1 && (size_t)(w - line_start) == 3
                && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-') {
                has_fm = true;
                fm_close_off = (uint32_t)(line_start - base);
                body_rel_off = (uint32_t)(w - base);
            }
            c->pool->len = (size_t)(w - c->pool->base);
            rf->content = pool_span(c->pool, pstart);
            rf->fm_close_off = fm_close_off;
            rf->body_rel_off = body_rel_off;
            rf->word_cnt = 0;
            rf->flags = (uint8_t)(RF_CRLF_NORM | (has_fm ? (RF_HAS_FM | RF_HAS_WC) : 0));
            c->pos = r;
            return true;
        }
        if (ch == '\\') {
            if (r + 1 < end) {
                uint8_t e = in[r + 1];
                if (e == 'n') {
                    r += 2;
                    size_t llen = (size_t)(w - line_start);
                    bool is_sep = (llen == 3 && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-');
                    *w++ = '\n';
                    if (line_no == 1) {
                        if (!is_sep) break;
                    } else if (is_sep) {
                        has_fm = true;
                        fm_close_off = (uint32_t)(line_start - base);
                        body_rel_off = (uint32_t)(w - base);
                        break;
                    }
                    line_start = w;
                    line_no++;
                    continue;
                }
                if (e == 'r') {
                    r += 2;
                    consume_json_lf(in, end, &r);
                    size_t llen = (size_t)(w - line_start);
                    bool is_sep = (llen == 3 && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-');
                    *w++ = '\n';
                    if (line_no == 1) {
                        if (!is_sep) break;
                    } else if (is_sep) {
                        has_fm = true;
                        fm_close_off = (uint32_t)(line_start - base);
                        body_rel_off = (uint32_t)(w - base);
                        break;
                    }
                    line_start = w;
                    line_no++;
                    continue;
                }
                if (e == '"') { *w++ = '"'; r += 2; continue; }
                if (e == '\\') { *w++ = '\\'; r += 2; continue; }
                if (e == 't') { *w++ = '\t'; r += 2; continue; }
            }
            uint8_t *prev_w = w;
            if (!unescape_one_to(in, end, &r, &w)) return false;
            if (w - prev_w == 1 && (prev_w[0] == '\n' || prev_w[0] == '\r')) {
                if (prev_w[0] == '\r') {
                    prev_w[0] = '\n';
                    consume_json_lf(in, end, &r);
                }
                size_t llen = (size_t)(prev_w - line_start);
                bool is_sep = (llen == 3 && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-');
                if (line_no == 1) {
                    if (!is_sep) break;
                } else if (is_sep) {
                    has_fm = true;
                    fm_close_off = (uint32_t)(line_start - base);
                    body_rel_off = (uint32_t)(w - base);
                    break;
                }
                line_start = w;
                line_no++;
            }
        } else if (ch == '\n' || ch == '\r') {
            r++;
            if (ch == '\r') consume_json_lf(in, end, &r);
            size_t llen = (size_t)(w - line_start);
            bool is_sep = (llen == 3 && line_start[0] == '-' && line_start[1] == '-' && line_start[2] == '-');
            *w++ = '\n';
            if (line_no == 1) {
                if (!is_sep) break;
            } else if (is_sep) {
                has_fm = true;
                fm_close_off = (uint32_t)(line_start - base);
                body_rel_off = (uint32_t)(w - base);
                break;
            }
            line_start = w;
            line_no++;
        } else {
            *w++ = ch;
            r++;
            if (line_no == 1 && (size_t)(w - line_start) > 3) break;
        }
    }

    /* Phase 2: fast SIMD body decode (+ fused word_count when has_fm). */
    size_t wc = 0;
    bool wc_ok = false;
    bool ok = (g_has_avx2 && g_has_popcnt)
        ? md_body_decode_avx2(in, end, &r, &w, has_fm, &wc, &wc_ok)
        : md_body_decode_sse2(in, end, &r, &w, has_fm, &wc, &wc_ok);
    if (!ok) return false;
    c->pool->len = (size_t)(w - c->pool->base);
    rf->content = pool_span(c->pool, pstart);
    rf->fm_close_off = fm_close_off;
    rf->body_rel_off = body_rel_off;
    rf->word_cnt = (uint32_t)wc;
    rf->flags = (uint8_t)(RF_CRLF_NORM | (has_fm ? RF_HAS_FM : 0) | (wc_ok ? RF_HAS_WC : 0));
    c->pos = r;
    return true;
}

static bool skip_json_string(Cur *c) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    const uint8_t *in = c->in;
    size_t r = c->pos, end = c->end;
    while (r < end) {
        int j = scan_two(in, end, r, '"', '\\');
        if (j < 0) return false;
        r = (size_t)j;
        if (in[r] == '"') { c->pos = r + 1; return true; }
        if (r + 1 >= end) return false;
        r += 2;
    }
    return false;
}

static bool parse_i64(Cur *c, int64_t *out) {
    cur_ws(c);
    bool neg = false;
    if (cur_peek(c) == '-') { neg = true; c->pos++; }
    int64_t v = 0;
    bool any = false;
    while (c->pos < c->end) {
        uint8_t ch = c->in[c->pos];
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
        case '"': return skip_json_string(c);
        case '{': {
            c->pos++;
            cur_ws(c);
            if (cur_peek(c) == '}') { c->pos++; return true; }
            for (;;) {
                if (!skip_json_string(c)) return false;
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
    arr_new(&out->files, sizeof(RawFile));
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
                RawFile rf;
                rf.path = SP_NONE; rf.content = SP_NONE;
                rf.fm_close_off = 0xffffffffu; rf.body_rel_off = 0;
                rf.word_cnt = 0; rf.flags = 0;
                bool has_path = false;
                bool path_md = false;
                for (;;) {
                    cur_ws(c);
                    if (cur_peek(c) == '}') { c->pos++; break; }
                    Sp fk;
                    if (!parse_string(c, &fk)) return false;
                    if (!cur_byte(c, ':')) return false;
                    const uint8_t *fkb = pool_at(c->pool, fk);
                    if (fk.len == 4 && cmp_bytes(fkb, "path", 4) == 0) {
                        if (!parse_string(c, &rf.path)) return false;
                        has_path = true;
                        const uint8_t *pb = pool_at(c->pool, rf.path);
                        path_md = rf.path.len >= 3 && pb[rf.path.len - 3] == '.'
                            && pb[rf.path.len - 2] == 'm' && pb[rf.path.len - 1] == 'd';
                    } else if (fk.len == 7 && cmp_bytes(fkb, "content", 7) == 0) {
                        if (has_path && path_md) {
                            if (!parse_string_md(c, &rf)) return false;
                        } else {
                            uint8_t fl = 0;
                            if (!parse_string_flags(c, &rf.content, &fl)) return false;
                            rf.flags = fl;
                        }
                    } else {
                        if (!skip_value(c)) return false;
                    }
                    cur_ws(c);
                    if (cur_peek(c) == ',') { c->pos++; continue; }
                    if (cur_peek(c) == '}') { c->pos++; break; }
                    return false;
                }
                RawFile *slot = arr_slot(&out->files, c->pool);
                *slot = rf;
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
    if (cur_peek(c) == ']') return 1;
    return -1;
}
