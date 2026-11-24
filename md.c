/* md.c — high-throughput ISA-specialized Markdown renderer for the makefun-site
 * subset. Features:
 * - Zero-branch table-driven HTML/JSON escaping (`ESC_HTML_BYTES`/`ESC_HTML_LEN`)
 *   and zero-branch overwriting vector copies during bitmask draining.
 * - Dual-bitmask SIMD scanner (`m_all` + `m_struct`) across AVX2, SSSE3, and
 *   pure SSE2: vectors with no structural markup (`*`, `` ` ``, `[`, `\`) drain
 *   purely via branchless table lookup + overwriting vector copy.
 * - Full ISA specialization of `md_build_json` (AVX2/SSSE3/SSE2): zero per-line
 *   CPUID checks, zero `vzeroupper` transitions, zero indirect calls, inlined
 *   `scan_nl` and `scan_two`.
 * - Fast `../` relative path resolution using precomputed `dir_prefix_len`,
 *   `.md`-extension filter before `page_map` lookup, and zero-hash ASCII heading
 *   slugification. */
#include "zg.h"
#include <immintrin.h>

static const uint8_t PARA_END_FIRST[256] = {
    [' '] = 1, ['\t'] = 1, ['`'] = 1, ['#'] = 1, ['-'] = 1, ['*'] = 1
};

/* Inline character category: 0=plain, 1=simple HTML/JSON escape, 2=MD structural */
static const uint8_t TBL_INLINE_CAT[256] = {
    [0 ... 0x1f] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1,
    ['*'] = 2, ['`'] = 2, ['['] = 2, ['\\'] = 2
};

static const uint8_t TBL_ESCAPABLE[256] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['_'] = 1, ['['] = 1, [']'] = 1,
    ['('] = 1, [')'] = 1, ['#'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1,
    ['!'] = 1, ['>'] = 1
};

/* Branchless 8-byte HTML+JSON escape table (covers all 0x00..0x1f, &, <, >, ", ', \). */
static const uint8_t ESC_HTML_BYTES[256][8] = {
    [0x00] = "\\u0000", [0x01] = "\\u0001", [0x02] = "\\u0002", [0x03] = "\\u0003",
    [0x04] = "\\u0004", [0x05] = "\\u0005", [0x06] = "\\u0006", [0x07] = "\\u0007",
    [0x08] = "\\b",     [0x09] = "\\t",     [0x0a] = "\\n",     [0x0b] = "\\u000b",
    [0x0c] = "\\f",     [0x0d] = "\\r",     [0x0e] = "\\u000e", [0x0f] = "\\u000f",
    [0x10] = "\\u0010", [0x11] = "\\u0011", [0x12] = "\\u0012", [0x13] = "\\u0013",
    [0x14] = "\\u0014", [0x15] = "\\u0015", [0x16] = "\\u0016", [0x17] = "\\u0017",
    [0x18] = "\\u0018", [0x19] = "\\u0019", [0x1a] = "\\u001a", [0x1b] = "\\u001b",
    [0x1c] = "\\u001c", [0x1d] = "\\u001d", [0x1e] = "\\u001e", [0x1f] = "\\u001f",
    ['"']  = "&quot;",
    ['&']  = "&amp;",
    ['\''] = "&#39;",
    ['<']  = "&lt;",
    ['>']  = "&gt;",
    ['\\'] = "\\\\",
};

static const uint8_t ESC_HTML_LEN[256] = {
    [0x00] = 6, [0x01] = 6, [0x02] = 6, [0x03] = 6,
    [0x04] = 6, [0x05] = 6, [0x06] = 6, [0x07] = 6,
    [0x08] = 2, [0x09] = 2, [0x0a] = 2, [0x0b] = 6,
    [0x0c] = 2, [0x0d] = 2, [0x0e] = 6, [0x0f] = 6,
    [0x10] = 6, [0x11] = 6, [0x12] = 6, [0x13] = 6,
    [0x14] = 6, [0x15] = 6, [0x16] = 6, [0x17] = 6,
    [0x18] = 6, [0x19] = 6, [0x1a] = 6, [0x1b] = 6,
    [0x1c] = 6, [0x1d] = 6, [0x1e] = 6, [0x1f] = 6,
    ['"']  = 6,
    ['&']  = 5,
    ['\''] = 5,
    ['<']  = 4,
    ['>']  = 4,
    ['\\'] = 2,
};

