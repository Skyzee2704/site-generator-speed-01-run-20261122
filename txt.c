/* txt.c — UTF-8, whitespace, UTF-16 order, NFKD slugify, word count,
 * long dates, JSON/HTML escaping. Follows the reference semantics
 * exactly (including the lenient UTF-8 decoder, which masks and
 * combines without validating continuation bytes or ranges). */
#include "zg.h"
#include "decomp.h"
#include <immintrin.h>

static const char *MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August",
    "September", "October", "November", "December"
};

static inline bool ws_byte(uint8_t b) {
    return b == ' ' || b == '\t' || b == '\n' || b == 0x0b || b == 0x0c || b == '\r';
}

int u8_decode(const uint8_t *b, size_t n, size_t i, uint32_t *cp) {
    uint8_t c0 = b[i];
    if (c0 < 0x80) { *cp = c0; return 1; }
    if (c0 >= 0xf0) {
        if (i + 3 >= n) { *cp = 0xfffd; return 1; }
        *cp = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(b[i + 1] & 0x3f) << 12)
            | ((uint32_t)(b[i + 2] & 0x3f) << 6) | (uint32_t)(b[i + 3] & 0x3f);
        return 4;
    }
    if (c0 >= 0xe0) {
        if (i + 2 >= n) { *cp = 0xfffd; return 1; }
        *cp = ((uint32_t)(c0 & 0x0f) << 12) | ((uint32_t)(b[i + 1] & 0x3f) << 6)
            | (uint32_t)(b[i + 2] & 0x3f);
        return 3;
    }
    if (i + 1 >= n) { *cp = 0xfffd; return 1; }
    *cp = ((uint32_t)(c0 & 0x1f) << 6) | (uint32_t)(b[i + 1] & 0x3f);
    return 2;
}

int u8_encode(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xc0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xe0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (uint8_t)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (uint8_t)(0xf0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (uint8_t)(0x80 | (cp & 0x3f));
    return 4;
}

bool u_is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == 0x0b || cp == 0x0c || cp == '\r'
        || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a)
        || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f
        || cp == 0x3000 || cp == 0xfeff;
}

int u16_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    /* Fast path: while both bytes are ASCII, UTF-16 code units equal the
     * byte values, so byte order is UTF-16 order. Fall back to decoding
     * at the first non-ASCII byte; that position is a character boundary
     * in both strings because all preceding bytes were ASCII. */
    size_t n = an < bn ? an : bn;
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t wa, wb;
        __builtin_memcpy(&wa, a + i, 8);
        __builtin_memcpy(&wb, b + i, 8);
        if (__builtin_expect(((wa | wb) & 0x8080808080808080ULL) == 0, 1)) {
            if (wa != wb) {
                int k = __builtin_ctzll(wa ^ wb) >> 3;
                return a[i + k] < b[i + k] ? -1 : 1;
            }
            i += 8;
            continue;
        }
        break;
    }
    for (; i < n; i++) {
        uint8_t ca = a[i], cb = b[i];
        if (ca != cb) {
            if (ca < 0x80 && cb < 0x80) return ca < cb ? -1 : 1;
            break;
        }
        if (ca >= 0x80) break;
    }
    if (i == n) {
        if (an == bn) return 0;
        return an < bn ? -1 : 1;
    }
    size_t ai = i, bi = i;
    uint16_t apend = 0, bpend = 0;
    bool ahas = false, bhas = false;
    for (;;) {
        uint16_t au, bu;
        bool aok, bok;
        if (ahas) { au = apend; ahas = false; aok = true; }
        else if (ai >= an) aok = false;
        else {
            uint32_t cp; size_t m = (size_t)u8_decode(a, an, ai, &cp); ai += m;
            if (cp >= 0x10000) {
                uint32_t v = cp - 0x10000;
                apend = (uint16_t)(0xdc00 + (v & 0x3ff)); ahas = true;
                au = (uint16_t)(0xd800 + ((v >> 10) & 0x3ff));
            } else au = (uint16_t)cp;
            aok = true;
        }
        if (bhas) { bu = bpend; bhas = false; bok = true; }
        else if (bi >= bn) bok = false;
        else {
            uint32_t cp; size_t m = (size_t)u8_decode(b, bn, bi, &cp); bi += m;
            if (cp >= 0x10000) {
                uint32_t v = cp - 0x10000;
                bpend = (uint16_t)(0xdc00 + (v & 0x3ff)); bhas = true;
                bu = (uint16_t)(0xd800 + ((v >> 10) & 0x3ff));
            } else bu = (uint16_t)cp;
            bok = true;
        }
        if (!aok && !bok) return 0;
        if (!aok) return -1;
        if (!bok) return 1;
        if (au != bu) return au < bu ? -1 : 1;
    }
}

