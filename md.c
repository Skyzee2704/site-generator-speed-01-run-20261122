/* md.c — the Markdown subset, lexed into blocks and rendered, in the
 * shape of marked's lexer and parser. Block rules: fenced code, ATX
 * headings (h2/h3 feed the TOC), thematic breaks, blockquotes (one
 * paragraph), lists, paragraphs. Inline: escapes, code spans, strong,
 * emphasis, links with path resolution. */
#include "zg.h"
#include <immintrin.h>

static inline int line_nl(Pool *p, size_t from, size_t end) {
    return scan_nl(p->base, end, from);
}

static inline bool line_blank(Pool *p, size_t off, size_t end) {
    const uint8_t *b = p->base;
    for (size_t i = off; i < end; i++) {
        uint8_t c = b[i];
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

static inline void line_trim(Pool *p, size_t off, size_t end, size_t *s, size_t *e) {
    const uint8_t *b = p->base;
    size_t a = off, z = end;
    while (a < z && (b[a] == ' ' || b[a] == '\t')) a++;
    while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t')) z--;
    *s = a; *e = z;
}

/* FENCE_OPEN = /^```([A-Za-z0-9_+-]*)[ \t]*$/ — returns the lang span. */
static bool fence_open_at(Pool *p, size_t off, size_t end, size_t *loff, size_t *llen) {
    const uint8_t *b = p->base;
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

static bool fence_close_at(Pool *p, size_t off, size_t end) {
    const uint8_t *b = p->base;
    if (end - off < 3 || b[off] != '`' || b[off + 1] != '`' || b[off + 2] != '`') return false;
    for (size_t k = off + 3; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    return true;
}

/* HEADING = /^(#{1,6})[ \t]+(.*?)[ \t]*$/ */
static bool heading_at(Pool *p, size_t off, size_t end, size_t *depth, size_t *toff, size_t *tlen) {
    const uint8_t *b = p->base;
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

static bool hr_at(Pool *p, size_t off, size_t end) {
    const uint8_t *b = p->base;
    if (end - off < 3) return false;
    uint8_t c = b[off];
    if ((c != '-' && c != '*') || b[off + 1] != c || b[off + 2] != c) return false;
    for (size_t k = off + 3; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    return true;
}

static inline bool para_ends(Pool *p, size_t off, size_t end) {
    if (off >= end) return true;
    uint8_t c0 = p->base[off];
    if (c0 != ' ' && c0 != '\t' && c0 != '`' && c0 != '#' && c0 != '-' && c0 != '*') return false;
    size_t d, t, l;
    if (c0 == ' ' || c0 == '\t') return line_blank(p, off, end);
    if (c0 == '`') return fence_open_at(p, off, end, &t, &l);
    if (c0 == '#') return heading_at(p, off, end, &d, &t, &l);
    return hr_at(p, off, end);
}

/* A list item's start: bullet or ordered. Returns trimmed span [ts..te). */
static bool item_start_span(Pool *p, size_t off, size_t end, bool *ordered, int64_t *num, size_t *ts_out, size_t *te_out) {
    const uint8_t *b = p->base;
    if (off >= end) return false;
    uint8_t c0 = b[off];
    if ((c0 == '-' || c0 == '*') && off + 1 < end && b[off + 1] == ' ') {
        line_trim(p, off + 2, end, ts_out, te_out);
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
    line_trim(p, i + 2, end, ts_out, te_out);
    *ordered = true; *num = n;
    return true;
}

/* CONTINUED = /^ {2,}\S/ */
static bool continued_at(Pool *p, size_t off, size_t end) {
    size_t i = off, spaces = 0;
    while (i < end && p->base[i] == ' ') { spaces++; i++; }
    if (spaces < 2 || i >= end) return false;
    uint8_t c = p->base[i];
    return !(c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r');
}

/* HTML-escape a byte span into a Str: & < > " '. */
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

/* Fused HTML + JSON escape into a Str (AVX2 speculative-store fast path). */
__attribute__((target("avx2,bmi")))
static void esc_html_json_into_str_avx2(const uint8_t *b, size_t n, Str *out) {
    static const char HEX[] = "0123456789abcdef";
    static const uint8_t TBL[256] = {
        [0 ... 0x1f] = 1,
        ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
    };
    if (out->len + n + 64 > out->cap) str_grow(out, out->len + n + 64);
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
            str_grow(out, out->len + (n - i) + 128);
            dst = out->data + out->len;
            dst_end = out->data + out->cap;
        }
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
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
        uint8_t c = b[i++];
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
        if (__builtin_expect(dst + 64 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - i) + 128);
            dst = out->data + out->len;
            dst_end = out->data + out->cap;
        }
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
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
        uint8_t c = b[i++];
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
        if (__builtin_expect(dst + 16 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - i) * 6 + 64);
            dst = out->data + out->len;
            dst_end = out->data + out->cap;
        }
        uint8_t c = b[i++];
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
    out->len = (size_t)(dst - out->data);
}

/* Fused HTML + JSON escape into a Str. */
static void esc_html_json_into_str(const uint8_t *b, size_t n, Str *out) {
    if (n == 0) return;
    if (g_has_avx2) { esc_html_json_into_str_avx2(b, n, out); return; }
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        int j = scan_html_json_esc(b, n, i);
        if (j < 0) { str_add(out, b + i, n - i); return; }
        str_add(out, b + i, (size_t)j - i);
        uint8_t c = b[j];
        switch (c) {
            case '&': str_lit(out, "&amp;"); break;
            case '<': str_lit(out, "&lt;"); break;
            case '>': str_lit(out, "&gt;"); break;
            case '"': str_lit(out, "&quot;"); break;
            case '\'': str_lit(out, "&#39;"); break;
            case '\\': str_lit(out, "\\\\"); break;
            case '\n': str_lit(out, "\\n"); break;
            case '\r': str_lit(out, "\\r"); break;
            case '\t': str_lit(out, "\\t"); break;
            case 0x08: str_lit(out, "\\b"); break;
            case 0x0c: str_lit(out, "\\f"); break;
            default:
                str_lit(out, "\\u00");
                str_c(out, (uint8_t)HEX[(c >> 4) & 0xf]);
                str_c(out, (uint8_t)HEX[c & 0xf]);
                break;
        }
        i = (size_t)j + 1;
    }
}

/* ---------------- inline ---------------- */
static inline bool is_escapable(uint8_t c) {
    switch (c) {
        case '\\': case '`': case '*': case '_': case '[': case ']':
        case '(': case ')': case '#': case '+': case '-': case '.':
        case '!': case '>': return true;
        default: return false;
    }
}

/* Find `what` (1 or 2 bytes) from `from`, skipping `\`+escapable pairs. */
static int find_unesc(const uint8_t *s, size_t n, const uint8_t *what, size_t wlen, size_t from) {
    uint8_t first = what[0];
    size_t pos = from;
    for (;;) {
        int j = scan_two(s, n, pos, '\\', first);
        if (j < 0) return -1;
        if (s[j] == '\\') {
            if ((size_t)j + 1 < n && is_escapable(s[j + 1])) pos = (size_t)j + 2;
            else pos = (size_t)j + 1;
            continue;
        }
        if (wlen == 1) return j;
        if ((size_t)j + 1 < n && s[j + 1] == what[1]) return j;
        pos = (size_t)j + 1;
    }
}

/* Find a lone `*` (not part of `**`, not escaped). */
static int find_lone_star(const uint8_t *s, size_t n, size_t from) {
    size_t pos = from;
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

/* resolvePath(dir, rel) into stack buffer when small, else out; false when it climbs out of the root. */
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

static const uint8_t *resolve_href(Pool *p, LinkCtx *env, const uint8_t *target, size_t tn,
                                   Str *tmp, size_t *href_len) {
    if ((tn > 0 && (target[0] == '/' || target[0] == '#')) || is_scheme_span(target, tn)) {
        *href_len = tn;
        return target;
    }
    size_t hash = tn;
    for (size_t i = 0; i < tn; i++) if (target[i] == '#') { hash = i; break; }
    size_t rl = hash, fl = tn - hash;
    const uint8_t *frag = target + hash;
    if (rl == 0) {
        note_broken(p, env, target, tn);
        *href_len = tn;
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
                return pool_at(p, pg->url);
            }
            str_clear(tmp);
            str_add(tmp, pool_at(p, pg->url), pg->url.len);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            return tmp->data;
        }
        if (static_lookup(env, p, rpath, rlen)) {
            str_clear(tmp);
            str_add(tmp, pool_at(p, env->base), env->base.len);
            str_c(tmp, '/');
            str_add(tmp, rpath, rlen);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            return tmp->data;
        }
    }
    note_broken(p, env, target, tn);
    *href_len = tn;
    return target;
}

static void inline_span(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    size_t i = 0;
    while (i < n) {
        int j = scan_inline_md(s, n, i);
        if (j < 0) {
            str_add(out, s + i, n - i); return;
        }
        str_add(out, s + i, (size_t)j - i);
        uint8_t c = s[j];
        switch (c) {
            case '\\': {
                if ((size_t)j + 1 < n && is_escapable(s[j + 1])) {
                    uint8_t e = s[j + 1];
                    if (e == '>') str_lit(out, "&gt;");
                    else str_c(out, e);
                    i = (size_t)j + 2;
                } else {
                    str_c(out, '\\');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '`': {
                int k = scan_byte(s, n, (size_t)j + 1, '`');
                if (k >= 0 && (size_t)k > (size_t)j + 1) {
                    str_lit(out, "<code>");
                    esc_html_into(s + j + 1, (size_t)k - ((size_t)j + 1), out);
                    str_lit(out, "</code>");
                    i = (size_t)k + 1;
                } else {
                    str_c(out, '`');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '*': {
                if ((size_t)j + 1 < n && s[j + 1] == '*') {
                    static const uint8_t star2[2] = {'*', '*'};
                    int k = find_unesc(s, n, star2, 2, (size_t)j + 2);
                    if (k > (int)(j + 2)) {
                        str_lit(out, "<strong>");
                        inline_span(p, env, s + j + 2, (size_t)k - ((size_t)j + 2), out, tmp);
                        str_lit(out, "</strong>");
                        i = (size_t)k + 2;
                    } else {
                        str_lit(out, "**");
                        i = (size_t)j + 2;
                    }
                } else {
                    int k = find_lone_star(s, n, (size_t)j + 1);
                    if (k > (int)(j + 1)) {
                        str_lit(out, "<em>");
                        inline_span(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                        str_lit(out, "</em>");
                        i = (size_t)k + 1;
                    } else {
                        str_c(out, '*');
                        i = (size_t)j + 1;
                    }
                }
                break;
            }
            case '[': {
                static const uint8_t rb[1] = {']'};
                int k = find_unesc(s, n, rb, 1, (size_t)j + 1);
                int m = -1;
                if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                    int q = scan_byte(s, n, (size_t)k + 2, ')');
                    if (q >= 0) m = q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t href_len;
                    const uint8_t *href = resolve_href(p, env, target, tl, tmp, &href_len);
                    str_lit(out, "<a href=\"");
                    esc_html_into(href, href_len, out);
                    str_lit(out, "\">");
                    inline_span(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                    str_lit(out, "</a>");
                    i = (size_t)m + 1;
                } else {
                    str_c(out, '[');
                    i = (size_t)j + 1;
                }
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

__attribute__((target("avx2,bmi")))
static inline int scan_two_avx2_i(const uint8_t *b, size_t n, size_t i, uint8_t c1, uint8_t c2) {
    if (i + 32 <= n) {
        __m256i v1 = _mm256_set1_epi8((char)c1);
        __m256i v2 = _mm256_set1_epi8((char)c2);
        do {
            __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
            uint32_t m = (uint32_t)_mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, v1), _mm256_cmpeq_epi8(v, v2)));
            if (m) return (int)(i + (size_t)__builtin_ctz(m));
            i += 32;
        } while (i + 32 <= n);
    }
    if (i + 16 <= n) {
        __m128i v1 = _mm_set1_epi8((char)c1);
        __m128i v2 = _mm_set1_epi8((char)c2);
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(
            _mm_or_si128(_mm_cmpeq_epi8(v, v1), _mm_cmpeq_epi8(v, v2)));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c1 || b[i] == c2) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static inline int find_unesc_avx2(const uint8_t *s, size_t n, uint8_t w0, uint8_t w1, size_t wlen, size_t pos) {
    for (;;) {
        int j = scan_two_avx2_i(s, n, pos, '\\', w0);
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

__attribute__((target("avx2,bmi")))
static inline int find_lone_star_avx2(const uint8_t *s, size_t n, size_t pos) {
    for (;;) {
        int j = scan_two_avx2_i(s, n, pos, '\\', '*');
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

__attribute__((target("avx2,bmi")))
static void inline_span_json_avx2(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    static const char HEX[] = "0123456789abcdef";
    static const uint8_t TBL[256] = {
        [0 ... 0x1f] = 1,
        ['\\'] = 1, ['`'] = 1, ['['] = 1, ['*'] = 1,
        ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
    };
    if (__builtin_expect(out->len + n + 64 > out->cap, 0)) str_grow(out, out->len + n + 128);
    uint8_t *dst = out->data + out->len;
    uint8_t *dst_end = out->data + out->cap;
    size_t i = 0;
    __m256i lo_lut, hi_lut, mask0f, zero;
    if (n >= 16) {
        lo_lut = _mm256_setr_epi8(
            17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1,
            17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1);
        hi_lut = _mm256_setr_epi8(
            1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        mask0f = _mm256_set1_epi8(0x0f);
        zero = _mm256_setzero_si256();
    }
    while (i < n) {
        /* Tight SIMD scan+copy while invariant (dst_end - dst >= n - i + 64) holds */
        size_t j = i;
        while (j + 32 <= n) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(s + j));
            __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
            __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
            uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), zero));
            _mm256_storeu_si256((__m256i *)dst, v);
            if (__builtin_expect(m == 0, 1)) {
                dst += 32; j += 32;
                continue;
            }
            size_t k = (size_t)__builtin_ctz(m);
            dst += k; j += k;
            goto hit;
        }
        if (j + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
            __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
            __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                          _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
            uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), _mm_setzero_si128()))) ^ 0xffffu;
            _mm_storeu_si128((__m128i *)dst, v);
            if (m == 0) {
                dst += 16; j += 16;
            } else {
                size_t k = (size_t)__builtin_ctz(m);
                dst += k; j += k;
                goto hit;
            }
        }
        while (j < n && !TBL[s[j]]) { *dst++ = s[j++]; }
        if (j >= n) break;
    hit:
        if (__builtin_expect(dst + (n - j) + 64 > dst_end, 0)) {
            out->len = (size_t)(dst - out->data);
            str_grow(out, out->len + (n - j) + 256);
            dst = out->data + out->len;
            dst_end = out->data + out->cap;
        }
        uint8_t c = s[j];
        switch (c) {
            case '\n': dst[0] = '\\'; dst[1] = 'n'; dst += 2; i = j + 1; break;
            case '\\': {
                if (j + 1 < n && is_escapable(s[j + 1])) {
                    uint8_t e = s[j + 1];
                    if (e == '>') { __builtin_memcpy(dst, "&gt;", 4); dst += 4; }
                    else if (e == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
                    else { *dst++ = e; }
                    i = j + 2;
                } else {
                    dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                    i = j + 1;
                }
                break;
            }
            case '`': {
                int k = scan_byte_avx2_i(s, n, j + 1, '`');
                if (k >= 0 && (size_t)k > j + 1) {
                    __builtin_memcpy(dst, "<code>", 6); dst += 6;
                    out->len = (size_t)(dst - out->data);
                    esc_html_json_into_str_avx2(s + j + 1, (size_t)k - (j + 1), out);
                    if (__builtin_expect(out->len + (n - (size_t)k) + 64 > out->cap, 0))
                        str_grow(out, out->len + (n - (size_t)k) + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                    __builtin_memcpy(dst, "</code>", 7); dst += 7;
                    i = (size_t)k + 1;
                } else {
                    *dst++ = '`';
                    i = j + 1;
                }
                break;
            }
            case '*': {
                if (j + 1 < n && s[j + 1] == '*') {
                    int k = find_unesc_avx2(s, n, '*', '*', 2, j + 2);
                    if (k > (int)(j + 2)) {
                        __builtin_memcpy(dst, "<strong>", 8); dst += 8;
                        out->len = (size_t)(dst - out->data);
                        inline_span_json_avx2(p, env, s + j + 2, (size_t)k - (j + 2), out, tmp);
                        if (__builtin_expect(out->len + (n - (size_t)k) + 64 > out->cap, 0))
                            str_grow(out, out->len + (n - (size_t)k) + 256);
                        dst = out->data + out->len; dst_end = out->data + out->cap;
                        __builtin_memcpy(dst, "</strong>", 9); dst += 9;
                        i = (size_t)k + 2;
                    } else {
                        dst[0] = '*'; dst[1] = '*'; dst += 2;
                        i = j + 2;
                    }
                } else {
                    int k = find_lone_star_avx2(s, n, j + 1);
                    if (k > (int)(j + 1)) {
                        __builtin_memcpy(dst, "<em>", 4); dst += 4;
                        out->len = (size_t)(dst - out->data);
                        inline_span_json_avx2(p, env, s + j + 1, (size_t)k - (j + 1), out, tmp);
                        if (__builtin_expect(out->len + (n - (size_t)k) + 64 > out->cap, 0))
                            str_grow(out, out->len + (n - (size_t)k) + 256);
                        dst = out->data + out->len; dst_end = out->data + out->cap;
                        __builtin_memcpy(dst, "</em>", 5); dst += 5;
                        i = (size_t)k + 1;
                    } else {
                        *dst++ = '*';
                        i = j + 1;
                    }
                }
                break;
            }
            case '[': {
                int k = find_unesc_avx2(s, n, ']', 0, 1, j + 1);
                int m = -1;
                if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                    int q = scan_byte_avx2_i(s, n, (size_t)k + 2, ')');
                    if (q >= 0) m = q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t href_len;
                    const uint8_t *href = resolve_href(p, env, target, tl, tmp, &href_len);
                    __builtin_memcpy(dst, "<a href=\\\"", 10); dst += 10;
                    out->len = (size_t)(dst - out->data);
                    esc_html_json_into_str_avx2(href, href_len, out);
                    if (__builtin_expect(out->len + 16 > out->cap, 0)) str_grow(out, out->len + 64);
                    dst = out->data + out->len;
                    __builtin_memcpy(dst, "\\\">", 3); dst += 3;
                    out->len = (size_t)(dst - out->data);
                    inline_span_json_avx2(p, env, s + j + 1, (size_t)k - (j + 1), out, tmp);
                    if (__builtin_expect(out->len + (n - (size_t)m) + 64 > out->cap, 0))
                        str_grow(out, out->len + (n - (size_t)m) + 256);
                    dst = out->data + out->len; dst_end = out->data + out->cap;
                    __builtin_memcpy(dst, "</a>", 4); dst += 4;
                    i = (size_t)m + 1;
                } else {
                    *dst++ = '[';
                    i = j + 1;
                }
                break;
            }
            case '&': __builtin_memcpy(dst, "&amp;", 5); dst += 5; i = j + 1; break;
            case '<': __builtin_memcpy(dst, "&lt;", 4); dst += 4; i = j + 1; break;
            case '>': __builtin_memcpy(dst, "&gt;", 4); dst += 4; i = j + 1; break;
            case '"': __builtin_memcpy(dst, "&quot;", 6); dst += 6; i = j + 1; break;
            case '\'': __builtin_memcpy(dst, "&#39;", 5); dst += 5; i = j + 1; break;
            case '\r': dst[0] = '\\'; dst[1] = 'r'; dst += 2; i = j + 1; break;
            case '\t': dst[0] = '\\'; dst[1] = 't'; dst += 2; i = j + 1; break;
            case 0x08: dst[0] = '\\'; dst[1] = 'b'; dst += 2; i = j + 1; break;
            case 0x0c: dst[0] = '\\'; dst[1] = 'f'; dst += 2; i = j + 1; break;
            default:
                __builtin_memcpy(dst, "\\u00", 4);
                dst[4] = (uint8_t)HEX[(c >> 4) & 0xf];
                dst[5] = (uint8_t)HEX[c & 0xf];
                dst += 6;
                i = j + 1;
                break;
        }
    }
    out->len = (size_t)(dst - out->data);
}

static void inline_span_json(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    if (n == 0) return;
    if (g_has_avx2) { inline_span_json_avx2(p, env, s, n, out, tmp); return; }
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        int j = scan_inline_md_json(s, n, i);
        if (j < 0) {
            str_add(out, s + i, n - i); return;
        }
        str_add(out, s + i, (size_t)j - i);
        uint8_t c = s[j];
        switch (c) {
            case '\\': {
                if ((size_t)j + 1 < n && is_escapable(s[j + 1])) {
                    uint8_t e = s[j + 1];
                    if (e == '>') str_lit(out, "&gt;");
                    else if (e == '\\') str_lit(out, "\\\\");
                    else str_c(out, e);
                    i = (size_t)j + 2;
                } else {
                    str_lit(out, "\\\\");
                    i = (size_t)j + 1;
                }
                break;
            }
            case '`': {
                int k = scan_byte(s, n, (size_t)j + 1, '`');
                if (k >= 0 && (size_t)k > (size_t)j + 1) {
                    str_lit(out, "<code>");
                    esc_html_json_into_str(s + j + 1, (size_t)k - ((size_t)j + 1), out);
                    str_lit(out, "</code>");
                    i = (size_t)k + 1;
                } else {
                    str_c(out, '`');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '*': {
                if ((size_t)j + 1 < n && s[j + 1] == '*') {
                    static const uint8_t star2[2] = {'*', '*'};
                    int k = find_unesc(s, n, star2, 2, (size_t)j + 2);
                    if (k > (int)(j + 2)) {
                        str_lit(out, "<strong>");
                        inline_span_json(p, env, s + j + 2, (size_t)k - ((size_t)j + 2), out, tmp);
                        str_lit(out, "</strong>");
                        i = (size_t)k + 2;
                    } else {
                        str_lit(out, "**");
                        i = (size_t)j + 2;
                    }
                } else {
                    int k = find_lone_star(s, n, (size_t)j + 1);
                    if (k > (int)(j + 1)) {
                        str_lit(out, "<em>");
                        inline_span_json(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                        str_lit(out, "</em>");
                        i = (size_t)k + 1;
                    } else {
                        str_c(out, '*');
                        i = (size_t)j + 1;
                    }
                }
                break;
            }
            case '[': {
                static const uint8_t rb[1] = {']'};
                int k = find_unesc(s, n, rb, 1, (size_t)j + 1);
                int m = -1;
                if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                    int q = scan_byte(s, n, (size_t)k + 2, ')');
                    if (q >= 0) m = q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t href_len;
                    const uint8_t *href = resolve_href(p, env, target, tl, tmp, &href_len);
                    str_lit(out, "<a href=\\\"");
                    esc_html_json_into_str(href, href_len, out);
                    str_lit(out, "\\\">");
                    inline_span_json(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                    str_lit(out, "</a>");
                    i = (size_t)m + 1;
                } else {
                    str_c(out, '[');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '&': str_lit(out, "&amp;"); i = (size_t)j + 1; break;
            case '<': str_lit(out, "&lt;"); i = (size_t)j + 1; break;
            case '>': str_lit(out, "&gt;"); i = (size_t)j + 1; break;
            case '"': str_lit(out, "&quot;"); i = (size_t)j + 1; break;
            case '\'': str_lit(out, "&#39;"); i = (size_t)j + 1; break;
            case '\n': str_lit(out, "\\n"); i = (size_t)j + 1; break;
            case '\r': str_lit(out, "\\r"); i = (size_t)j + 1; break;
            case '\t': str_lit(out, "\\t"); i = (size_t)j + 1; break;
            case 0x08: str_lit(out, "\\b"); i = (size_t)j + 1; break;
            case 0x0c: str_lit(out, "\\f"); i = (size_t)j + 1; break;
            default:
                str_lit(out, "\\u00");
                str_c(out, (uint8_t)HEX[(c >> 4) & 0xf]);
                str_c(out, (uint8_t)HEX[c & 0xf]);
                i = (size_t)j + 1;
                break;
        }
    }
}

__attribute__((target("avx2,bmi")))
static bool md_build_json_avx2(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
                               Str *scratch, Str *tmp, Str *text_tmp,
                               Sp *html_out, Arr *toc_out, Sp *excerpt_out) {
    str_clear(scratch);
    size_t off = src.off, len = src.len, end = off + len;
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    Set ids; set_new(&ids);
    size_t i = off;
    Str *item_scratch = env->item_scratch;
    while (i < end) {
        int nl = scan_byte_avx2_i(p->base, end, i, '\n');
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (i == le) { i = le + 1; continue; }
        uint8_t c0 = p->base[i];
        if ((c0 == ' ' || c0 == '\t') && line_blank(p, i, le)) { i = le + 1; continue; }
        size_t loff, llen;
        if (c0 == '`' && fence_open_at(p, i, le, &loff, &llen)) {
            str_lit(scratch, "<pre><code");
            if (llen > 0) {
                str_lit(scratch, " class=\\\"language-");
                str_add(scratch, p->base + loff, llen);
                str_lit(scratch, "\\\"");
            }
            str_c(scratch, '>');
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = scan_byte_avx2_i(p->base, end, i, '\n');
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (i < le2 && p->base[i] == '`' && fence_close_at(p, i, le2)) { i = le2 + 1; break; }
                esc_html_json_into_str_avx2(p->base + i, le2 - i, scratch);
                str_lit(scratch, "\\n");
                i = le2 + 1;
            }
            str_lit(scratch, "</code></pre>\\n");
            continue;
        }
        size_t depth, t_off, t_len;
        if (c0 == '#' && heading_at(p, i, le, &depth, &t_off, &t_len)) {
            Sp text_s = {(uint32_t)t_off, (uint32_t)t_len};
            Sp slug = slug_make(p, text_s, slug_cache);
            Sp id = slug_unique(p, slug, &ids);
            str_lit(scratch, "<h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_lit(scratch, " id=\\\"");
            str_add(scratch, pool_at(p, id), id.len);
            str_lit(scratch, "\\\">");
            uint32_t html_start = (uint32_t)scratch->len;
            inline_span_json_avx2(p, env, pool_at(p, text_s), text_s.len, scratch, tmp);
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
        if ((c0 == '-' || c0 == '*') && hr_at(p, i, le)) {
            str_lit(scratch, "<hr>\\n");
            i = le + 1;
            continue;
        }
        if (c0 == '>') {
            str_clear(text_tmp);
            bool first = true;
            while (i < end) {
                int nl2 = scan_byte_avx2_i(p->base, end, i, '\n');
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (p->base[i] != '>') break;
                size_t s = i + 1;
                if (s < le2 && p->base[s] == ' ') s++;
                size_t ts, te;
                line_trim(p, s, le2, &ts, &te);
                if (ts < te) {
                    if (!first) str_c(text_tmp, '\n');
                    first = false;
                    str_add(text_tmp, p->base + ts, te - ts);
                }
                i = le2 + 1;
            }
            if (text_tmp->len == 0) {
                str_lit(scratch, "<blockquote>\\n</blockquote>\\n");
            } else {
                str_lit(scratch, "<blockquote>\\n<p>");
                inline_span_json_avx2(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                str_lit(scratch, "</p>\\n</blockquote>\\n");
            }
            continue;
        }
        bool ordered; int64_t start_num; size_t cur_ts, cur_te;
        if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
            && item_start_span(p, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
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
                    int nl2 = scan_byte_avx2_i(p->base, end, i, '\n');
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    bool o2; int64_t s2;
                    size_t nts, nte;
                    if (i < le2 && (p->base[i] == '-' || p->base[i] == '*' || (p->base[i] >= '0' && p->base[i] <= '9'))
                        && item_start_span(p, i, le2, &o2, &s2, &nts, &nte)) {
                        if (o2 == is_ordered) {
                            has_next = true;
                            next_ts = nts; next_te = nte; next_le = le2;
                        }
                        break;
                    }
                    if (i < le2 && p->base[i] == ' ' && continued_at(p, i, le2)) {
                        if (!multi) {
                            str_clear(item_scratch);
                            str_add(item_scratch, p->base + cur_ts, cur_te - cur_ts);
                            multi = true;
                        }
                        size_t cts, cte;
                        line_trim(p, i, le2, &cts, &cte);
                        str_c(item_scratch, '\n');
                        str_add(item_scratch, p->base + cts, cte - cts);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                const uint8_t *its = multi ? item_scratch->data : p->base + cur_ts;
                size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                str_lit(scratch, "<li>");
                inline_span_json_avx2(p, env, its, itn, scratch, tmp);
                str_lit(scratch, "</li>\\n");
                if (!has_next) break;
                cur_ts = next_ts; cur_te = next_te;
                i = next_le + 1;
            }
            if (is_ordered) str_lit(scratch, "</ol>\\n");
            else str_lit(scratch, "</ul>\\n");
            continue;
        }
        /* Paragraph (zero-copy even for multi-line paragraphs without leading/trailing ws). */
        size_t ts, te;
        line_trim(p, i, le, &ts, &te);
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            int nl2 = scan_byte_avx2_i(p->base, end, i, '\n');
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (para_ends(p, i, le2)) break;
            size_t cts, cte;
            line_trim(p, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, p->base + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, p->base + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : p->base + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        str_lit(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        inline_span_json_avx2(p, env, ps, pn, scratch, tmp);
        uint32_t html_end = (uint32_t)scratch->len;
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        str_lit(scratch, "</p>\\n");
    }
    *html_out = pool_store(p, scratch->data, scratch->len);
    if (has_excerpt) {
        *excerpt_out = (Sp){html_out->off + excerpt_range.start, excerpt_range.len};
    } else {
        *excerpt_out = SP_NONE;
    }
    for (size_t k = 0; k < toc_cnt; k++) {
        Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
        Toc *t = arr_at(toc_out, toc_base + k);
        t->html = (Sp){html_out->off + r.start, r.len};
    }
    return true;
}

bool md_build(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
              Str *scratch, Str *tmp, Str *text_tmp,
              Sp *html_out, Arr *toc_out, Sp *excerpt_out, bool json_esc) {
    if (json_esc && g_has_avx2) {
        return md_build_json_avx2(p, src, env, slug_cache, scratch, tmp, text_tmp, html_out, toc_out, excerpt_out);
    }
    str_clear(scratch);
    size_t off = src.off, len = src.len, end = off + len;
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    Set ids; set_new(&ids);
    size_t i = off;
    /* List item scratch buffers are reused across pages (see LinkCtx):
     * a fresh Str per page cost an mmap growth sequence each. */
    Str *item_scratch = env->item_scratch;
    while (i < end) {
        int nl = line_nl(p, i, end);
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (i == le) { i = le + 1; continue; }
        uint8_t c0 = p->base[i];
        if ((c0 == ' ' || c0 == '\t') && line_blank(p, i, le)) { i = le + 1; continue; }
        /* Fenced code. */
        size_t loff, llen;
        if (c0 == '`' && fence_open_at(p, i, le, &loff, &llen)) {
            str_lit(scratch, "<pre><code");
            if (llen > 0) {
                if (json_esc) str_lit(scratch, " class=\\\"language-");
                else str_lit(scratch, " class=\"language-");
                str_add(scratch, p->base + loff, llen);
                if (json_esc) str_lit(scratch, "\\\"");
                else str_c(scratch, '"');
            }
            str_c(scratch, '>');
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = line_nl(p, i, end);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (i < le2 && p->base[i] == '`' && fence_close_at(p, i, le2)) { i = le2 + 1; break; }
                if (json_esc) {
                    esc_html_json_into_str(p->base + i, le2 - i, scratch);
                    str_lit(scratch, "\\n");
                } else {
                    esc_html_into(p->base + i, le2 - i, scratch);
                    str_c(scratch, '\n');
                }
                i = le2 + 1;
            }
            if (json_esc) str_lit(scratch, "</code></pre>\\n");
            else str_lit(scratch, "</code></pre>\n");
            continue;
        }
        /* ATX heading. */
        size_t depth, t_off, t_len;
        if (c0 == '#' && heading_at(p, i, le, &depth, &t_off, &t_len)) {
            Sp text_s = {(uint32_t)t_off, (uint32_t)t_len};
            Sp slug = slug_make(p, text_s, slug_cache);
            Sp id = slug_unique(p, slug, &ids);
            str_lit(scratch, "<h");
            str_c(scratch, (uint8_t)('0' + depth));
            if (json_esc) str_lit(scratch, " id=\\\"");
            else str_lit(scratch, " id=\"");
            str_add(scratch, pool_at(p, id), id.len);
            if (json_esc) str_lit(scratch, "\\\">");
            else str_lit(scratch, "\">");
            uint32_t html_start = (uint32_t)scratch->len;
            if (json_esc) inline_span_json(p, env, pool_at(p, text_s), text_s.len, scratch, tmp);
            else inline_span(p, env, pool_at(p, text_s), text_s.len, scratch, tmp);
            uint32_t html_end = (uint32_t)scratch->len;
            str_lit(scratch, "</h");
            str_c(scratch, (uint8_t)('0' + depth));
            if (json_esc) str_lit(scratch, ">\\n");
            else str_lit(scratch, ">\n");
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
        /* Thematic break. */
        if ((c0 == '-' || c0 == '*') && hr_at(p, i, le)) {
            if (json_esc) str_lit(scratch, "<hr>\\n");
            else str_lit(scratch, "<hr>\n");
            i = le + 1;
            continue;
        }
        /* Blockquote. */
        if (c0 == '>') {
            str_clear(text_tmp);
            bool first = true;
            while (i < end) {
                int nl2 = line_nl(p, i, end);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (p->base[i] != '>') break;
                size_t s = i + 1;
                if (s < le2 && p->base[s] == ' ') s++;
                size_t ts, te;
                line_trim(p, s, le2, &ts, &te);
                if (ts < te) {
                    if (!first) str_c(text_tmp, '\n');
                    first = false;
                    str_add(text_tmp, p->base + ts, te - ts);
                }
                i = le2 + 1;
            }
            if (text_tmp->len == 0) {
                if (json_esc) str_lit(scratch, "<blockquote>\\n</blockquote>\\n");
                else str_lit(scratch, "<blockquote>\n</blockquote>\n");
            } else {
                if (json_esc) {
                    str_lit(scratch, "<blockquote>\\n<p>");
                    inline_span_json(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                    str_lit(scratch, "</p>\\n</blockquote>\\n");
                } else {
                    str_lit(scratch, "<blockquote>\n<p>");
                    inline_span(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                    str_lit(scratch, "</p>\n</blockquote>\n");
                }
            }
            continue;
        }
        /* Lists (streaming, zero-copy for single-line items). */
        bool ordered; int64_t start_num; size_t cur_ts, cur_te;
        if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
            && item_start_span(p, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
            bool is_ordered = ordered;
            if (is_ordered && start_num != 1) {
                if (json_esc) str_lit(scratch, "<ol start=\\\"");
                else str_lit(scratch, "<ol start=\"");
                str_i64(scratch, start_num);
                if (json_esc) str_lit(scratch, "\\\">\\n");
                else str_lit(scratch, "\">\n");
            } else {
                if (json_esc) {
                    if (is_ordered) str_lit(scratch, "<ol>\\n");
                    else str_lit(scratch, "<ul>\\n");
                } else {
                    if (is_ordered) str_lit(scratch, "<ol>\n");
                    else str_lit(scratch, "<ul>\n");
                }
            }
            i = le + 1;
            for (;;) {
                bool multi = false;
                bool has_next = false;
                size_t next_ts = 0, next_te = 0, next_le = 0;
                while (i < end) {
                    int nl2 = line_nl(p, i, end);
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    bool o2; int64_t s2;
                    size_t nts, nte;
                    if (i < le2 && (p->base[i] == '-' || p->base[i] == '*' || (p->base[i] >= '0' && p->base[i] <= '9'))
                        && item_start_span(p, i, le2, &o2, &s2, &nts, &nte)) {
                        if (o2 == is_ordered) {
                            has_next = true;
                            next_ts = nts; next_te = nte; next_le = le2;
                        }
                        break;
                    }
                    if (i < le2 && p->base[i] == ' ' && continued_at(p, i, le2)) {
                        if (!multi) {
                            str_clear(item_scratch);
                            str_add(item_scratch, p->base + cur_ts, cur_te - cur_ts);
                            multi = true;
                        }
                        size_t cts, cte;
                        line_trim(p, i, le2, &cts, &cte);
                        str_c(item_scratch, '\n');
                        str_add(item_scratch, p->base + cts, cte - cts);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                const uint8_t *its = multi ? item_scratch->data : p->base + cur_ts;
                size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                str_lit(scratch, "<li>");
                if (json_esc) {
                    inline_span_json(p, env, its, itn, scratch, tmp);
                    str_lit(scratch, "</li>\\n");
                } else {
                    inline_span(p, env, its, itn, scratch, tmp);
                    str_lit(scratch, "</li>\n");
                }
                if (!has_next) break;
                cur_ts = next_ts; cur_te = next_te;
                i = next_le + 1;
            }
            if (json_esc) {
                if (is_ordered) str_lit(scratch, "</ol>\\n");
                else str_lit(scratch, "</ul>\\n");
            } else {
                if (is_ordered) str_lit(scratch, "</ol>\n");
                else str_lit(scratch, "</ul>\n");
            }
            continue;
        }
        /* Paragraph (zero-copy when lines have no leading/trailing ws). */
        size_t ts, te;
        line_trim(p, i, le, &ts, &te);
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            int nl2 = line_nl(p, i, end);
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (para_ends(p, i, le2)) break;
            size_t cts, cte;
            line_trim(p, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, p->base + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, p->base + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : p->base + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        str_lit(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        if (json_esc) inline_span_json(p, env, ps, pn, scratch, tmp);
        else inline_span(p, env, ps, pn, scratch, tmp);
        uint32_t html_end = (uint32_t)scratch->len;
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        if (json_esc) str_lit(scratch, "</p>\\n");
        else str_lit(scratch, "</p>\n");
    }
    *html_out = pool_store(p, scratch->data, scratch->len);
    if (has_excerpt) {
        *excerpt_out = (Sp){html_out->off + excerpt_range.start, excerpt_range.len};
    } else {
        *excerpt_out = SP_NONE;
    }
    for (size_t k = 0; k < toc_cnt; k++) {
        Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
        Toc *t = arr_at(toc_out, toc_base + k);
        t->html = (Sp){html_out->off + r.start, r.len};
    }
    return true;
}
