/* txt.c — UTF-8, whitespace, UTF-16 order, NFKD slugify, word count,
 * long dates, JSON/HTML escaping. Follows the reference semantics
 * exactly (including the lenient UTF-8 decoder, which masks and
 * combines without validating continuation bytes or ranges). */
#include "zg.h"
#include "decomp.h"

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

Sp slug_make(Pool *p, Sp text, Dict *cache) {
    const uint8_t *bytes = pool_at(p, text);
    Sp hit;
    if (dict_get(cache, p, bytes, text.len, &hit)) return hit;
    uint32_t start = pool_mark(p);
    bool gap = false;
    bool ascii = true;
    for (uint32_t i = 0; i < text.len; i++) if (bytes[i] >= 0x80) { ascii = false; break; }
    if (ascii) {
        for (uint32_t i = 0; i < text.len; i++) {
            uint8_t b = bytes[i];
            int keep = -1;
            if (b >= 'A' && b <= 'Z') keep = b + 32;
            else if ((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9')) keep = b;
            if (keep >= 0) {
                if (gap) pool_u8(p, '-');
                gap = false;
                pool_u8(p, (uint8_t)keep);
            } else {
                gap = p->len > start;
            }
        }
    } else {
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

size_t word_count(const uint8_t *b, size_t n) {
    size_t count = 0, i = 0;
    bool in_word = false;
    /* SSE2 fast path: 16 bytes at a time while the chunk is all ASCII.
     * A word starts at a non-whitespace byte whose predecessor is
     * whitespace (or the start of input). Non-ASCII chunks fall back to
     * the scalar loop for that chunk only, then SIMD resumes. */
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        if (_mm_movemask_epi8(v) != 0) {
            size_t end = i + 16;
            while (i < end) {
                uint8_t c0 = b[i];
                if (c0 < 0x80) {
                    if (ws_byte(c0)) in_word = false;
                    else if (!in_word) { in_word = true; count++; }
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
        __m128i m = _mm_setzero_si128();
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8(' ')));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\t')));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\n')));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\v')));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\f')));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\r')));
        uint16_t ws = (uint16_t)_mm_movemask_epi8(m);
        uint16_t nonws = (uint16_t)~ws;
        /* bit j set iff byte j is non-ws and byte j-1 is ws; bit 0 uses
         * the carried state (1 when not inside a word). */
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

void html_json_escape_into(Pool *p, const uint8_t *bytes, size_t n) {
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
