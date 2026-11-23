/* md.c — the Markdown subset, lexed into blocks and rendered, in the
 * shape of marked's lexer and parser. Block rules: fenced code, ATX
 * headings (h2/h3 feed the TOC), thematic breaks, blockquotes (one
 * paragraph), lists, paragraphs. Inline: escapes, code spans, strong,
 * emphasis, links with path resolution. */
#include "zg.h"

static int line_nl(Pool *p, size_t from, size_t end) {
    return scan_nl(p->base, end, from);
}

static bool line_blank(Pool *p, size_t off, size_t end) {
    for (size_t i = off; i < end; i++) {
        uint8_t c = p->base[i];
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

static void line_trim(Pool *p, size_t off, size_t end, size_t *s, size_t *e) {
    size_t a = off, b = end;
    while (a < b && (p->base[a] == ' ' || p->base[a] == '\t')) a++;
    while (b > a && (p->base[b - 1] == ' ' || p->base[b - 1] == '\t')) b--;
    *s = a; *e = b;
}

/* FENCE_OPEN = /^```([A-Za-z0-9_+-]*)[ \t]*$/ — returns the lang span. */
static bool fence_open_at(Pool *p, size_t off, size_t end, size_t *loff, size_t *llen) {
    if (end - off < 3 || cmp_bytes(p->base + off, "```", 3) != 0) return false;
    size_t i = off + 3;
    while (i < end) {
        uint8_t c = p->base[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '_' || c == '+' || c == '-') i++;
        else break;
    }
    for (size_t k = i; k < end; k++) {
        if (p->base[k] != ' ' && p->base[k] != '\t') return false;
    }
    *loff = off + 3; *llen = i - (off + 3);
    return true;
}

static bool fence_close_at(Pool *p, size_t off, size_t end) {
    if (end - off < 3 || cmp_bytes(p->base + off, "```", 3) != 0) return false;
    for (size_t k = off + 3; k < end; k++) {
        if (p->base[k] != ' ' && p->base[k] != '\t') return false;
    }
    return true;
}

/* HEADING = /^(#{1,6})[ \t]+(.*?)[ \t]*$/ */
static bool heading_at(Pool *p, size_t off, size_t end, size_t *depth, size_t *toff, size_t *tlen) {
    size_t n = 0;
    while (off + n < end && p->base[off + n] == '#' && n < 6) n++;
    if (n == 0) return false;
    size_t i = off + n;
    if (i >= end || (p->base[i] != ' ' && p->base[i] != '\t')) return false;
    while (i < end && (p->base[i] == ' ' || p->base[i] == '\t')) i++;
    size_t e = end;
    while (e > i && (p->base[e - 1] == ' ' || p->base[e - 1] == '\t')) e--;
    if (e <= i) return false;
    *depth = n; *toff = i; *tlen = e - i;
    return true;
}

static bool hr_at(Pool *p, size_t off, size_t end) {
    if (end - off < 3) return false;
    if (cmp_bytes(p->base + off, "---", 3) != 0 && cmp_bytes(p->base + off, "***", 3) != 0) return false;
    for (size_t k = off + 3; k < end; k++) {
        if (p->base[k] != ' ' && p->base[k] != '\t') return false;
    }
    return true;
}

static bool para_ends(Pool *p, size_t off, size_t end) {
    size_t d, t, l;
    return line_blank(p, off, end)
        || fence_open_at(p, off, end, &t, &l)
        || heading_at(p, off, end, &d, &t, &l)
        || hr_at(p, off, end);
}

/* A list item's start: bullet or ordered. Returns the text in a Str. */
static bool item_start(Pool *p, size_t off, size_t end, bool *ordered, int64_t *num, Str *text) {
    if (end - off >= 2 && (p->base[off] == '-' || p->base[off] == '*') && p->base[off + 1] == ' ') {
        size_t ts, te;
        line_trim(p, off + 2, end, &ts, &te);
        str_clear(text);
        str_add(text, p->base + ts, te - ts);
        *ordered = false; *num = 1;
        return true;
    }
    size_t i = off, digits = 0;
    while (i < end && p->base[i] >= '0' && p->base[i] <= '9' && digits < 9) { i++; digits++; }
    if (digits == 0) return false;
    if (i + 1 >= end || p->base[i] != '.' || p->base[i + 1] != ' ') return false;
    int64_t n = 0;
    for (size_t k = off; k < i; k++) n = n * 10 + (p->base[k] - '0');
    size_t ts, te;
    line_trim(p, i + 2, end, &ts, &te);
    str_clear(text);
    str_add(text, p->base + ts, te - ts);
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
            case '&': str_cstr(out, "&amp;"); break;
            case '<': str_cstr(out, "&lt;"); break;
            case '>': str_cstr(out, "&gt;"); break;
            case '"': str_cstr(out, "&quot;"); break;
            default: str_cstr(out, "&#39;"); break;
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
        uint8_t needles[2] = {'\\', first};
        int j = scan_any(s, n, pos, needles, 2);
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
        uint8_t needles[2] = {'\\', '*'};
        int j = scan_any(s, n, pos, needles, 2);
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

static int page_lookup(LinkCtx *env, Pool *p, const uint8_t *path, size_t n) {
    size_t lo = 0, hi = env->npages;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        Page *pg = &env->pages[mid];
        int c = u16_cmp(pool_at(p, pg->path), pg->path.len, path, n);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else return (int)mid;
    }
    return -1;
}

static bool static_lookup(LinkCtx *env, Pool *p, const uint8_t *path, size_t n) {
    size_t lo = 0, hi = env->nstatics;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        SpPair *s = &env->statics[mid];
        int c = u16_cmp(pool_at(p, s->a), s->a.len, path, n);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else return true;
    }
    return false;
}

/* resolvePath(dir, rel) into out; false when it climbs out of the root. */
static bool resolve_rel(Pool *p, Sp dir, const uint8_t *rel, size_t rn, Str *out) {
    /* parts as spans into dir/rel (stack, small counts) */
    const uint8_t *parts[64];
    size_t plens[64];
    size_t np = 0;
    const uint8_t *db = pool_at(p, dir);
    if (dir.len > 0) {
        size_t s = 0;
        for (size_t i = 0; i <= dir.len; i++) {
            if (i == dir.len || db[i] == '/') {
                if (np < 64) { parts[np] = db + s; plens[np] = i - s; np++; }
                s = i + 1;
            }
        }
    }
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
    str_clear(out);
    for (size_t k = 0; k < np; k++) {
        if (k > 0) str_c(out, '/');
        str_add(out, parts[k], plens[k]);
    }
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
    if ((tn > 0 && target[0] == '/') || (tn > 0 && target[0] == '#') || is_scheme_span(target, tn)) {
        str_clear(tmp);
        str_add(tmp, target, tn);
        *href_len = tmp->len;
        return tmp->data;
    }
    size_t hash = tn;
    for (size_t i = 0; i < tn; i++) if (target[i] == '#') { hash = i; break; }
    size_t rl = hash, fl = tn - hash;
    const uint8_t *frag = target + hash;
    if (rl == 0) {
        note_broken(p, env, target, tn);
        str_clear(tmp);
        str_add(tmp, target, tn);
        *href_len = tmp->len;
        return tmp->data;
    }
    Str *path_buf = env->path_buf;
    bool ok = resolve_rel(p, env->dir, target, rl, path_buf);
    if (ok) {
        int idx = page_lookup(env, p, path_buf->data, path_buf->len);
        if (idx >= 0) {
            Page *pg = &env->pages[idx];
            str_clear(tmp);
            str_add(tmp, pool_at(p, pg->url), pg->url.len);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            return tmp->data;
        }
        if (static_lookup(env, p, path_buf->data, path_buf->len)) {
            str_clear(tmp);
            str_add(tmp, pool_at(p, env->base), env->base.len);
            str_c(tmp, '/');
            str_add(tmp, path_buf->data, path_buf->len);
            str_add(tmp, frag, fl);
            *href_len = tmp->len;
            return tmp->data;
        }
    }
    note_broken(p, env, target, tn);
    str_clear(tmp);
    str_add(tmp, target, tn);
    *href_len = tmp->len;
    return tmp->data;
}

static void inline_span(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, Str *out, Str *tmp) {
    static const uint8_t needles[] = {'\\', '`', '[', '*', '&', '<', '>', '"', '\''};
    size_t i = 0;
    while (i < n) {
        int j = scan_any(s, n, i, needles, 9);
        if (j < 0) {
            str_add(out, s + i, n - i); return;
        }
        str_add(out, s + i, (size_t)j - i);
        uint8_t c = s[j];
        switch (c) {
            case '\\': {
                if ((size_t)j + 1 < n && is_escapable(s[j + 1])) {
                    uint8_t e = s[j + 1];
                    if (e == '>') str_cstr(out, "&gt;");
                    else str_c(out, e);
                    i = (size_t)j + 2;
                } else {
                    str_c(out, '\\');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '`': {
                static const uint8_t bt[1] = {'`'};
                int k = scan_any(s, n, (size_t)j + 1, bt, 1);
                if (k >= 0 && (size_t)k > (size_t)j + 1) {
                    str_cstr(out, "<code>");
                    esc_html_into(s + j + 1, (size_t)k - ((size_t)j + 1), out);
                    str_cstr(out, "</code>");
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
                        str_cstr(out, "<strong>");
                        inline_span(p, env, s + j + 2, (size_t)k - ((size_t)j + 2), out, tmp);
                        str_cstr(out, "</strong>");
                        i = (size_t)k + 2;
                    } else {
                        str_cstr(out, "**");
                        i = (size_t)j + 2;
                    }
                } else {
                    int k = find_lone_star(s, n, (size_t)j + 1);
                    if (k > (int)(j + 1)) {
                        str_cstr(out, "<em>");
                        inline_span(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                        str_cstr(out, "</em>");
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
                    size_t q = (size_t)k + 2;
                    while (q < n && s[q] != ')') q++;
                    if (q < n) m = (int)q;
                }
                size_t tl = 0;
                const uint8_t *target = (const uint8_t *)"";
                if (m != -1) { target = s + (size_t)k + 2; tl = (size_t)m - ((size_t)k + 2); }
                if (tl > 0 && !has_ws_span(target, tl)) {
                    size_t href_len;
                    const uint8_t *href = resolve_href(p, env, target, tl, tmp, &href_len);
                    str_cstr(out, "<a href=\"");
                    esc_html_into(href, href_len, out);
                    str_cstr(out, "\">");
                    inline_span(p, env, s + j + 1, (size_t)k - ((size_t)j + 1), out, tmp);
                    str_cstr(out, "</a>");
                    i = (size_t)m + 1;
                } else {
                    str_c(out, '[');
                    i = (size_t)j + 1;
                }
                break;
            }
            case '&': str_cstr(out, "&amp;"); i = (size_t)j + 1; break;
            case '<': str_cstr(out, "&lt;"); i = (size_t)j + 1; break;
            case '>': str_cstr(out, "&gt;"); i = (size_t)j + 1; break;
            case '"': str_cstr(out, "&quot;"); i = (size_t)j + 1; break;
            default: str_cstr(out, "&#39;"); i = (size_t)j + 1; break;
        }
    }
}

bool md_build(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
              Str *scratch, Str *tmp, Str *text_tmp,
              Sp *html_out, Arr *toc_out, Sp *excerpt_out) {
    str_clear(scratch);
    size_t off = src.off, len = src.len, end = off + len;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    Set ids; set_new(&ids);
    size_t i = off;
    /* List item scratch buffers are reused across pages (see LinkCtx):
     * a fresh Str per page cost an mmap growth sequence each. */
    Str *item_buf = env->item_buf;
    Str *item_scratch = env->item_scratch;
    while (i < end) {
        int nl = line_nl(p, i, end);
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (line_blank(p, i, le)) { i = le + 1; continue; }
        /* Fenced code. */
        size_t loff, llen;
        if (fence_open_at(p, i, le, &loff, &llen)) {
            str_cstr(scratch, "<pre><code");
            if (llen > 0) {
                str_cstr(scratch, " class=\"language-");
                esc_html_into(p->base + loff, llen, scratch);
                str_c(scratch, '"');
            }
            str_c(scratch, '>');
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = line_nl(p, i, end);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                if (fence_close_at(p, i, le2)) { i = le2 + 1; break; }
                esc_html_into(p->base + i, le2 - i, scratch);
                str_c(scratch, '\n');
                i = le2 + 1;
            }
            str_cstr(scratch, "</code></pre>\n");
            continue;
        }
        /* ATX heading. */
        size_t depth, t_off, t_len;
        if (heading_at(p, i, le, &depth, &t_off, &t_len)) {
            Sp text_s = {(uint32_t)t_off, (uint32_t)t_len};
            Sp slug = slug_make(p, text_s, slug_cache);
            Sp id = slug_unique(p, slug, &ids);
            str_cstr(scratch, "<h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_cstr(scratch, " id=\"");
            str_add(scratch, pool_at(p, id), id.len);
            str_cstr(scratch, "\">");
            uint32_t html_start = (uint32_t)scratch->len;
            inline_span(p, env, pool_at(p, text_s), text_s.len, scratch, tmp);
            uint32_t html_end = (uint32_t)scratch->len;
            str_cstr(scratch, "</h");
            str_c(scratch, (uint8_t)('0' + depth));
            str_cstr(scratch, ">\n");
            if (depth == 2 || depth == 3) {
                Toc *t = arr_slot(toc_out, p);
                t->level = (int64_t)depth; t->id = id; t->html = SP_NONE;
                Range *r = arr_slot(&toc_html, p);
                r->start = html_start; r->len = html_end - html_start;
            }
            i = le + 1;
            continue;
        }
        /* Thematic break. */
        if (hr_at(p, i, le)) {
            str_cstr(scratch, "<hr>\n");
            i = le + 1;
            continue;
        }
        /* Blockquote. */
        if (p->base[i] == '>') {
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
                str_cstr(scratch, "<blockquote>\n</blockquote>\n");
            } else {
                str_cstr(scratch, "<blockquote>\n<p>");
                inline_span(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
                str_cstr(scratch, "</p>\n</blockquote>\n");
            }
            continue;
        }
        /* Lists. */
        bool ordered; int64_t start_num;
        if (item_start(p, i, le, &ordered, &start_num, item_buf)) {
            bool is_ordered = ordered;
            /* Items as pool spans; one scratch Str reused for the current
             * item (a fresh Str per item cost an mmap each). */
            Arr items; arr_new(&items, sizeof(Sp));
            str_clear(item_scratch);
            str_add(item_scratch, item_buf->data, item_buf->len);
            i = le + 1;
            for (;;) {
                if (i >= end) break;
                int nl2 = line_nl(p, i, end);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                bool o2; int64_t s2;
                if (item_start(p, i, le2, &o2, &s2, item_buf)) {
                    if (o2 == is_ordered) {
                        Sp *slot = arr_slot(&items, p);
                        *slot = pool_store(p, item_scratch->data, item_scratch->len);
                        str_clear(item_scratch);
                        str_add(item_scratch, item_buf->data, item_buf->len);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                if (continued_at(p, i, le2)) {
                    size_t ts, te;
                    line_trim(p, i, le2, &ts, &te);
                    str_c(item_scratch, '\n');
                    str_add(item_scratch, p->base + ts, te - ts);
                    i = le2 + 1;
                    continue;
                }
                break;
            }
            {
                Sp *slot = arr_slot(&items, p);
                *slot = pool_store(p, item_scratch->data, item_scratch->len);
            }
            if (is_ordered && start_num != 1) {
                str_cstr(scratch, "<ol start=\"");
                str_i64(scratch, start_num);
                str_cstr(scratch, "\">\n");
            } else {
                str_c(scratch, '<');
                str_cstr(scratch, is_ordered ? "ol" : "ul");
                str_cstr(scratch, ">\n");
            }
            for (size_t k = 0; k < items.len; k++) {
                Sp *it = arr_at(&items, k);
                str_cstr(scratch, "<li>");
                inline_span(p, env, pool_at(p, *it), it->len, scratch, tmp);
                str_cstr(scratch, "</li>\n");
            }
            str_cstr(scratch, "</");
            str_cstr(scratch, is_ordered ? "ol" : "ul");
            str_cstr(scratch, ">\n");
            continue;
        }
        /* Paragraph. */
        str_clear(text_tmp);
        {
            size_t ts, te;
            line_trim(p, i, le, &ts, &te);
            str_add(text_tmp, p->base + ts, te - ts);
        }
        i = le + 1;
        while (i < end) {
            int nl2 = line_nl(p, i, end);
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (para_ends(p, i, le2)) break;
            size_t ts, te;
            line_trim(p, i, le2, &ts, &te);
            str_c(text_tmp, '\n');
            str_add(text_tmp, p->base + ts, te - ts);
            i = le2 + 1;
        }
        str_cstr(scratch, "<p>");
        uint32_t html_start = (uint32_t)scratch->len;
        inline_span(p, env, text_tmp->data, text_tmp->len, scratch, tmp);
        uint32_t html_end = (uint32_t)scratch->len;
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        str_cstr(scratch, "</p>\n");
    }
    *html_out = pool_store(p, scratch->data, scratch->len);
    if (has_excerpt) {
        *excerpt_out = pool_store(p, scratch->data + excerpt_range.start, excerpt_range.len);
    } else {
        *excerpt_out = SP_NONE;
    }
    for (size_t k = 0; k < toc_out->len; k++) {
        Range *r = arr_at(&toc_html, k);
        Toc *t = arr_at(toc_out, k);
        t->html = pool_store(p, scratch->data + r->start, r->len);
    }
    return true;
}
