/* md.c — high-speed Markdown subset renderer with speculative-store SIMD
 * (AVX2 / SSSE3 / pure SSE2), zero-recursion inline fast paths, out-of-line
 * cold markup fallbacks (keeping hot loops L1i-tiny), and bulk code escaping. */
#include "zg.h"
#include <immintrin.h>

static const uint8_t TBL_INL[256] = {
    [0 ... 0x1f] = 1,
    ['\\'] = 1, ['`'] = 1, ['['] = 1, ['*'] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};

static const uint8_t TBL_HTML_JSON[256] = {
    [0 ... 0x1f] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
};

static const uint8_t TBL_CODE_FAST[256] = {
    [0 ... 0x1f] = 1,
    ['`'] = 1, ['\\'] = 1, ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};

static const uint8_t TBL_LTXT_FAST[256] = {
    [0 ... 0x1f] = 1,
    ['\\'] = 1, ['`'] = 1, ['['] = 1, [']'] = 1, ['*'] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};

static const uint8_t TBL_LURL_FAST[256] = {
    [0 ... 0x20] = 1,
    [')'] = 1, ['\\'] = 1, ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1,
    [0x80 ... 0xff] = 1
};

static const uint8_t TBL_ESCAPABLE[256] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['_'] = 1, ['['] = 1, [']'] = 1,
    ['('] = 1, [')'] = 1, ['#'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1,
    ['!'] = 1, ['>'] = 1
};

static inline bool is_escapable(uint8_t c) { return TBL_ESCAPABLE[c] != 0; }