Sp date_long(Pool *p, Sp d) {
    const uint8_t *b = pool_at(p, d);
    size_t n = d.len;
    uint32_t day = 0;
    size_t mo = 0;
    uint8_t year[4] = {0, 0, 0, 0};
    if (n >= 10) day = (uint32_t)(b[8] - '0') * 10 + (uint32_t)(b[9] - '0');
    if (n >= 7) {
        size_t m = (size_t)(b[5] - '0') * 10 + (size_t)(b[6] - '0');
        mo = m >= 1 ? m - 1 : 0;
    }
    if (n >= 4) { year[0] = b[0]; year[1] = b[1]; year[2] = b[2]; year[3] = b[3]; }
    const char *month = mo < 12 ? MONTH_NAMES[mo] : "";
    uint32_t start = pool_mark(p);
    pool_i64(p, (int64_t)day);
    pool_u8(p, ' ');
    size_t ml = 0; while (month[ml]) ml++;
    pool_put(p, (const uint8_t *)month, ml);
    pool_u8(p, ' ');
    pool_put(p, year, 4);
    return pool_span(p, start);
}

static inline bool is_mark(uint32_t cp) { return cp >= 0x300 && cp <= 0x36f; }

uint32_t nfkd_find(uint32_t cp, uint32_t *out) {
    size_t lo = 0, hi = DECOMP_N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (DECOMP[mid].cp < cp) lo = mid + 1;
        else if (DECOMP[mid].cp > cp) hi = mid;
        else {
            out[0] = DECOMP[mid].d0;
            out[1] = DECOMP[mid].d1;
            out[2] = DECOMP[mid].d2;
            return DECOMP[mid].n;
        }
    }
    return 0;
}

/* NFKD for one code point: fullwidth ASCII by formula, U+3000 to space,
 * else the table; 0 when there is no decomposition. */
static int decomp_into(uint32_t c, uint32_t buf[3]) {
    if (c >= 0xff01 && c <= 0xff5e) { buf[0] = c - 0xfee0; return 1; }
    if (c == 0x3000) { buf[0] = ' '; return 1; }
    return (int)nfkd_find(c, buf);
}

static const uint8_t SLUG_ASCII[256] = {
    ['0'] = '0', ['1'] = '1', ['2'] = '2', ['3'] = '3', ['4'] = '4',
    ['5'] = '5', ['6'] = '6', ['7'] = '7', ['8'] = '8', ['9'] = '9',
    ['A'] = 'a', ['B'] = 'b', ['C'] = 'c', ['D'] = 'd', ['E'] = 'e',
    ['F'] = 'f', ['G'] = 'g', ['H'] = 'h', ['I'] = 'i', ['J'] = 'j',
    ['K'] = 'k', ['L'] = 'l', ['M'] = 'm', ['N'] = 'n', ['O'] = 'o',
    ['P'] = 'p', ['Q'] = 'q', ['R'] = 'r', ['S'] = 's', ['T'] = 't',
    ['U'] = 'u', ['V'] = 'v', ['W'] = 'w', ['X'] = 'x', ['Y'] = 'y',
    ['Z'] = 'z',
    ['a'] = 'a', ['b'] = 'b', ['c'] = 'c', ['d'] = 'd', ['e'] = 'e',
    ['f'] = 'f', ['g'] = 'g', ['h'] = 'h', ['i'] = 'i', ['j'] = 'j',
    ['k'] = 'k', ['l'] = 'l', ['m'] = 'm', ['n'] = 'n', ['o'] = 'o',
    ['p'] = 'p', ['q'] = 'q', ['r'] = 'r', ['s'] = 's', ['t'] = 't',
    ['u'] = 'u', ['v'] = 'v', ['w'] = 'w', ['x'] = 'x', ['y'] = 'y',
    ['z'] = 'z',
    [0x80 ... 0xff] = 0xff
};