static inline bool is_escapable(uint8_t c) { return TBL_ESCAPABLE[c] != 0; }

static inline uint8_t *ensure_room(LinkCtx *env, uint8_t *dst, size_t need) {
    if (__builtin_expect(dst + need <= env->out_end, 1)) return dst;
    size_t off = (size_t)(dst - env->out_str->data);
    env->out_str->len = off;
    str_grow(env->out_str, off + need + 16384);
    env->out_end = env->out_str->data + env->out_str->cap;
    return env->out_str->data + off;
}

/* ---------------- block helpers ---------------- */

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

static inline bool fence_open_at(const uint8_t *b, size_t off, size_t end, size_t *loff, size_t *llen) {
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

static inline bool fence_close_at(const uint8_t *b, size_t off, size_t end) {
    if (end - off < 3 || b[off] != '`' || b[off + 1] != '`' || b[off + 2] != '`') return false;
    for (size_t k = off + 3; k < end; k++) {
        if (b[k] != ' ' && b[k] != '\t') return false;
    }
    return true;
}

static inline bool heading_at(const uint8_t *b, size_t off, size_t end, size_t *depth, size_t *toff, size_t *tlen) {
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

static inline bool hr_at(const uint8_t *b, size_t off, size_t end) {
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
    if (__builtin_expect(!PARA_END_FIRST[c0], 1)) return false;
    size_t d, t, l;
    if (c0 == ' ' || c0 == '\t') return line_blank(b, off, end);
    if (c0 == '`') return fence_open_at(b, off, end, &t, &l);
    if (c0 == '#') return heading_at(b, off, end, &d, &t, &l);
    return hr_at(b, off, end);
}

static inline bool item_start_span(const uint8_t *b, size_t off, size_t end, bool *ordered, int64_t *num, size_t *ts_out, size_t *te_out) {
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

static inline bool continued_at(const uint8_t *b, size_t off, size_t end) {
    size_t i = off, spaces = 0;
    while (i < end && b[i] == ' ') { spaces++; i++; }
    if (spaces < 2 || i >= end) return false;
    uint8_t c = b[i];
    return !(c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r');
}

/* ---------------- link resolution ---------------- */

static inline bool is_scheme_span(const uint8_t *t, size_t n) {
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
    /* Every entry in page_map ends with ".md"; skip hashing/probing for statics! */
    if (n < 3 || path[n - 3] != '.' || path[n - 2] != 'm' || path[n - 1] != 'd') return -1;
    Sp hit;
    if (dict_get(env->page_map, p, path, n, &hit)) return (int)hit.off;
    return -1;
}

static inline bool static_lookup(LinkCtx *env, Pool *p, const uint8_t *path, size_t n) {
    if (env->nstatics == 0) return false;
    return set_has(env->static_set, p, path, n);
}

static bool resolve_rel_buf(LinkCtx *env, Pool *p, const uint8_t *rel, size_t rn,
                            uint8_t *sbuf, size_t scap, Str *fallback,
                            const uint8_t **out_ptr, size_t *out_len) {
    /* Fast path: handle any leading "../" or "./" segments + clean tail! */
    size_t np = env->dir_np;
    const uint8_t *t = rel;
    size_t tn = rn;
    bool fast = (tn > 0 && t[0] != '/');
    while (fast && tn >= 2 && t[0] == '.') {
        if (t[1] == '/') { t += 2; tn -= 2; continue; }
        if (tn >= 3 && t[1] == '.' && t[2] == '/') {
            if (np == 0) return false;
            np--;
            t += 3; tn -= 3;
            continue;
        }
        fast = false;
    }
    if (fast && tn > 0 && t[0] != '.' && t[0] != '/') {
        for (size_t i = 0; i < tn; i++) {
            if (t[i] == '/') {
                if (i + 1 >= tn || t[i + 1] == '/' || t[i + 1] == '.') { fast = false; break; }
            }
        }
        if (fast) {
            size_t dlen = env->dir_prefix_len[np];
            if (dlen == 0) {
                *out_ptr = t;
                *out_len = tn;
                return true;
            }
            size_t total = dlen + 1 + tn;
            if (__builtin_expect(total <= scap, 1)) {
                uint8_t *d = sbuf;
                copy_fast(d, pool_at(p, env->dir), dlen);
                d += dlen;
                *d++ = '/';
                copy_fast(d, t, tn);
                *out_ptr = sbuf;
                *out_len = total;
                return true;
            }
        }
    }

    const uint8_t *parts[64];
    size_t plens[64];
    np = env->dir_np;
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

static const uint8_t *resolve_href_ex(Pool *p, LinkCtx *env, const uint8_t *target, size_t tn,
                                      size_t hash, bool target_clean, Str *tmp, size_t *href_len, bool *href_clean) {
    *href_clean = false;
    if (tn > 0) {
        uint8_t c0 = target[0];
        if (c0 == '/' || c0 == '#') {
            *href_len = tn;
            *href_clean = target_clean;
            return target;
        }
        if (c0 != '.' && is_scheme_span(target, tn)) {
            *href_len = tn;
            *href_clean = target_clean;
            return target;
        }
    }
    size_t rl = hash, fl = tn - hash;
    const uint8_t *frag = target + hash;
    if (rl == 0) {
        note_broken(p, env, target, tn);
        *href_len = tn;
        *href_clean = target_clean;
        return target;
    }
    uint8_t sbuf[576];
    const uint8_t *rpath;
    size_t rlen;
    bool ok = resolve_rel_buf(env, p, target, rl, sbuf, 512, env->path_buf, &rpath, &rlen);
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
            *href_clean = ((pg->clean_mask & 2) != 0) && target_clean;
            return tmp->data;
        }
        if (static_lookup(env, p, rpath, rlen)) {
            str_clear(tmp);
            str_add(tmp, pool_at(p, env->base), env->base.len);
            str_c(tmp, '/');
            str_add(tmp, rpath, rlen);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            *href_clean = env->base_clean && env->dir_clean && target_clean;
            return tmp->data;
        }
    }
    note_broken(p, env, target, tn);
    *href_len = tn;
    *href_clean = target_clean;
    return target;
}

/* Stack-backed heading id deduplication (zero pool/hash overhead for <=16 headings). */
typedef struct {
    Sp small[16];
    size_t nsmall;
    bool spilled;
    Set set;
} SmallIds;

static inline bool small_ids_add(SmallIds *si, Pool *p, Sp s) {
    if (__builtin_expect(!si->spilled, 1)) {
        const uint8_t *sb = pool_at(p, s);
        for (size_t k = 0; k < si->nsmall; k++) {
            if (si->small[k].len == s.len && eq_bytes(pool_at(p, si->small[k]), sb, s.len)) return false;
        }
        if (si->nsmall < 16) { si->small[si->nsmall++] = s; return true; }
        set_new(&si->set);
        for (size_t k = 0; k < si->nsmall; k++) set_add(&si->set, p, si->small[k]);
        si->spilled = true;
    }
    return set_add(&si->set, p, s);
}

static Sp slug_unique_small(Pool *p, Sp slug, SmallIds *si) {
    if (__builtin_expect(small_ids_add(si, p, slug), 1)) return slug;
    uint8_t stack[64];
    size_t slen = slug.len;
    copy_fast(stack, pool_at(p, slug), slen);
    uint64_t n = 2;
    for (;;) {
        uint32_t start = pool_mark(p);
        pool_put(p, stack, slen);
        pool_u8(p, '-');
        pool_u64(p, n);
        Sp cand = pool_span(p, start);
        n++;
        if (small_ids_add(si, p, cand)) return cand;
    }
}

/* ---------------- ISA-specialized fast JSON-escaped Markdown engines ---------------- */

#define MD_SUF avx2
#define MD_ATTR __attribute__((target("avx2,bmi")))
#define MD_IS_AVX2 1
#define MD_IS_SSSE3 0
#include "md_isa.inc"
#undef MD_SUF
#undef MD_ATTR
#undef MD_IS_AVX2
#undef MD_IS_SSSE3

#define MD_SUF ssse3
#define MD_ATTR __attribute__((target("ssse3")))
#define MD_IS_AVX2 0
#define MD_IS_SSSE3 1
#include "md_isa.inc"
#undef MD_SUF
#undef MD_ATTR
#undef MD_IS_AVX2
#undef MD_IS_SSSE3

#define MD_SUF sse2
#define MD_ATTR
#define MD_IS_AVX2 0
#define MD_IS_SSSE3 0
#include "md_isa.inc"
#undef MD_SUF
#undef MD_ATTR
#undef MD_IS_AVX2
#undef MD_IS_SSSE3

/* ---------------- non-json_esc fallback (rare: {{ page.content }}) ---------------- */

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

static void inline_span(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
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
                    int k = find_unesc_i_sse2(s, n, '*', '*', 2, (size_t)j + 2);
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
                    int k = find_lone_star_i_sse2(s, n, (size_t)j + 1);
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
                int k = find_unesc_i_sse2(s, n, ']', 0, 1, (size_t)j + 1);
                int m = -1;
                if (k != -1 && (size_t)k + 1 < n && s[k + 1] == '(') {
                    int q = scan_byte(s, n, (size_t)k + 2, ')');
                    if (q >= 0) m = q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t hash = tl;
                    for (size_t q = 0; q < tl; q++) if (target[q] == '#') { hash = q; break; }
                    size_t href_len; bool hclean;
                    const uint8_t *href = resolve_href_ex(p, env, target, tl, hash, false, tmp, &href_len, &hclean);
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

bool md_build(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
              Str *scratch, Str *tmp, Str *text_tmp,
              Sp *html_out, Arr *toc_out, Sp *excerpt_out,
              bool json_esc, bool store_html_in_pool) {
    if (__builtin_expect(json_esc, 1)) {
        if (g_has_avx2) return md_build_json_avx2(p, src, env, slug_cache, scratch, tmp, text_tmp, html_out, toc_out, excerpt_out, store_html_in_pool);
        if (g_has_ssse3) return md_build_json_ssse3(p, src, env, slug_cache, scratch, tmp, text_tmp, html_out, toc_out, excerpt_out, store_html_in_pool);
        return md_build_json_sse2(p, src, env, slug_cache, scratch, tmp, text_tmp, html_out, toc_out, excerpt_out, store_html_in_pool);
    }
    str_clear(scratch);
    const uint8_t *base = p->base;
    size_t off = src.off, end = off + src.len;
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    SmallIds ids; ids.nsmall = 0; ids.spilled = false;
    size_t i = off;
    Str *item_scratch = env->item_scratch;
    while (i < end) {
        int nl = scan_nl_i_sse2(base, end, i);
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (i == le) { i = le + 1; continue; }
        uint8_t c0 = base[i];
        if ((c0 == ' ' || c0 == '\t') && line_blank(base, i, le)) { i = le + 1; continue; }
        size_t loff, llen;
        if (c0 == '`' && fence_open_at(base, i, le, &loff, &llen)) {
            str_lit(scratch, "<pre><code");
            if (llen > 0) {
                str_lit(scratch, " class=\"language-");
                str_add(scratch, base + loff, llen);
                str_c(scratch, '"');
            }
            str_c(scratch, '>');
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = scan_nl_i_sse2(base, end, i);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (i < le2 && base[i] == '`' && fence_close_at(base, i, le2)) { i = le2 + 1; break; }
                esc_html_into(base + i, le2 - i, scratch);
                str_c(scratch, '\n');
                i = le2 + 1;
            }
            str_lit(scratch, "</code></pre>\n");
            continue;
        }
        size_t depth, t_off, t_len;
        if (c0 == '#' && heading_at(base, i, le, &depth, &t_off, &t_len)) {
            Sp text_s = {(uint32_t)t_off, (uint32_t)t_len};
            Sp slug = slug_make_fast(p, text_s, slug_cache);
            Sp id = slug_unique_small(p, slug, &ids);
            str_lit(scratch, "<h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_lit(scratch, " id=\"");
            str_add(scratch, base + id.off, id.len);
            str_lit(scratch, "\">");
            uint32_t html_start = (uint32_t)scratch->len;
            inline_span(p, env, base + t_off, t_len, scratch, tmp);
            uint32_t html_end = (uint32_t)scratch->len;
            str_lit(scratch, "</h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_lit(scratch, ">\n");
            if (depth == 2 || depth == 3) {
                Toc *t = arr_slot(toc_out, p);
                t->level = (int64_t)depth; t->id = id; t->html = SP_NONE;
                Range hr = {html_start, html_end - html_start};
                if (toc_cnt < 64 && toc_html.len == 0) toc_stack[toc_cnt++] = hr;
                else {
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
        if ((c0 == '-' || c0 == '*') && hr_at(base, i, le)) {
            str_lit(scratch, "<hr>\n");
            i = le + 1;
            continue;
        }
        if (c0 == '>') {
            str_clear(text_tmp);
            bool first = true;
            while (i < end) {
                int nl2 = scan_nl_i_sse2(base, end, i);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (base[i] != '>') break;
                size_t s = i + 1;
                if (s < le2 && base[s] == ' ') s++;
                size_t ts, te;
                line_trim(base, s, le2, &ts, &te);
                if (ts < te) {
                    if (!first) str_c(text_tmp, '\n');
                    first = false;
                    str_add(text_tmp, base + ts, te - ts);
                }
                i = le2 + 1;
            }
            if (text_tmp->len == 0) str_lit(scratch, "<blockquote>\n</blockquote>\n");
            else {
                str_lit(scratch, "<blockquote>\n<p>");
                inline_span(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                str_lit(scratch, "</p>\n</blockquote>\n");
            }
            continue;
        }
        bool ordered; int64_t start_num; size_t cur_ts, cur_te;
        if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
            && item_start_span(base, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
            bool is_ordered = ordered;
            if (is_ordered && start_num != 1) {
                str_lit(scratch, "<ol start=\"");
                str_i64(scratch, start_num);
                str_lit(scratch, "\">\n");
            } else {
                str_lit(scratch, is_ordered ? "<ol>\n" : "<ul>\n");
            }
            i = le + 1;
            for (;;) {
                bool multi = false;
                bool has_next = false;
                size_t next_ts = 0, next_te = 0, next_le = 0;
                while (i < end) {
                    int nl2 = scan_nl_i_sse2(base, end, i);
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    bool o2; int64_t s2;
                    size_t nts, nte;
                    if (i < le2 && (base[i] == '-' || base[i] == '*' || (base[i] >= '0' && base[i] <= '9'))
                        && item_start_span(base, i, le2, &o2, &s2, &nts, &nte)) {
                        if (o2 == is_ordered) {
                            has_next = true;
                            next_ts = nts; next_te = nte; next_le = le2;
                        }
                        break;
                    }
                    if (i < le2 && base[i] == ' ' && continued_at(base, i, le2)) {
                        if (!multi) {
                            str_clear(item_scratch);
                            str_add(item_scratch, base + cur_ts, cur_te - cur_ts);
                            multi = true;
                        }
                        size_t cts, cte;
                        line_trim(base, i, le2, &cts, &cte);
                        str_c(item_scratch, '\n');
                        str_add(item_scratch, base + cts, cte - cts);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                const uint8_t *its = multi ? item_scratch->data : base + cur_ts;
                size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                str_lit(scratch, "<li>");
                inline_span(p, env, its, itn, scratch, tmp);
                str_lit(scratch, "</li>\n");
                if (!has_next) break;
                cur_ts = next_ts; cur_te = next_te;
                i = next_le + 1;
            }
            str_lit(scratch, is_ordered ? "</ol>\n" : "</ul>\n");
            continue;
        }
        size_t ts, te;
        line_trim(base, i, le, &ts, &te);
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            int nl2 = scan_nl_i_sse2(base, end, i);
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (para_ends(base, i, le2)) break;
            size_t cts, cte;
            line_trim(base, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, base + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, base + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : base + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        str_lit(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        inline_span(p, env, ps, pn, scratch, tmp);
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