static inline bool line_blank(const uint8_t *b, size_t off, size_t end) {
    for (size_t i = off; i < end; i++) {
        uint8_t c = b[i];
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

static inline void line_trim(const uint8_t *b, size_t off, size_t end, size_t *s, size_t *e) {
    size_t a = off, z = end;
    while (a < z && (b[a] == ' ' || b[a] == '\t')) a++;
    while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t')) z--;
    *s = a; *e = z;
}

static bool fence_open_at(const uint8_t *b, size_t off, size_t end, size_t *loff, size_t *llen) {
    if (end - off < 3 || b[off] != '`' || b[off + 1] != '`' || b[off + 2] != '`') return false;
    size_t i = off + 3;
    while (i < end) {
        uint8_t c = b[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '_' || c == '+' || c == '-') i++;
        else break;
    }
    for (size_t k = i; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    *loff = off + 3; *llen = i - (off + 3);
    return true;
}

static bool fence_close_at(const uint8_t *b, size_t off, size_t end) {
    if (end - off < 3 || b[off] != '`' || b[off + 1] != '`' || b[off + 2] != '`') return false;
    for (size_t k = off + 3; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    return true;
}

static bool heading_at(const uint8_t *b, size_t off, size_t end, size_t *depth, size_t *toff, size_t *tlen) {
    if (off >= end || b[off] != '#') return false;
    size_t n = 1;
    while (off + n < end && b[off + n] == '#' && n < 6) n++;
    size_t i = off + n;
    if (i >= end || (b[i] != ' ' && b[i] != '\t')) return false;
    while (i < end && (b[i] == ' ' || b[i] == '\t')) i++;
    size_t e = end;
    while (e > i && (b[e - 1] == ' ' || b[e - 1] == '\t')) e--;
    if (e <= i) return false;
    *depth = n; *toff = i; *tlen = e - i;
    return true;
}

static bool hr_at(const uint8_t *b, size_t off, size_t end) {
    if (end - off < 3) return false;
    uint8_t c = b[off];
    if ((c != '-' && c != '*') || b[off + 1] != c || b[off + 2] != c) return false;
    for (size_t k = off + 3; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    return true;
}

static inline bool para_ends(const uint8_t *b, size_t off, size_t end) {
    if (off >= end) return true;
    uint8_t c0 = b[off];
    if (__builtin_expect(c0 > '`', 1)) return false;
    if (c0 != ' ' && c0 != '\t' && c0 != '`' && c0 != '#' && c0 != '-' && c0 != '*') return false;
    size_t d, t, l;
    if (c0 == ' ' || c0 == '\t') return line_blank(b, off, end);
    if (c0 == '`') return fence_open_at(b, off, end, &t, &l);
    if (c0 == '#') return heading_at(b, off, end, &d, &t, &l);
    return hr_at(b, off, end);
}

static bool item_start_span(const uint8_t *b, size_t off, size_t end, bool *ordered, int64_t *num, size_t *ts_out, size_t *te_out) {
    if (off >= end) return false;
    uint8_t c0 = b[off];
    if ((c0 == '-' || c0 == '*') && off + 1 < end && b[off + 1] == ' ') {
        line_trim(b, off + 2, end, ts_out, te_out);
        *ordered = false; *num = 1;
        return true;
    }
    if (c0 < '0' || c0 > '9') return false;
    size_t i = off, digits = 0;
    int64_t n = 0;
    while (i < end && b[i] >= '0' && b[i] <= '9' && digits < 9) {
        n = n * 10 + (b[i] - '0');
        i++; digits++;
    }
    if (i + 1 >= end || b[i] != '.' || b[i + 1] != ' ') return false;
    line_trim(b, i + 2, end, ts_out, te_out);
    *ordered = true; *num = n;
    return true;
}

static bool continued_at(const uint8_t *b, size_t off, size_t end) {
    size_t i = off, spaces = 0;
    while (i < end && b[i] == ' ') { spaces++; i++; }
    if (spaces < 2 || i >= end) return false;
    uint8_t c = b[i];
    return !(c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r');
}

typedef struct {
    Sp small[16];
    size_t nsmall;
    bool use_set;
    Set set;
} SmallIdSet;

static inline void small_id_init(SmallIdSet *s) { s->nsmall = 0; s->use_set = false; }

static Sp small_id_unique(Pool *p, Sp slug, SmallIdSet *s) {
    if (__builtin_expect(!s->use_set, 1)) {
        const uint8_t *sb = pool_at(p, slug);
        bool dup = false;
        for (size_t k = 0; k < s->nsmall; k++) {
            if (s->small[k].len == slug.len && eq_bytes(pool_at(p, s->small[k]), sb, slug.len)) { dup = true; break; }
        }
        if (!dup) {
            if (s->nsmall < 16) { s->small[s->nsmall++] = slug; return slug; }
            set_new(&s->set);
            for (size_t k = 0; k < s->nsmall; k++) set_add(&s->set, p, s->small[k]);
            s->use_set = true;
            set_add(&s->set, p, slug);
            return slug;
        }
        uint8_t stack[64];
        size_t slen = slug.len;
        copy_bytes(stack, sb, slen);
        uint64_t n = 2;
        for (;;) {
            uint32_t start = pool_mark(p);
            pool_put(p, stack, slen);
            pool_u8(p, '-');
            pool_u64(p, n);
            Sp cand = pool_span(p, start);
            n++;
            const uint8_t *cb = pool_at(p, cand);
            bool cdup = false;
            for (size_t k = 0; k < s->nsmall; k++) {
                if (s->small[k].len == cand.len && eq_bytes(pool_at(p, s->small[k]), cb, cand.len)) { cdup = true; break; }
            }
            if (!cdup) {
                if (s->nsmall < 16) { s->small[s->nsmall++] = cand; return cand; }
                set_new(&s->set);
                for (size_t k = 0; k < s->nsmall; k++) set_add(&s->set, p, s->small[k]);
                s->use_set = true;
                set_add(&s->set, p, cand);
                return cand;
            }
        }
    }
    return slug_unique(p, slug, &s->set);
}

/* ---------------- scan helpers ---------------- */

static inline int scan_byte_sse2_i(const uint8_t *b, size_t n, size_t i, uint8_t c) {
    if (i + 16 <= n) {
        __m128i vc = _mm_set1_epi8((char)c);
        do {
            __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
            uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
            if (m) return (int)(i + (size_t)__builtin_ctz(m));
            i += 16;
        } while (i + 16 <= n);
    }
    while (i < n) { if (b[i] == c) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static inline int scan_byte_avx2_i(const uint8_t *b, size_t n, size_t i, uint8_t c) {
    if (i + 32 <= n) {
        __m256i vc = _mm256_set1_epi8((char)c);
        do {
            __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
            uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vc));
            if (m) return (int)(i + (size_t)__builtin_ctz(m));
            i += 32;
        } while (i + 32 <= n);
    }
    if (i + 16 <= n) {
        __m128i vc = _mm_set1_epi8((char)c);
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c) return (int)i; i++; }
    return -1;
}

static int find_unesc_gen(const uint8_t *s, size_t n, uint8_t w0, uint8_t w1, size_t wlen, size_t pos) {
    for (;;) {
        int j = scan_two(s, n, pos, '\\', w0);
        if (j < 0) return -1;
        if (s[j] == '\\') {
            if ((size_t)j + 1 < n && is_escapable(s[j + 1])) pos = (size_t)j + 2;
            else pos = (size_t)j + 1;
            continue;
        }
        if (wlen == 1) return j;
        if ((size_t)j + 1 < n && s[j + 1] == w1) return j;
        pos = (size_t)j + 1;
    }
}

static int find_lone_star_gen(const uint8_t *s, size_t n, size_t pos) {
    for (;;) {
        int j = scan_two(s, n, pos, '\\', '*');
        if (j < 0) return -1;
        if (s[j] == '\\') {
            if ((size_t)j + 1 < n && is_escapable(s[j + 1])) pos = (size_t)j + 2;
            else pos = (size_t)j + 1;
            continue;
        }
        if ((size_t)j + 1 < n && s[j + 1] == '*') { pos = (size_t)j + 2; continue; }
        return j;
    }
}

/* ---------------- escaping ---------------- */

static inline uint8_t *emit_html_json_byte(uint8_t *dst, uint8_t c) {
    static const char HEX[] = "0123456789abcdef";
    switch (c) {
        case '&': __builtin_memcpy(dst, "&amp;", 5); return dst + 5;
        case '<': __builtin_memcpy(dst, "&lt;", 4); return dst + 4;
        case '>': __builtin_memcpy(dst, "&gt;", 4); return dst + 4;
        case '"': __builtin_memcpy(dst, "&quot;", 6); return dst + 6;
        case '\'': __builtin_memcpy(dst, "&#39;", 5); return dst + 5;
        case '\\': dst[0] = '\\'; dst[1] = '\\'; return dst + 2;
        case '\n': dst[0] = '\\'; dst[1] = 'n'; return dst + 2;
        case '\r': dst[0] = '\\'; dst[1] = 'r'; return dst + 2;
        case '\t': dst[0] = '\\'; dst[1] = 't'; return dst + 2;
        case 0x08: dst[0] = '\\'; dst[1] = 'b'; return dst + 2;
        case 0x0c: dst[0] = '\\'; dst[1] = 'f'; return dst + 2;
        default:
            __builtin_memcpy(dst, "\\u00", 4);
            dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
            dst[5] = (uint8_t)HEX[c & 0xf];
            return dst + 6;
    }
}

static void esc_html_into(const uint8_t *b, size_t n, Str *out) {
    size_t i = 0;
    while (i < n) {
        int j = scan_html_esc(b, n, i);
        if (j < 0) { str_add(out, b + i, n - i); return; }
        str_add(out, b + i, (size_t)j - i);
        switch (b[j]) {
            case '&': str_lit(out, "&amp;"); break;
            case '<': str_lit(out, "&lt;"); break;
            case '>': str_lit(out, "&gt;"); break;
            case '"': str_lit(out, "&quot;"); break;
            default: str_lit(out, "&#39;"); break;
        }
        i = (size_t)j + 1;
    }
}

__attribute__((target("avx2,bmi")))
static void esc_html_json_into_str_avx2(const uint8_t *b, size_t n, Str *out) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    __m256i lo_lut = _mm256_setr_epi8(
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8,
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
    __m256i hi_lut = _mm256_setr_epi8(
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t i = 0;
    while (i + 32 <= n) {
        if (__builtin_expect(dst + 64 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - i) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), zero));
        _mm256_storeu_si256((__m256i *)dst, v);
        if (m == 0) { dst += 32; i += 32; continue; }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        dst = emit_html_json_byte(dst, b[i++]);
    }
    if (__builtin_expect(dst + (n - i) * 6 + 16 > dst_end, 0)) {
        out->len = (size_t)(dst - out->data);
        str_grow(out, out->len + (n - i) * 6 + 64);
        dst = out->data + out->len;
    }
    while (i < n) {
        uint8_t c = b[i++];
        if (!TBL_HTML_JSON[c]) { *dst++ = c; continue; }
        dst = emit_html_json_byte(dst, c);
    }
    out->len = (size_t)(dst - out->data);
}

__attribute__((target("ssse3")))
static void esc_html_json_into_str_ssse3(const uint8_t *b, size_t n, Str *out) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    __m128i lo_lut = _mm_setr_epi8(8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
    __m128i hi_lut = _mm_setr_epi8(8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m128i mask0f = _mm_set1_epi8(0x0f);
    __m128i zero = _mm_setzero_si128();
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t i = 0;
    while (i + 16 <= n) {
        if (__builtin_expect(dst + 48 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - i) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_shuffle_epi8(lo_lut, v);
        __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), zero))) ^ 0xffffu;
        _mm_storeu_si128((__m128i *)dst, v);
        if (m == 0) { dst += 16; i += 16; continue; }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        dst = emit_html_json_byte(dst, b[i++]);
    }
    if (__builtin_expect(dst + (n - i) * 6 + 16 > dst_end, 0)) {
        out->len = (size_t)(dst - out->data);
        str_grow(out, out->len + (n - i) * 6 + 64);
        dst = out->data + out->len;
    }
    while (i < n) {
        uint8_t c = b[i++];
        if (!TBL_HTML_JSON[c]) { *dst++ = c; continue; }
        dst = emit_html_json_byte(dst, c);
    }
    out->len = (size_t)(dst - out->data);
}

static void esc_html_json_into_str_sse2(const uint8_t *b, size_t n, Str *out) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    __m128i zero = _mm_setzero_si128();
    __m128i v60 = _mm_set1_epi8(0x60);
    __m128i v20 = _mm_set1_epi8(0x20);
    __m128i v41 = _mm_set1_epi8(0x41);
    __m128i vm100 = _mm_set1_epi8((char)-100);
    __m128i v55 = _mm_set1_epi8(0x55);
    __m128i vm111 = _mm_set1_epi8((char)-111);
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t i = 0;
    while (i + 16 <= n) {
        if (__builtin_expect(dst + 48 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - i) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i safe = _mm_or_si128(
            _mm_or_si128(_mm_cmpgt_epi8(v, v60), _mm_cmpgt_epi8(zero, v)),
            _mm_or_si128(_mm_cmpeq_epi8(v, v20),
                _mm_or_si128(_mm_cmpgt_epi8(vm100, _mm_add_epi8(v, v41)),
                             _mm_cmpgt_epi8(vm111, _mm_add_epi8(v, v55)))));
        uint32_t m = ((uint32_t)_mm_movemask_epi8(safe)) ^ 0xffffu;
        _mm_storeu_si128((__m128i *)dst, v);
        if (m == 0) { dst += 16; i += 16; continue; }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        uint8_t c = b[i++];
        if (!TBL_HTML_JSON[c]) { *dst++ = c; continue; }
        dst = emit_html_json_byte(dst, c);
    }
    if (__builtin_expect(dst + (n - i) * 6 + 16 > dst_end, 0)) {
        out->len = (size_t)(dst - out->data);
        str_grow(out, out->len + (n - i) * 6 + 64);
        dst = out->data + out->len;
    }
    while (i < n) {
        uint8_t c = b[i++];
        if (!TBL_HTML_JSON[c]) { *dst++ = c; continue; }
        dst = emit_html_json_byte(dst, c);
    }
    out->len = (size_t)(dst - out->data);
}

static void esc_html_json_into_str(const uint8_t *b, size_t n, Str *out) {
    if (n == 0) return;
    if (g_has_avx2) esc_html_json_into_str_avx2(b, n, out);
    else if (g_has_ssse3) esc_html_json_into_str_ssse3(b, n, out);
    else esc_html_json_into_str_sse2(b, n, out);
}

/* ---------------- links ---------------- */

static bool has_ws_span(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            if (c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r') return true;
            i++;
        } else {
            uint32_t cp; size_t m = (size_t)u8_decode(s, n, i, &cp);
            if (u_is_space(cp)) return true;
            i += m;
        }
    }
    return false;
}

static bool is_scheme_span(const uint8_t *t, size_t n) {
    if (n == 0) return false;
    uint8_t c0 = t[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
    size_t i = 1;
    while (i < n) {
        uint8_t c = t[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '+' || c == '-' || c == '.') i++;
        else break;
    }
    return i < n && t[i] == ':';
}

static inline int page_lookup(LinkCtx *env, Pool *p, const uint8_t *path, size_t n) {
    Sp hit;
    if (dict_get(env->page_map, p, path, n, &hit)) return (int)hit.off;
    return -1;
}

static inline bool static_lookup(LinkCtx *env, Pool *p, const uint8_t *path, size_t n) {
    if (env->nstatics == 0) return false;
    return set_has(env->static_set, p, path, n);
}

static bool resolve_rel_buf(LinkCtx *env, const uint8_t *rel, size_t rn,
                            uint8_t *sbuf, size_t scap, Str *fallback,
                            const uint8_t **out_ptr, size_t *out_len) {
    const uint8_t *parts[64];
    size_t plens[64];
    size_t np = env->dir_np;
    for (size_t k = 0; k < np; k++) { parts[k] = env->dir_parts[k]; plens[k] = env->dir_plens[k]; }
    size_t s = 0;
    for (size_t i = 0; i <= rn; i++) {
        if (i == rn || rel[i] == '/') {
            size_t sl = i - s;
            const uint8_t *seg = rel + s;
            s = i + 1;
            if (sl == 0 || (sl == 1 && seg[0] == '.')) continue;
            if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
                if (np == 0) return false;
                np--;
            } else {
                if (np < 64) { parts[np] = seg; plens[np] = sl; np++; }
                else return false;
            }
        }
    }
    size_t total = np > 1 ? np - 1 : 0;
    for (size_t k = 0; k < np; k++) total += plens[k];
    if (__builtin_expect(total <= scap, 1)) {
        uint8_t *d = sbuf;
        for (size_t k = 0; k < np; k++) {
            if (k > 0) *d++ = '/';
            copy_fast(d, parts[k], plens[k]);
            d += plens[k];
        }
        *out_ptr = sbuf;
        *out_len = total;
        return true;
    }
    str_clear(fallback);
    for (size_t k = 0; k < np; k++) {
        if (k > 0) str_c(fallback, '/');
        str_add(fallback, parts[k], plens[k]);
    }
    *out_ptr = fallback->data;
    *out_len = fallback->len;
    return true;
}

static void note_broken(Pool *p, LinkCtx *env, const uint8_t *target, size_t tn) {
    uint32_t st = pool_mark(p);
    pool_put(p, (const uint8_t *)"broken-link ", 12);
    pool_copy(p, env->src_path);
    pool_put(p, (const uint8_t *)" -> ", 4);
    pool_put(p, target, tn);
    Sp line = pool_span(p, st);
    Sp *slot = arr_slot(env->broken, p);
    *slot = line;
}

static const uint8_t *resolve_href_fast(Pool *p, LinkCtx *env, const uint8_t *target, size_t tn,
                                        bool target_clean, Str *tmp, size_t *href_len, bool *href_clean) {
    if ((tn > 0 && (target[0] == '/' || target[0] == '#')) || is_scheme_span(target, tn)) {
        *href_len = tn;
        *href_clean = target_clean;
        return target;
    }
    size_t hash = tn;
    for (size_t i = 0; i < tn; i++) if (target[i] == '#') { hash = i; break; }
    size_t rl = hash, fl = tn - hash;
    const uint8_t *frag = target + hash;
    if (rl == 0) {
        note_broken(p, env, target, tn);
        *href_len = tn;
        *href_clean = target_clean;
        return target;
    }
    uint8_t sbuf[512];
    const uint8_t *rpath;
    size_t rlen;
    bool ok = resolve_rel_buf(env, target, rl, sbuf, sizeof(sbuf), env->path_buf, &rpath, &rlen);
    if (ok) {
        int idx = page_lookup(env, p, rpath, rlen);
        if (idx >= 0) {
            Page *pg = &env->pages[idx];
            if (fl == 0) {
                *href_len = pg->url.len;
                *href_clean = (pg->clean_mask & 2) != 0;
                return pool_at(p, pg->url);
            }
            str_clear(tmp);
            str_add(tmp, pool_at(p, pg->url), pg->url.len);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            *href_clean = target_clean && ((pg->clean_mask & 2) != 0);
            return tmp->data;
        }
        if (static_lookup(env, p, rpath, rlen)) {
            str_clear(tmp);
            str_add(tmp, pool_at(p, env->base), env->base.len);
            str_c(tmp, '/');
            str_add(tmp, rpath, rlen);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            *href_clean = false;
            return tmp->data;
        }
    }
    note_broken(p, env, target, tn);
    *href_len = tn;
    *href_clean = target_clean;
    return target;
}

/* ---------------- inline engines ---------------- */

static void inline_span_json(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp);

/* Cold fallback for nested/complex markup (*, `, [, \). Keeps hot SIMD loops tiny! */
__attribute__((noinline))
static size_t handle_markup_slow(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, size_t j, Str *out, Str *tmp) {
    uint8_t c = s[j];
    switch (c) {
        case '*': {
            if (j + 1 < n && s[j + 1] == '*') {
                int k = find_unesc_gen(s, n, '*', '*', 2, j + 2);
                if (k > (int)(j + 2)) {
                    str_lit(out, "<strong>");
                    inline_span_json(p, env, s + j + 2, (size_t)k - (j + 2), out, tmp);
                    str_lit(out, "</strong>");
                    return (size_t)k + 2;
                }
                str_lit(out, "**");
                return j + 2;
            }
            int k = find_lone_star_gen(s, n, j + 1);
            if (k > (int)(j + 1)) {
                str_lit(out, "<em>");
                inline_span_json(p, env, s + j + 1, (size_t)k - (j + 1), out, tmp);
                str_lit(out, "</em>");
                return (size_t)k + 1;
            }
            str_c(out, '*');
            return j + 1;
        }
        case '`': {
            int k = scan_byte(s, n, j + 1, '`');
            if (k >= 0 && (size_t)k > j + 1) {
                str_lit(out, "<code>");
                esc_html_json_into_str(s + j + 1, (size_t)k - (j + 1), out);
                str_lit(out, "</code>");
                return (size_t)k + 1;
            }
            str_c(out, '`');
            return j + 1;
        }
        case '[': {
            int k = find_unesc_gen(s, n, ']', 0, 1, j + 1);
            int m2 = -1;
            if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                int q2 = scan_byte(s, n, (size_t)k + 2, ')');
                if (q2 >= 0) m2 = q2;
            }
            size_t tl = 0;
            const uint8_t *target = (const uint8_t *)"";
            if (m2 != -1) { target = s + (size_t)k + 2; tl = (size_t)m2 - ((size_t)k + 2); }
            if (tl > 0 && !has_ws_span(target, tl)) {
                size_t href_len; bool hclean;
                const uint8_t *href = resolve_href_fast(p, env, target, tl, false, tmp, &href_len, &hclean);
                str_lit(out, "<a href=\\\"");
                if (hclean) str_add(out, href, href_len);
                else esc_html_json_into_str(href, href_len, out);
                str_lit(out, "\\\">");
                inline_span_json(p, env, s + j + 1, (size_t)k - (j + 1), out, tmp);
                str_lit(out, "</a>");
                return (size_t)m2 + 1;
            }
            str_c(out, '[');
            return j + 1;
        }
        default: /* '\\' */ {
            if (j + 1 < n && is_escapable(s[j + 1])) {
                uint8_t e = s[j + 1];
                if (e == '>') str_lit(out, "&gt;");
                else if (e == '\\') str_lit(out, "\\\\");
                else str_c(out, e);
                return j + 2;
            }
            str_lit(out, "\\\\");
            return j + 1;
        }
    }
}