Sp slug_make_fast(Pool *p, Sp text, Dict *non_ascii_cache) {
    const uint8_t *bytes = pool_at(p, text);
    uint32_t start = pool_mark(p);
    bool gap = false;
    bool ascii = true;
    uint8_t *dst0 = p->base + start;
    uint8_t *dst = dst0;
    for (uint32_t i = 0; i < text.len; i++) {
        uint8_t v = SLUG_ASCII[bytes[i]];
        if (__builtin_expect(v == 0xff, 0)) { ascii = false; break; }
        if (v) {
            if (gap) *dst++ = '-';
            gap = false;
            *dst++ = v;
        } else {
            gap = dst > dst0;
        }
    }
    if (__builtin_expect(ascii, 1)) {
        size_t slen = (size_t)(dst - dst0);
        if (slen > 64) slen = 64;
        while (slen > 0 && dst0[slen - 1] == '-') slen--;
        if (__builtin_expect(slen == 0, 0)) {
            p->len = start;
            return pool_store(p, (const uint8_t *)"untitled", 8);
        }
        p->len = start + slen;
        return (Sp){start, (uint32_t)slen};
    }
    p->len = start;
    return slug_make(p, text, non_ascii_cache);
}

Sp slug_make(Pool *p, Sp text, Dict *cache) {
    const uint8_t *bytes = pool_at(p, text);
    Sp hit;
    if (dict_get(cache, p, bytes, text.len, &hit)) return hit;
    uint32_t start = pool_mark(p);
    bool gap = false;
    bool ascii = true;
    uint8_t *dst0 = p->base + start;
    uint8_t *dst = dst0;
    for (uint32_t i = 0; i < text.len; i++) {
        uint8_t v = SLUG_ASCII[bytes[i]];
        if (v == 0xff) { ascii = false; break; }
        if (v) {
            if (gap) *dst++ = '-';
            gap = false;
            *dst++ = v;
        } else {
            gap = dst > dst0;
        }
    }
    if (ascii) {
        p->len = (size_t)(dst - p->base);
    } else {
        p->len = start;
        gap = false;
        size_t i = 0;
        while (i < text.len) {
            uint32_t c; size_t n = (size_t)u8_decode(bytes, text.len, i, &c); i += n;
            uint32_t buf[3]; int cnt = decomp_into(c, buf);
            uint32_t seq[3]; size_t slen;
            if (cnt == 0) { seq[0] = c; slen = 1; }
            else { seq[0] = buf[0]; seq[1] = buf[1]; seq[2] = buf[2]; slen = (size_t)cnt; }
            for (size_t k = 0; k < slen; k++) {
                uint32_t d = seq[k];
                if (is_mark(d)) continue;
                int keep = -1;
                if (d >= 65 && d <= 90) keep = (int)(d + 32);
                else if ((d >= 97 && d <= 122) || (d >= 48 && d <= 57)) keep = (int)d;
                if (keep >= 0) {
                    if (gap) pool_u8(p, '-');
                    gap = false;
                    pool_u8(p, (uint8_t)keep);
                } else {
                    gap = p->len > start;
                }
            }
        }
    }
    Sp s = pool_span(p, start);
    if (s.len > 64) {
        s.len = 64;
        p->len = s.off + s.len;
    }
    while (s.len > 0 && p->base[s.off + s.len - 1] == '-') {
        s.len--;
        p->len = s.off + s.len;
    }
    if (s.len == 0) s = pool_store(p, (const uint8_t *)"untitled", 8);
    dict_put(cache, p, text, s);
    return s;
}