/* Inline fast-path for *, `, [, \ — handles ~96% of bold/em/code and ~90% of
 * links with zero recursion and zero find_unesc; delegates complex cases to
 * noinline handle_markup_slow. */
static inline size_t handle_special_json(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, size_t j,
                                         Str *out, Str *tmp, uint8_t **dst_p, uint8_t **dst_end_p) {
    uint8_t *dst = *dst_p;
    uint8_t *dst_end = *dst_end_p;
    uint8_t c = s[j];
    switch (c) {
        case '\n': dst[0] = '\\'; dst[1] = 'n'; *dst_p = dst + 2; return j + 1;
        case '&': __builtin_memcpy(dst, "&amp;", 5); *dst_p = dst + 5; return j + 1;
        case '<': __builtin_memcpy(dst, "&lt;", 4); *dst_p = dst + 4; return j + 1;
        case '>': __builtin_memcpy(dst, "&gt;", 4); *dst_p = dst + 4; return j + 1;
        case '"': __builtin_memcpy(dst, "&quot;", 6); *dst_p = dst + 6; return j + 1;
        case '\'': __builtin_memcpy(dst, "&#39;", 5); *dst_p = dst + 5; return j + 1;
        case '*': {
            if (j + 1 < n && s[j + 1] == '*') {
                size_t q = j + 2;
                while (q < n && !TBL_INL[s[q]]) q++;
                if (__builtin_expect(q > j + 2 && q + 1 < n && s[q] == '*' && s[q + 1] == '*', 1)) {
                    size_t slen = q - (j + 2);
                    if (__builtin_expect(dst + slen + 64 > dst_end, 0)) {
                        out->len = (size_t)(dst - out->data);
                        str_grow(out, out->len + slen + 256);
                        dst = out->data + out->len; dst_end = out->data + out->cap;
                    }
                    __builtin_memcpy(dst, "<strong>", 8); dst += 8;
                    copy_fast(dst, s + j + 2, slen); dst += slen;
                    __builtin_memcpy(dst, "</strong>", 9); dst += 9;
                    *dst_p = dst; *dst_end_p = dst_end;
                    return q + 2;
                }
            } else {
                size_t q = j + 1;
                while (q < n && !TBL_INL[s[q]]) q++;
                if (__builtin_expect(q > j + 1 && q < n && s[q] == '*' && !(q + 1 < n && s[q + 1] == '*'), 1)) {
                    size_t slen = q - (j + 1);
                    if (__builtin_expect(dst + slen + 64 > dst_end, 0)) {
                        out->len = (size_t)(dst - out->data);
                        str_grow(out, out->len + slen + 256);
                        dst = out->data + out->len; dst_end = out->data + out->cap;
                    }
                    __builtin_memcpy(dst, "<em>", 4); dst += 4;
                    copy_fast(dst, s + j + 1, slen); dst += slen;
                    __builtin_memcpy(dst, "</em>", 5); dst += 5;
                    *dst_p = dst; *dst_end_p = dst_end;
                    return q + 1;
                }
            }
            break;
        }
        case '`': {
            size_t q = j + 1;
            while (q < n && !TBL_CODE_FAST[s[q]]) q++;
            if (__builtin_expect(q > j + 1 && q < n && s[q] == '`', 1)) {
                size_t slen = q - (j + 1);
                if (__builtin_expect(dst + slen + 64 > dst_end, 0)) {
                    out->len = (size_t)(dst - out->data);
                    str_grow(out, out->len + slen + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                }
                __builtin_memcpy(dst, "<code>", 6); dst += 6;
                copy_fast(dst, s + j + 1, slen); dst += slen;
                __builtin_memcpy(dst, "</code>", 7); dst += 7;
                *dst_p = dst; *dst_end_p = dst_end;
                return q + 1;
            }
            break;
        }
        case '[': {
            size_t q = j + 1;
            while (q < n && !TBL_LTXT_FAST[s[q]]) q++;
            if (__builtin_expect(q < n && s[q] == ']' && q + 1 < n && s[q + 1] == '(', 1)) {
                size_t u = q + 2;
                while (u < n && !TBL_LURL_FAST[s[u]]) u++;
                if (__builtin_expect(u > q + 2 && u < n && s[u] == ')', 1)) {
                    size_t href_len; bool hclean;
                    const uint8_t *href = resolve_href_fast(p, env, s + q + 2, u - (q + 2), true, tmp, &href_len, &hclean);
                    size_t txt_len = q - (j + 1);
                    if (__builtin_expect(hclean, 1)) {
                        if (__builtin_expect(dst + href_len + txt_len + 64 > dst_end, 0)) {
                            out->len = (size_t)(dst - out->data);
                            str_grow(out, out->len + href_len + txt_len + 256);
                            dst = out->data + out->len; dst_end = out->data + out->cap;
                        }
                        __builtin_memcpy(dst, "<a href=\\\"", 10); dst += 10;
                        copy_fast(dst, href, href_len); dst += href_len;
                        __builtin_memcpy(dst, "\\\">", 3); dst += 3;
                        copy_fast(dst, s + j + 1, txt_len); dst += txt_len;
                        __builtin_memcpy(dst, "</a>", 4); dst += 4;
                        *dst_p = dst; *dst_end_p = dst_end;
                        return u + 1;
                    }
                }
            }
            break;
        }
        case '\\': {
            if (j + 1 < n && is_escapable(s[j + 1])) {
                uint8_t e = s[j + 1];
                if (e == '>') { __builtin_memcpy(dst, "&gt;", 4); dst += 4; }
                else if (e == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
                else { *dst++ = e; }
                *dst_p = dst;
                return j + 2;
            }
            dst[0] = '\\'; dst[1] = '\\'; *dst_p = dst + 2;
            return j + 1;
        }
        default: {
            *dst_p = emit_html_json_byte(dst, c);
            return j + 1;
        }
    }
    out->len = (size_t)(dst - out->data);
    size_t nj = handle_markup_slow(p, env, s, n, j, out, tmp);
    if (__builtin_expect(out->len + (n - nj) + 64 > out->cap, 0))
        str_grow(out, out->len + (n - nj) + 256);
    *dst_p = out->data + out->len;
    *dst_end_p = out->data + out->cap;
    return nj;
}

__attribute__((target("avx2,bmi")))
static void inline_span_json_avx2(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t j = 0;

    if (n >= 16) {
        __m256i lo_lut = _mm256_setr_epi8(
            17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1,
            17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1);
        __m256i hi_lut = _mm256_setr_epi8(
            1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        __m256i mask0f = _mm256_set1_epi8(0x0f);
        __m256i zero = _mm256_setzero_si256();
        while (j < n) {
            if (__builtin_expect(dst + 64 > dst_end, 0)) {
                out->len = (size_t)(dst - out->data);
                str_grow(out, out->len + (n - j) + 256);
                dst = out->data + out->len; dst_end = out->data + out->cap;
            }
            while (j + 32 <= n) {
                if (__builtin_expect(dst + 64 > dst_end, 0)) {
                    out->len = (size_t)(dst - out->data);
                    str_grow(out, out->len + (n - j) + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                }
                __m256i v = _mm256_loadu_si256((const __m256i *)(s + j));
                __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
                __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
                uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), zero));
                _mm256_storeu_si256((__m256i *)dst, v);
                if (m == 0) { dst += 32; j += 32; continue; }
                size_t k = (size_t)__builtin_ctz(m);
                dst += k; j += k;
                goto hit_avx2;
            }
            if (j + 16 <= n) {
                __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
                __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
                __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                              _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
                uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), _mm_setzero_si128()))) ^ 0xffffu;
                _mm_storeu_si128((__m128i *)dst, v);
                if (m == 0) { dst += 16; j += 16; }
                else { size_t k = (size_t)__builtin_ctz(m); dst += k; j += k; goto hit_avx2; }
            }
            while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
            if (j >= n) break;
        hit_avx2:
            j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
        }
        out->len = (size_t)(dst - out->data);
        return;
    }
    while (j < n) {
        if (__builtin_expect(dst + 64 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - j) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
        if (j >= n) break;
        j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
    }
    out->len = (size_t)(dst - out->data);
}

__attribute__((target("ssse3")))
static void inline_span_json_ssse3(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t j = 0;

    if (n >= 16) {
        __m128i lo_lut = _mm_setr_epi8(17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1);
        __m128i hi_lut = _mm_setr_epi8(1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        __m128i mask0f = _mm_set1_epi8(0x0f);
        __m128i zero = _mm_setzero_si128();
        while (j < n) {
            if (__builtin_expect(dst + 48 > dst_end, 0)) {
                out->len = (size_t)(dst - out->data);
                str_grow(out, out->len + (n - j) + 256);
                dst = out->data + out->len; dst_end = out->data + out->cap;
            }
            while (j + 16 <= n) {
                if (__builtin_expect(dst + 48 > dst_end, 0)) {
                    out->len = (size_t)(dst - out->data);
                    str_grow(out, out->len + (n - j) + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                }
                __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
                __m128i lo = _mm_shuffle_epi8(lo_lut, v);
                __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
                uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), zero))) ^ 0xffffu;
                _mm_storeu_si128((__m128i *)dst, v);
                if (m == 0) { dst += 16; j += 16; continue; }
                size_t k = (size_t)__builtin_ctz(m);
                dst += k; j += k;
                goto hit_ssse3;
            }
            while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
            if (j >= n) break;
        hit_ssse3:
            j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
        }
        out->len = (size_t)(dst - out->data);
        return;
    }
    while (j < n) {
        if (__builtin_expect(dst + 48 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - j) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
        if (j >= n) break;
        j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
    }
    out->len = (size_t)(dst - out->data);
}