Sp slug_unique(Pool *p, Sp slug, Set *taken) {
    if (set_add(taken, p, slug)) return slug;
    uint8_t stack[64];
    size_t slen = slug.len;
    copy_bytes(stack, pool_at(p, slug), slen);
    uint64_t n = 2;
    for (;;) {
        uint32_t start = pool_mark(p);
        pool_put(p, stack, slen);
        pool_u8(p, '-');
        pool_u64(p, n);
        Sp cand = pool_span(p, start);
        n++;
        if (set_add(taken, p, cand)) return cand;
    }
}

static inline unsigned popcount16(uint16_t x) {
    x = (uint16_t)((x & 0x5555u) + ((x >> 1) & 0x5555u));
    x = (uint16_t)((x & 0x3333u) + ((x >> 2) & 0x3333u));
    x = (uint16_t)((x & 0x0f0fu) + ((x >> 4) & 0x0f0fu));
    x = (uint16_t)((x & 0x00ffu) + ((x >> 8) & 0x00ffu));
    return x;
}

__attribute__((target("avx2,popcnt")))
static size_t word_count_avx2(const uint8_t *b, size_t n) {
    size_t count = 0, i = 0;
    bool in_word = false;
    __m256i vsp = _mm256_set1_epi8(' ');
    __m256i v8 = _mm256_set1_epi8(8);
    __m256i v14 = _mm256_set1_epi8(14);
    __m256i maskf0 = _mm256_set1_epi8((char)0xf0);
    __m256i ve0 = _mm256_set1_epi8((char)0xe0);
    __m256i vc2 = _mm256_set1_epi8((char)0xc2);
    while (i + 64 <= n) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(b + i + 32));
        if (__builtin_expect(_mm256_movemask_epi8(_mm256_or_si256(v0, v1)) != 0, 0)) {
            __m256i sp0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, vc2),
                                          _mm256_cmpeq_epi8(_mm256_and_si256(v0, maskf0), ve0));
            __m256i sp1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, vc2),
                                          _mm256_cmpeq_epi8(_mm256_and_si256(v1, maskf0), ve0));
            if (_mm256_movemask_epi8(_mm256_or_si256(sp0, sp1)) != 0) {
                size_t end = i + 64;
                while (i < end) {
                    uint8_t c0 = b[i];
                    if (c0 < 0x80) {
                        if (ws_byte(c0)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i++;
                    } else if ((c0 & 0xc0) == 0x80) {
                        if (!in_word) { in_word = true; count++; }
                        i++;
                    } else {
                        uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
                        if (u_is_space(c)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i += m;
                    }
                }
                continue;
            }
        }
        __m256i w0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, vsp),
            _mm256_and_si256(_mm256_cmpgt_epi8(v0, v8), _mm256_cmpgt_epi8(v14, v0)));
        __m256i w1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, vsp),
            _mm256_and_si256(_mm256_cmpgt_epi8(v1, v8), _mm256_cmpgt_epi8(v14, v1)));
        uint64_t ws = (uint32_t)_mm256_movemask_epi8(w0) | ((uint64_t)(uint32_t)_mm256_movemask_epi8(w1) << 32);
        uint64_t nonws = ~ws;
        uint64_t prev = (ws << 1) | (in_word ? 0ULL : 1ULL);
        count += (size_t)__builtin_popcountll(nonws & prev);
        in_word = (ws >> 63) == 0;
        i += 64;
    }
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        if (__builtin_expect(_mm256_movemask_epi8(v) != 0, 0)) {
            __m256i sp = _mm256_or_si256(_mm256_cmpeq_epi8(v, vc2),
                                         _mm256_cmpeq_epi8(_mm256_and_si256(v, maskf0), ve0));
            if (_mm256_movemask_epi8(sp) != 0) {
                size_t end = i + 32;
                while (i < end) {
                    uint8_t c0 = b[i];
                    if (c0 < 0x80) {
                        if (ws_byte(c0)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i++;
                    } else if ((c0 & 0xc0) == 0x80) {
                        if (!in_word) { in_word = true; count++; }
                        i++;
                    } else {
                        uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
                        if (u_is_space(c)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i += m;
                    }
                }
                continue;
            }
        }
        __m256i w = _mm256_or_si256(_mm256_cmpeq_epi8(v, vsp),
            _mm256_and_si256(_mm256_cmpgt_epi8(v, v8), _mm256_cmpgt_epi8(v14, v)));
        uint32_t ws = (uint32_t)_mm256_movemask_epi8(w);
        uint32_t nonws = ~ws;
        uint32_t prev = (ws << 1) | (in_word ? 0u : 1u);
        count += (size_t)__builtin_popcount(nonws & prev);
        in_word = (ws & 0x80000000u) == 0;
        i += 32;
    }
    while (i < n) {
        uint8_t c0 = b[i];
        if (c0 < 0x80) {
            if (ws_byte(c0)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i++;
        } else if ((c0 & 0xc0) == 0x80) {
            if (!in_word) { in_word = true; count++; }
            i++;
        } else {
            uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
            if (u_is_space(c)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i += m;
        }
    }
    return count;
}

static inline unsigned popcount64_sw(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    return (unsigned)((((x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL) * 0x0101010101010101ULL) >> 56);
}

__attribute__((target("popcnt")))
static size_t word_count_sse2_popcnt(const uint8_t *b, size_t n) {
    size_t count = 0, i = 0;
    bool in_word = false;
    __m128i vsp = _mm_set1_epi8(' ');
    __m128i v8 = _mm_set1_epi8(8);
    __m128i v14 = _mm_set1_epi8(14);
    __m128i maskf0 = _mm_set1_epi8((char)0xf0);
    __m128i ve0 = _mm_set1_epi8((char)0xe0);
    __m128i vc2 = _mm_set1_epi8((char)0xc2);
    while (i + 64 <= n) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(b + i + 16));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(b + i + 32));
        __m128i v3 = _mm_loadu_si128((const __m128i *)(b + i + 48));
        if (__builtin_expect(_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3))) != 0, 0)) {
            __m128i sp0 = _mm_or_si128(_mm_cmpeq_epi8(v0, vc2), _mm_cmpeq_epi8(_mm_and_si128(v0, maskf0), ve0));
            __m128i sp1 = _mm_or_si128(_mm_cmpeq_epi8(v1, vc2), _mm_cmpeq_epi8(_mm_and_si128(v1, maskf0), ve0));
            __m128i sp2 = _mm_or_si128(_mm_cmpeq_epi8(v2, vc2), _mm_cmpeq_epi8(_mm_and_si128(v2, maskf0), ve0));
            __m128i sp3 = _mm_or_si128(_mm_cmpeq_epi8(v3, vc2), _mm_cmpeq_epi8(_mm_and_si128(v3, maskf0), ve0));
            if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(sp0, sp1), _mm_or_si128(sp2, sp3))) != 0) {
                size_t end = i + 64;
                while (i < end) {
                    uint8_t c0 = b[i];
                    if (c0 < 0x80) {
                        if (ws_byte(c0)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i++;
                    } else if ((c0 & 0xc0) == 0x80) {
                        if (!in_word) { in_word = true; count++; }
                        i++;
                    } else {
                        uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
                        if (u_is_space(c)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i += m;
                    }
                }
                continue;
            }
        }
        uint64_t m0 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v0, vsp), _mm_and_si128(_mm_cmpgt_epi8(v0, v8), _mm_cmpgt_epi8(v14, v0))));
        uint64_t m1 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v1, vsp), _mm_and_si128(_mm_cmpgt_epi8(v1, v8), _mm_cmpgt_epi8(v14, v1))));
        uint64_t m2 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v2, vsp), _mm_and_si128(_mm_cmpgt_epi8(v2, v8), _mm_cmpgt_epi8(v14, v2))));
        uint64_t m3 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v3, vsp), _mm_and_si128(_mm_cmpgt_epi8(v3, v8), _mm_cmpgt_epi8(v14, v3))));
        uint64_t ws = m0 | (m1 << 16) | (m2 << 32) | (m3 << 48);
        uint64_t prev = (ws << 1) | (in_word ? 0ULL : 1ULL);
        count += (size_t)__builtin_popcountll((~ws) & prev);
        in_word = (ws >> 63) == 0;
        i += 64;
    }
    while (i < n) {
        uint8_t c0 = b[i];
        if (c0 < 0x80) {
            if (ws_byte(c0)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i++;
        } else if ((c0 & 0xc0) == 0x80) {
            if (!in_word) { in_word = true; count++; }
            i++;
        } else {
            uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
            if (u_is_space(c)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i += m;
        }
    }
    return count;
}

size_t word_count(const uint8_t *b, size_t n) {
    if (g_has_avx2 && g_has_popcnt && n >= 32) return word_count_avx2(b, n);
    if (g_has_popcnt && n >= 64) return word_count_sse2_popcnt(b, n);
    size_t count = 0, i = 0;
    bool in_word = false;
    __m128i vsp = _mm_set1_epi8(' ');
    __m128i v8 = _mm_set1_epi8(8);
    __m128i v14 = _mm_set1_epi8(14);
    __m128i maskf0 = _mm_set1_epi8((char)0xf0);
    __m128i ve0 = _mm_set1_epi8((char)0xe0);
    __m128i vc2 = _mm_set1_epi8((char)0xc2);
    while (i + 64 <= n) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(b + i + 16));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(b + i + 32));
        __m128i v3 = _mm_loadu_si128((const __m128i *)(b + i + 48));
        if (__builtin_expect(_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(v0, v1), _mm_or_si128(v2, v3))) != 0, 0)) {
            __m128i sp0 = _mm_or_si128(_mm_cmpeq_epi8(v0, vc2), _mm_cmpeq_epi8(_mm_and_si128(v0, maskf0), ve0));
            __m128i sp1 = _mm_or_si128(_mm_cmpeq_epi8(v1, vc2), _mm_cmpeq_epi8(_mm_and_si128(v1, maskf0), ve0));
            __m128i sp2 = _mm_or_si128(_mm_cmpeq_epi8(v2, vc2), _mm_cmpeq_epi8(_mm_and_si128(v2, maskf0), ve0));
            __m128i sp3 = _mm_or_si128(_mm_cmpeq_epi8(v3, vc2), _mm_cmpeq_epi8(_mm_and_si128(v3, maskf0), ve0));
            if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(sp0, sp1), _mm_or_si128(sp2, sp3))) != 0) {
                size_t end = i + 64;
                while (i < end) {
                    uint8_t c0 = b[i];
                    if (c0 < 0x80) {
                        if (ws_byte(c0)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i++;
                    } else if ((c0 & 0xc0) == 0x80) {
                        if (!in_word) { in_word = true; count++; }
                        i++;
                    } else {
                        uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
                        if (u_is_space(c)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i += m;
                    }
                }
                continue;
            }
        }
        uint64_t m0 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v0, vsp), _mm_and_si128(_mm_cmpgt_epi8(v0, v8), _mm_cmpgt_epi8(v14, v0))));
        uint64_t m1 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v1, vsp), _mm_and_si128(_mm_cmpgt_epi8(v1, v8), _mm_cmpgt_epi8(v14, v1))));
        uint64_t m2 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v2, vsp), _mm_and_si128(_mm_cmpgt_epi8(v2, v8), _mm_cmpgt_epi8(v14, v2))));
        uint64_t m3 = (uint16_t)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v3, vsp), _mm_and_si128(_mm_cmpgt_epi8(v3, v8), _mm_cmpgt_epi8(v14, v3))));
        uint64_t ws = m0 | (m1 << 16) | (m2 << 32) | (m3 << 48);
        uint64_t prev = (ws << 1) | (in_word ? 0ULL : 1ULL);
        count += (size_t)popcount64_sw((~ws) & prev);
        in_word = (ws >> 63) == 0;
        i += 64;
    }
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        if (__builtin_expect(_mm_movemask_epi8(v) != 0, 0)) {
            __m128i sp = _mm_or_si128(_mm_cmpeq_epi8(v, vc2),
                                      _mm_cmpeq_epi8(_mm_and_si128(v, maskf0), ve0));
            if (_mm_movemask_epi8(sp) != 0) {
                size_t end = i + 16;
                while (i < end) {
                    uint8_t c0 = b[i];
                    if (c0 < 0x80) {
                        if (ws_byte(c0)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i++;
                    } else if ((c0 & 0xc0) == 0x80) {
                        if (!in_word) { in_word = true; count++; }
                        i++;
                    } else {
                        uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
                        if (u_is_space(c)) in_word = false;
                        else if (!in_word) { in_word = true; count++; }
                        i += m;
                    }
                }
                continue;
            }
        }
        __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, vsp),
            _mm_and_si128(_mm_cmpgt_epi8(v, v8), _mm_cmpgt_epi8(v14, v)));
        uint16_t ws = (uint16_t)_mm_movemask_epi8(m);
        uint16_t nonws = (uint16_t)~ws;
        uint16_t prev = (uint16_t)((ws << 1) | (in_word ? 0 : 1));
        count += (size_t)popcount16((uint16_t)(nonws & prev));
        in_word = (ws & 0x8000u) == 0;
        i += 16;
    }
    while (i < n) {
        uint8_t c0 = b[i];
        if (c0 < 0x80) {
            if (ws_byte(c0)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i++;
        } else if ((c0 & 0xc0) == 0x80) {
            if (!in_word) { in_word = true; count++; }
            i++;
        } else {
            uint32_t c; size_t m = (size_t)u8_decode(b, n, i, &c);
            if (u_is_space(c)) in_word = false;
            else if (!in_word) { in_word = true; count++; }
            i += m;
        }
    }
    return count;
}