static void inline_span_json_sse2(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    if (n == 0) return;
    if (__builtin_expect(out->len + n + 128 > out->cap, 0)) str_grow(out, out->len + n + 256);
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t j = 0;

    if (n >= 16) {
        __m128i zero = _mm_setzero_si128();
        __m128i v60 = _mm_set1_epi8(0x60);
        __m128i v20 = _mm_set1_epi8(0x20);
        __m128i v41 = _mm_set1_epi8(0x41);
        __m128i vm100 = _mm_set1_epi8((char)-100);
        __m128i v55 = _mm_set1_epi8(0x55);
        __m128i vm111 = _mm_set1_epi8((char)-111);
        while (j < n) {
            if (__builtin_expect(dst + 48 > dst_end, 0)) {
                out->len = (size_t)(dst - out->data);
                str_grow(out, out->len + (n - j) + 256);
                dst = out->data + out->len; dst_end = out->data + out->cap;
            }
            while (j + 16 <= n) {
                if (__builtin_expect(dst + 48 > dst_end, 0)) {
                    out->len = (size_t)(dst - out->data);
                    str_grow(out, out->len + (n - j) + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                }
                __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
                __m128i safe = _mm_or_si128(
                    _mm_or_si128(_mm_cmpgt_epi8(v, v60), _mm_cmpgt_epi8(zero, v)),
                    _mm_or_si128(_mm_cmpeq_epi8(v, v20),
                        _mm_or_si128(_mm_cmpgt_epi8(vm100, _mm_add_epi8(v, v41)),
                                     _mm_cmpgt_epi8(vm111, _mm_add_epi8(v, v55)))));
                uint32_t m = ((uint32_t)_mm_movemask_epi8(safe)) ^ 0xffffu;
                _mm_storeu_si128((__m128i *)dst, v);
                if (m == 0) { dst += 16; j += 16; continue; }
                size_t k = (size_t)__builtin_ctz(m);
                dst += k; j += k;
                if (!TBL_INL[s[j]]) { *dst++ = s[j++]; continue; }
                goto hit_sse2;
            }
            while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
            if (j >= n) break;
        hit_sse2:
            j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
        }
        out->len = (size_t)(dst - out->data);
        return;
    }
    while (j < n) {
        if (__builtin_expect(dst + 48 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - j) + 256);
            dst = out->data + out->len; dst_end = out->data + out->cap;
        }
        while (j < n && !TBL_INL[s[j]]) { *dst++ = s[j++]; }
        if (j >= n) break;
        j = handle_special_json(p, env, s, n, j, out, tmp, &dst, &dst_end);
    }
    out->len = (size_t)(dst - out->data);
}