void json_escape_into(Pool *p, const uint8_t *bytes, size_t n) {
    static const char *HEX = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        int j = scan_json_esc(bytes, n, i);
        if (j < 0) { pool_put(p, bytes + i, n - i); return; }
        pool_put(p, bytes + i, (size_t)j - i);
        uint8_t c = bytes[j];
        switch (c) {
            case '"': pool_put(p, (const uint8_t *)"\\\"", 2); break;
            case '\\': pool_put(p, (const uint8_t *)"\\\\", 2); break;
            case '\n': pool_put(p, (const uint8_t *)"\\n", 2); break;
            case '\r': pool_put(p, (const uint8_t *)"\\r", 2); break;
            case '\t': pool_put(p, (const uint8_t *)"\\t", 2); break;
            case 0x08: pool_put(p, (const uint8_t *)"\\b", 2); break;
            case 0x0c: pool_put(p, (const uint8_t *)"\\f", 2); break;
            default:
                pool_put(p, (const uint8_t *)"\\u00", 4);
                pool_u8(p, (uint8_t)HEX[(c >> 4) & 0xf]);
                pool_u8(p, (uint8_t)HEX[c & 0xf]);
                break;
        }
        i = (size_t)j + 1;
    }
}

__attribute__((target("avx2,bmi")))
static void html_json_escape_into_avx2(Pool *p, const uint8_t *bytes, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    static const uint8_t TBL[256] = {
        [0 ... 0x1f] = 1,
        ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
    };
    __m256i lo_lut = _mm256_setr_epi8(
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8,
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
    __m256i hi_lut = _mm256_setr_epi8(
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    uint8_t *dst = p->base + p->len;
    size_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(bytes + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        __m256i hit = _mm256_and_si256(lo, hi);
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        _mm256_storeu_si256((__m256i *)dst, v);
        if (m == 0) {
            dst += 32; i += 32;
            continue;
        }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        uint8_t c = bytes[i++];
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(bytes + i));
        __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
        __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                      _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
        __m128i hit = _mm_and_si128(lo, hi);
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu;
        _mm_storeu_si128((__m128i *)dst, v);
        if (m == 0) {
            dst += 16; i += 16;
            continue;
        }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        uint8_t c = bytes[i++];
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    while (i < n) {
        uint8_t c = bytes[i++];
        if (!TBL[c]) { *dst++ = c; continue; }
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    p->len = (size_t)(dst - p->base);
}

__attribute__((target("ssse3")))
static void html_json_escape_into_ssse3(Pool *p, const uint8_t *bytes, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    static const uint8_t TBL[256] = {
        [0 ... 0x1f] = 1,
        ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
    };
    __m128i lo_lut = _mm_setr_epi8(8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
    __m128i hi_lut = _mm_setr_epi8(8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m128i mask0f = _mm_set1_epi8(0x0f);
    __m128i zero = _mm_setzero_si128();
    uint8_t *dst = p->base + p->len;
    size_t i = 0;
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(bytes + i));
        __m128i lo = _mm_shuffle_epi8(lo_lut, v);
        __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), zero))) ^ 0xffffu;
        _mm_storeu_si128((__m128i *)dst, v);
        if (m == 0) { dst += 16; i += 16; continue; }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        uint8_t c = bytes[i++];
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    while (i < n) {
        uint8_t c = bytes[i++];
        if (!TBL[c]) { *dst++ = c; continue; }
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    p->len = (size_t)(dst - p->base);
}

static void html_json_escape_into_sse2(Pool *p, const uint8_t *bytes, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    static const uint8_t TBL[256] = {
        [0 ... 0x1f] = 1,
        ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
    };
    __m128i c1f = _mm_set1_epi8(0x1f);
    __m128i mfe = _mm_set1_epi8((char)0xfe);
    __m128i v26 = _mm_set1_epi8(0x26);
    __m128i mfd = _mm_set1_epi8((char)0xfd);
    __m128i v3c = _mm_set1_epi8(0x3c);
    __m128i vq = _mm_set1_epi8('"');
    __m128i vbs = _mm_set1_epi8('\\');
    uint8_t *dst = p->base + p->len;
    size_t i = 0;
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(bytes + i));
        __m128i h1 = _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v),
                                  _mm_cmpeq_epi8(_mm_and_si128(v, mfe), v26));
        __m128i h2 = _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(v, mfd), v3c),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h1, h2));
        _mm_storeu_si128((__m128i *)dst, v);
        if (m == 0) { dst += 16; i += 16; continue; }
        size_t k = (size_t)__builtin_ctz(m);
        dst += k; i += k;
        uint8_t c = bytes[i++];
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    while (i < n) {
        uint8_t c = bytes[i++];
        if (!TBL[c]) { *dst++ = c; continue; }
        switch (c) {
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; break;
            case '\\': dst[0] = '\\'; dst[1] = '\\'; dst += 2; break;
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                break;
        }
    }
    p->len = (size_t)(dst - p->base);
}