static void inline_span_json(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    if (n == 0) return;
    if (g_has_avx2) inline_span_json_avx2(p, env, s, n, out, tmp);
    else if (g_has_ssse3) inline_span_json_ssse3(p, env, s, n, out, tmp);
    else inline_span_json_sse2(p, env, s, n, out, tmp);
}

/* ---------------- inline raw (non-JSON fallback) ---------------- */

static void inline_span_raw(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    size_t i = 0;
    while (i < n) {
        int j = scan_inline_md(s, n, i);
        if (j < 0) { str_add(out, s + i, n - i); return; }
        str_add(out, s + i, (size_t)j - i);
        uint8_t c = s[j];
        switch (c) {
            case '\\': {
                if ((size_t)j + 1 < n && is_escapable(s[j + 1])) {
                    uint8_t e = s[j + 1];
                    if (e == '>') str_lit(out, "&gt;");
                    else str_c(out, e);
                    i = (size_t)j + 2;
                } else { str_c(out, '\\'); i = (size_t)j + 1; }
                break;
            }
            case '`': {
                int k = scan_byte(s, n, (size_t)j + 1, '`');
                if (k >= 0 && (size_t)k > (size_t)j + 1) {
                    str_lit(out, "<code>");
                    esc_html_into(s + j + 1, (size_t)k - ((size_t)j + 1), out);
                    str_lit(out, "</code>");
                    i = (size_t)k + 1;
                } else { str_c(out, '`'); i = (size_t)j + 1; }
                break;
            }
            case '*': {
                if ((size_t)j + 1 < n && s[j + 1] == '*') {
                    int k = find_unesc_gen(s, n, '*', '*', 2, (size_t)j + 2);
                    if (k > (int)(j + 2)) {
                        str_lit(out, "<strong>");
                        inline_span_raw(p, env, s + j + 2, (size_t)k - ((size_t)j + 2), out, tmp);
                        str_lit(out, "</strong>");
                        i = (size_t)k + 2;
                    } else { str_lit(out, "**"); i = (size_t)j + 2; }
                } else {
                    int k = find_lone_star_gen(s, n, (size_t)j + 1);
                    if (k > (int)(j + 1)) {
                        str_lit(out, "<em>");
                        inline_span_raw(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                        str_lit(out, "</em>");
                        i = (size_t)k + 1;
                    } else { str_c(out, '*'); i = (size_t)j + 1; }
                }
                break;
            }
            case '[': {
                int k = find_unesc_gen(s, n, ']', 0, 1, (size_t)j + 1);
                int m = -1;
                if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                    int q = scan_byte(s, n, (size_t)k + 2, ')');
                    if (q >= 0) m = q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t href_len; bool hclean;
                    const uint8_t *href = resolve_href_fast(p, env, target, tl, false, tmp, &href_len, &hclean);
                    str_lit(out, "<a href=\"");
                    esc_html_into(href, href_len, out);
                    str_lit(out, "\">");
                    inline_span_raw(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                    str_lit(out, "</a>");
                    i = (size_t)m + 1;
                } else { str_c(out, '['); i = (size_t)j + 1; }
                break;
            }
            case '&': str_lit(out, "&amp;"); i = (size_t)j + 1; break;
            case '<': str_lit(out, "&lt;"); i = (size_t)j + 1; break;
            case '>': str_lit(out, "&gt;"); i = (size_t)j + 1; break;
            case '"': str_lit(out, "&quot;"); i = (size_t)j + 1; break;
            default: str_lit(out, "&#39;"); i = (size_t)j + 1; break;
        }
    }
}

/* ---------------- single unified block-level builder ---------------- */

static bool md_build_json(Pool *p, const uint8_t *src, size_t end, LinkCtx *env, Dict *slug_cache,
                          Str *scratch, Str *tmp, Str *text_tmp,
                          Sp *html_out, Arr *toc_out, Sp *excerpt_out, bool store_html_in_pool) {
    str_clear(scratch);
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    SmallIdSet ids; small_id_init(&ids);
    size_t i = 0;
    Str *item_scratch = env->item_scratch;
    void (*inl_fn)(Pool *, LinkCtx *, const uint8_t *, size_t, Str *, Str *) =
        g_has_avx2 ? inline_span_json_avx2 : (g_has_ssse3 ? inline_span_json_ssse3 : inline_span_json_sse2);
    void (*esc_fn)(const uint8_t *, size_t, Str *) =
        g_has_avx2 ? esc_html_json_into_str_avx2 : (g_has_ssse3 ? esc_html_json_into_str_ssse3 : esc_html_json_into_str_sse2);

    while (i < end) {
        int nl = scan_byte_sse2_i(src, end, i, '\n');
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (i == le) { i = le + 1; continue; }
        uint8_t c0 = src[i];
        if (c0 <= '`') {
            if ((c0 == ' ' || c0 == '\t') && line_blank(src, i, le)) { i = le + 1; continue; }
            size_t loff, llen;
            if (c0 == '`' && fence_open_at(src, i, le, &loff, &llen)) {
                str_lit(scratch, "<pre><code");
                if (llen > 0) {
                    str_lit(scratch, " class=\\\"language-");
                    str_add(scratch, src + loff, llen);
                    str_lit(scratch, "\\\"");
                }
                str_c(scratch, '>');
                i = le + 1;
                size_t code_start = i;
                size_t code_end = end;
                bool closed = false;
                while (i < end) {
                    int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    if (i < le2 && src[i] == '`' && fence_close_at(src, i, le2)) {
                        code_end = i;
                        i = le2 + 1;
                        closed = true;
                        break;
                    }
                    i = le2 + 1;
                }
                if (code_end > code_start) {
                    esc_fn(src + code_start, code_end - code_start, scratch);
                    if (!closed && src[code_end - 1] != '\n') str_lit(scratch, "\\n");
                }
                str_lit(scratch, "</code></pre>\\n");
                continue;
            }
            size_t depth, t_off, t_len;
            if (c0 == '#' && heading_at(src, i, le, &depth, &t_off, &t_len)) {
                Sp slug;
                if (!dict_get(slug_cache, p, src + t_off, t_len, &slug)) {
                    Sp text_s = pool_store(p, src + t_off, t_len);
                    slug = slug_make(p, text_s, slug_cache);
                }
                Sp id = small_id_unique(p, slug, &ids);
                str_lit(scratch, "<h");
                str_c(scratch, (uint8_t)('0' + depth));
                str_lit(scratch, " id=\\\"");
                str_add(scratch, pool_at(p, id), id.len);
                str_lit(scratch, "\\\">");
                uint32_t html_start = (uint32_t)scratch->len;
                inl_fn(p, env, src + t_off, t_len, scratch, tmp);
                uint32_t html_end = (uint32_t)scratch->len;
                str_lit(scratch, "</h");
                str_c(scratch, (uint8_t)('0' + depth));
                str_lit(scratch, ">\\n");
                if (depth == 2 || depth == 3) {
                    Toc *t = arr_slot(toc_out, p);
                    t->level = (int64_t)depth; t->id = id; t->html = SP_NONE;
                    Range hr = {html_start, html_end - html_start};
                    if (toc_cnt < 64 && toc_html.len == 0) {
                        toc_stack[toc_cnt++] = hr;
                    } else {
                        if (toc_html.len == 0) {
                            for (size_t k = 0; k < toc_cnt; k++) *(Range *)arr_slot(&toc_html, p) = toc_stack[k];
                        }
                        *(Range *)arr_slot(&toc_html, p) = hr;
                        toc_cnt++;
                    }
                }
                i = le + 1;
                continue;
            }
            if ((c0 == '-' || c0 == '*') && hr_at(src, i, le)) {
                str_lit(scratch, "<hr>\\n");
                i = le + 1;
                continue;
            }
            if (c0 == '>') {
                str_clear(text_tmp);
                bool first = true;
                while (i < end) {
                    int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    if (src[i] != '>') break;
                    size_t s2 = i + 1;
                    if (s2 < le2 && src[s2] == ' ') s2++;
                    size_t ts, te;
                    line_trim(src, s2, le2, &ts, &te);
                    if (ts < te) {
                        if (!first) str_c(text_tmp, '\n');
                        first = false;
                        str_add(text_tmp, src + ts, te - ts);
                    }
                    i = le2 + 1;
                }
                if (text_tmp->len == 0) {
                    str_lit(scratch, "<blockquote>\\n</blockquote>\\n");
                } else {
                    str_lit(scratch, "<blockquote>\\n<p>");
                    inl_fn(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                    str_lit(scratch, "</p>\\n</blockquote>\\n");
                }
                continue;
            }
            bool ordered; int64_t start_num; size_t cur_ts, cur_te;
            if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
                && item_start_span(src, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
                bool is_ordered = ordered;
                if (is_ordered && start_num != 1) {
                    str_lit(scratch, "<ol start=\\\"");
                    str_i64(scratch, start_num);
                    str_lit(scratch, "\\\">\\n");
                } else {
                    if (is_ordered) str_lit(scratch, "<ol>\\n");
                    else str_lit(scratch, "<ul>\\n");
                }
                i = le + 1;
                for (;;) {
                    bool multi = false;
                    bool has_next = false;
                    size_t next_ts = 0, next_te = 0, next_le = 0;
                    while (i < end) {
                        int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                        size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                        bool o2; int64_t s2;
                        size_t nts, nte;
                        if (i < le2 && (src[i] == '-' || src[i] == '*' || (src[i] >= '0' && src[i] <= '9'))
                            && item_start_span(src, i, le2, &o2, &s2, &nts, &nte)) {
                            if (o2 == is_ordered) {
                                has_next = true;
                                next_ts = nts; next_te = nte; next_le = le2;
                            }
                            break;
                        }
                        if (i < le2 && src[i] == ' ' && continued_at(src, i, le2)) {
                            if (!multi) {
                                str_clear(item_scratch);
                                str_add(item_scratch, src + cur_ts, cur_te - cur_ts);
                                multi = true;
                            }
                            size_t cts, cte;
                            line_trim(src, i, le2, &cts, &cte);
                            str_c(item_scratch, '\n');
                            str_add(item_scratch, src + cts, cte - cts);
                            i = le2 + 1;
                            continue;
                        }
                        break;
                    }
                    const uint8_t *its = multi ? item_scratch->data : src + cur_ts;
                    size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                    str_lit(scratch, "<li>");
                    inl_fn(p, env, its, itn, scratch, tmp);
                    str_lit(scratch, "</li>\\n");
                    if (!has_next) break;
                    cur_ts = next_ts; cur_te = next_te;
                    i = next_le + 1;
                }
                if (is_ordered) str_lit(scratch, "</ol>\\n");
                else str_lit(scratch, "</ul>\\n");
                continue;
            }
        }
        size_t ts = i, te = le;
        if (c0 == ' ' || c0 == '\t') { while (ts < te && (src[ts] == ' ' || src[ts] == '\t')) ts++; }
        if (te > ts && (src[te - 1] == ' ' || src[te - 1] == '\t')) { while (te > ts && (src[te - 1] == ' ' || src[te - 1] == '\t')) te--; }
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            uint8_t cn = src[i];
            if (cn == '\n') break;
            int nl2 = scan_byte_sse2_i(src, end, i, '\n');
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (cn <= '`' && para_ends(src, i, le2)) break;
            if (__builtin_expect(!multi && contiguous && cn != ' ' && cn != '\t', 1)) {
                uint8_t cl = src[le2 - 1];
                if (__builtin_expect(cl != ' ' && cl != '\t', 1)) {
                    span_end = le2;
                    i = le2 + 1;
                    continue;
                }
            }
            size_t cts, cte;
            line_trim(src, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, src + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, src + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : src + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        str_lit(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        inl_fn(p, env, ps, pn, scratch, tmp);
        uint32_t html_end = (uint32_t)scratch->len;
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        str_lit(scratch, "</p>\\n");
    }
    if (store_html_in_pool) {
        *html_out = pool_store(p, scratch->data, scratch->len);
        if (has_excerpt) *excerpt_out = (Sp){html_out->off + excerpt_range.start, excerpt_range.len};
        else *excerpt_out = SP_NONE;
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){html_out->off + r.start, r.len};
        }
    } else {
        *html_out = (Sp){0, (uint32_t)scratch->len};
        if (has_excerpt) *excerpt_out = pool_store(p, scratch->data + excerpt_range.start, excerpt_range.len);
        else *excerpt_out = SP_NONE;
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){r.start, r.len};
        }
    }
    return true;
}