void html_json_escape_into(Pool *p, const uint8_t *bytes, size_t n) {
    if (n == 0) return;
    if (__builtin_expect(p->fd >= 0 && p->len + n * 6 + 64 > p->cap, 0)) {
        out_flush(p);
    }
    if (__builtin_expect(p->len + n * 6 + 64 <= p->cap, 1)) {
        if (g_has_avx2) { html_json_escape_into_avx2(p, bytes, n); return; }
        if (g_has_ssse3) { html_json_escape_into_ssse3(p, bytes, n); return; }
        html_json_escape_into_sse2(p, bytes, n);
        return;
    }
    static const char *HEX = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        int j = scan_html_json_esc(bytes, n, i);
        if (j < 0) { pool_put(p, bytes + i, n - i); return; }
        pool_put(p, bytes + i, (size_t)j - i);
        uint8_t c = bytes[j];
        switch (c) {
            case '&': pool_put(p, (const uint8_t *)"&amp;", 5); break;
            case '<': pool_put(p, (const uint8_t *)"&lt;", 4); break;
            case '>': pool_put(p, (const uint8_t *)"&gt;", 4); break;
            case '"': pool_put(p, (const uint8_t *)"&quot;", 6); break;
            case '\'': pool_put(p, (const uint8_t *)"&#39;", 5); break;
            case '\\': pool_put(p, (const uint8_t *)"\\\\", 2); break;
            case '\n': pool_put(p, (const uint8_t *)"\\n", 2); break;
            case '\r': pool_put(p, (const uint8_t *)"\\r", 2); break;
            case '\t': pool_put(p, (const uint8_t *)"\\t", 2); break;
            case 0x08: pool_put(p, (const uint8_t *)"\\b", 2); break;
            case 0x0c: pool_put(p, (const uint8_t *)"\\f", 2); break;
            default:
                pool_put(p, (const uint8_t *)"\\u00", 4);
                pool_u8(p, (uint8_t)HEX[(c >> 4) & 0xf]);
                pool_u8(p, (uint8_t)HEX[c & 0xf]);
                break;
        }
        i = (size_t)j + 1;
    }
}