bool md_build_buf(Pool *p, const uint8_t *src, size_t end, LinkCtx *env, Dict *slug_cache,
                  Str *scratch, Str *tmp, Str *text_tmp,
                  Sp *html_out, Arr *toc_out, Sp *excerpt_out, bool json_esc, bool store_html_in_pool) {
    if (__builtin_expect(json_esc, 1)) {
        return md_build_json(p, src, end, env, slug_cache, scratch, tmp, text_tmp, html_out, toc_out, excerpt_out, store_html_in_pool);
    }
    str_clear(scratch);
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    SmallIdSet ids; small_id_init(&ids);
    size_t i = 0;
    Str *item_scratch = env->item_scratch;
    while (i < end) {
        int nl = scan_byte_sse2_i(src, end, i, '\n');
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (i == le) { i = le + 1; continue; }
        uint8_t c0 = src[i];
        if ((c0 == ' ' || c0 == '\t') && line_blank(src, i, le)) { i = le + 1; continue; }
        size_t loff, llen;
        if (c0 == '`' && fence_open_at(src, i, le, &loff, &llen)) {
            str_lit(scratch, "<pre><code");
            if (llen > 0) {
                str_lit(scratch, " class=\"language-");
                str_add(scratch, src + loff, llen);
                str_c(scratch, '"');
            }
            str_c(scratch, '>');
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (i < le2 && src[i] == '`' && fence_close_at(src, i, le2)) { i = le2 + 1; break; }
                esc_html_into(src + i, le2 - i, scratch);
                str_c(scratch, '\n');
                i = le2 + 1;
            }
            str_lit(scratch, "</code></pre>\n");
            continue;
        }
        size_t depth, t_off, t_len;
        if (c0 == '#' && heading_at(src, i, le, &depth, &t_off, &t_len)) {
            Sp slug;
            if (!dict_get(slug_cache, p, src + t_off, t_len, &slug)) {
                Sp text_s = pool_store(p, src + t_off, t_len);
                slug = slug_make(p, text_s, slug_cache);
            }
            Sp id = small_id_unique(p, slug, &ids);
            str_lit(scratch, "<h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_lit(scratch, " id=\"");
            str_add(scratch, pool_at(p, id), id.len);
            str_lit(scratch, "\">");
            uint32_t html_start = (uint32_t)scratch->len;
            inline_span_raw(p, env, src + t_off, t_len, scratch, tmp);
            uint32_t html_end = (uint32_t)scratch->len;
            str_lit(scratch, "</h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_lit(scratch, ">\n");
            if (depth == 2 || depth == 3) {
                Toc *t = arr_slot(toc_out, p);
                t->level = (int64_t)depth; t->id = id; t->html = SP_NONE;
                Range hr = {html_start, html_end - html_start};
                if (toc_cnt < 64 && toc_html.len == 0) {
                    toc_stack[toc_cnt++] = hr;
                } else {
                    if (toc_html.len == 0) {
                        for (size_t k = 0; k < toc_cnt; k++) *(Range *)arr_slot(&toc_html, p) = toc_stack[k];
                    }
                    *(Range *)arr_slot(&toc_html, p) = hr;
                    toc_cnt++;
                }
            }
            i = le + 1;
            continue;
        }
        if ((c0 == '-' || c0 == '*') && hr_at(src, i, le)) {
            str_lit(scratch, "<hr>\n");
            i = le + 1;
            continue;
        }
        if (c0 == '>') {
            str_clear(text_tmp);
            bool first = true;
            while (i < end) {
                int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (src[i] != '>') break;
                size_t s2 = i + 1;
                if (s2 < le2 && src[s2] == ' ') s2++;
                size_t ts, te;
                line_trim(src, s2, le2, &ts, &te);
                if (ts < te) {
                    if (!first) str_c(text_tmp, '\n');
                    first = false;
                    str_add(text_tmp, src + ts, te - ts);
                }
                i = le2 + 1;
            }
            if (text_tmp->len == 0) {
                str_lit(scratch, "<blockquote>\n</blockquote>\n");
            } else {
                str_lit(scratch, "<blockquote>\n<p>");
                inline_span_raw(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                str_lit(scratch, "</p>\n</blockquote>\n");
            }
            continue;
        }
        bool ordered; int64_t start_num; size_t cur_ts, cur_te;
        if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
            && item_start_span(src, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
            bool is_ordered = ordered;
            if (is_ordered && start_num != 1) {
                str_lit(scratch, "<ol start=\"");
                str_i64(scratch, start_num);
                str_lit(scratch, "\">\n");
            } else {
                if (is_ordered) str_lit(scratch, "<ol>\n");
                else str_lit(scratch, "<ul>\n");
            }
            i = le + 1;
            for (;;) {
                bool multi = false;
                bool has_next = false;
                size_t next_ts = 0, next_te = 0, next_le = 0;
                while (i < end) {
                    int nl2 = scan_byte_sse2_i(src, end, i, '\n');
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    bool o2; int64_t s2;
                    size_t nts, nte;
                    if (i < le2 && (src[i] == '-' || src[i] == '*' || (src[i] >= '0' && src[i] <= '9'))
                        && item_start_span(src, i, le2, &o2, &s2, &nts, &nte)) {
                        if (o2 == is_ordered) {
                            has_next = true;
                            next_ts = nts; next_te = nte; next_le = le2;
                        }
                        break;
                    }
                    if (i < le2 && src[i] == ' ' && continued_at(src, i, le2)) {
                        if (!multi) {
                            str_clear(item_scratch);
                            str_add(item_scratch, src + cur_ts, cur_te - cur_ts);
                            multi = true;
                        }
                        size_t cts, cte;
                        line_trim(src, i, le2, &cts, &cte);
                        str_c(item_scratch, '\n');
                        str_add(item_scratch, src + cts, cte - cts);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                const uint8_t *its = multi ? item_scratch->data : src + cur_ts;
                size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                str_lit(scratch, "<li>");
                inline_span_raw(p, env, its, itn, scratch, tmp);
                str_lit(scratch, "</li>\n");
                if (!has_next) break;
                cur_ts = next_ts; cur_te = next_te;
                i = next_le + 1;
            }
            if (is_ordered) str_lit(scratch, "</ol>\n");
            else str_lit(scratch, "</ul>\n");
            continue;
        }
        size_t ts, te;
        line_trim(src, i, le, &ts, &te);
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            int nl2 = scan_byte_sse2_i(src, end, i, '\n');
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (para_ends(src, i, le2)) break;
            size_t cts, cte;
            line_trim(src, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, src + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, src + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : src + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        str_lit(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        inline_span_raw(p, env, ps, pn, scratch, tmp);
        uint32_t html_end = (uint32_t)scratch->len;
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        str_lit(scratch, "</p>\n");
    }
    if (store_html_in_pool) {
        *html_out = pool_store(p, scratch->data, scratch->len);
        if (has_excerpt) *excerpt_out = (Sp){html_out->off + excerpt_range.start, excerpt_range.len};
        else *excerpt_out = SP_NONE;
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){html_out->off + r.start, r.len};
        }
    } else {
        *html_out = (Sp){0, (uint32_t)scratch->len};
        if (has_excerpt) *excerpt_out = pool_store(p, scratch->data + excerpt_range.start, excerpt_range.len);
        else *excerpt_out = SP_NONE;
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){r.start, r.len};
        }
    }
    return true;
}
