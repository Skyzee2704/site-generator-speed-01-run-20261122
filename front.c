/* front.c — a source's front matter and body, following
 * lib/frontmatter.js exactly: seven keys, each with its own value
 * grammar; the first refusal wins. Error messages byte-identical. */
#include "zg.h"

static const char *META_KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

static int meta_nl(Pool *p, size_t from, size_t end) {
    return scan_nl(p->base, end, from);
}

static bool line_blank(Pool *p, size_t off, size_t end) {
    for (size_t i = off; i < end; i++) {
        uint8_t c = p->base[i];
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

/* Split one front-matter line into (key_off, key_len, val_off, val_len).
 * Key: [a-z_]+, then ':', then exactly one space, then the value with
 * trailing spaces/tabs trimmed. A line ending right at ':' has an
 * empty value. */
static bool line_split(Pool *p, size_t off, size_t end,
                       size_t *koff, size_t *klen, size_t *voff, size_t *vlen) {
    size_t i = off;
    while (i < end && ((p->base[i] >= 'a' && p->base[i] <= 'z') || p->base[i] == '_')) i++;
    if (i == off || i >= end || p->base[i] != ':') return false;
    *koff = off; *klen = i - off;
    if (i + 1 == end) { *voff = 0; *vlen = 0; return true; }
    if (p->base[i + 1] != ' ') return false;
    size_t vend = end;
    while (vend > i + 2 && (p->base[vend - 1] == ' ' || p->base[vend - 1] == '\t')) vend--;
    *voff = i + 2; *vlen = vend - (i + 2);
    return true;
}

static Sp keyed_err(Pool *p, const char *key, const char *msg) {
    uint32_t start = pool_mark(p);
    pool_put(p, (const uint8_t *)"key \"", 5);
    size_t kl = 0; while (key[kl]) kl++;
    pool_put(p, (const uint8_t *)key, kl);
    pool_put(p, (const uint8_t *)"\": ", 3);
    size_t ml = 0; while (msg[ml]) ml++;
    pool_put(p, (const uint8_t *)msg, ml);
    return pool_span(p, start);
}

/* A string value: "..." with \" and \\ its only escapes, or bare. */
static bool str_value(Pool *p, size_t voff, size_t vlen, Sp *out, const char **err) {
    if (p->base[voff] == '"') {
        if (vlen < 2 || p->base[voff + vlen - 1] != '"') {
            *err = "a quoted string is not closed";
            return false;
        }
        uint32_t start = pool_mark(p);
        size_t i = voff + 1;
        size_t last = voff + vlen - 1;
        while (i < last) {
            uint8_t c = p->base[i];
            if (c == '\\') {
                uint8_t n = p->base[i + 1];
                if ((n != '"' && n != '\\') || i + 1 == last) {
                    p->len = start;
                    *err = "a quoted string holds an escape other than \\\" or \\\\";
                    return false;
                }
                pool_u8(p, n);
                i += 2;
            } else if (c == '"') {
                p->len = start;
                *err = "a quoted string holds a stray quote";
                return false;
            } else {
                pool_u8(p, c);
                i++;
            }
        }
        *out = pool_span(p, start);
        return true;
    }
    if (p->base[voff] == '[') {
        *err = "a bare string opens on [";
        return false;
    }
    *out = (Sp){(uint32_t)voff, (uint32_t)vlen};
    return true;
}

static bool date_ok(Pool *p, size_t voff, size_t vlen) {
    if (vlen != 10 || p->base[voff + 4] != '-' || p->base[voff + 7] != '-') return false;
    for (size_t k = 0; k < 10; k++) {
        if (k != 4 && k != 7) {
            uint8_t c = p->base[voff + k];
            if (c < '0' || c > '9') return false;
        }
    }
    uint32_t y = (uint32_t)(p->base[voff] - '0') * 1000 + (uint32_t)(p->base[voff + 1] - '0') * 100
        + (uint32_t)(p->base[voff + 2] - '0') * 10 + (uint32_t)(p->base[voff + 3] - '0');
    uint32_t mo = (uint32_t)(p->base[voff + 5] - '0') * 10 + (uint32_t)(p->base[voff + 6] - '0');
    uint32_t day = (uint32_t)(p->base[voff + 8] - '0') * 10 + (uint32_t)(p->base[voff + 9] - '0');
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || day < 1) return false;
    uint32_t dim;
    if (mo == 2) dim = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
    else if (mo == 4 || mo == 6 || mo == 9 || mo == 11) dim = 30;
    else dim = 31;
    return day <= dim;
}

/* [a-z0-9]+(-[a-z0-9]+)* */
static bool slug_ok(Pool *p, size_t voff, size_t vlen) {
    if (vlen == 0) return false;
    size_t i = voff, end = voff + vlen;
    for (;;) {
        uint8_t c = p->base[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
        i++;
        while (i < end && ((p->base[i] >= 'a' && p->base[i] <= 'z') || (p->base[i] >= '0' && p->base[i] <= '9'))) i++;
        if (i >= end) return true;
        if (p->base[i] != '-') return false;
        i++;
        if (i >= end) return false;
    }
}

static bool tags_parse(Pool *p, size_t voff, size_t vlen, Arr *tags_out, const char **err) {
    if (p->base[voff] != '[' || p->base[voff + vlen - 1] != ']') {
        *err = "not a list [a, b]";
        return false;
    }
    size_t inner_off = voff + 1, inner_end = voff + vlen - 1;
    bool all_ws = true;
    for (size_t i = inner_off; i < inner_end; i++) {
        uint8_t c = p->base[i];
        if (c != ' ' && c != '\t') { all_ws = false; break; }
    }
    if (all_ws) return true;
    size_t i = inner_off;
    for (;;) {
        size_t j = i;
        while (j < inner_end && p->base[j] != ',') j++;
        size_t s = i, e = j;
        while (s < e && (p->base[s] == ' ' || p->base[s] == '\t')) s++;
        while (e > s && (p->base[e - 1] == ' ' || p->base[e - 1] == '\t')) e--;
        bool bad = s >= e;
        if (!bad) {
            for (size_t k = s; k < e; k++) {
                uint8_t c = p->base[k];
                if (c == '"' || c == '[' || c == ']') { bad = true; break; }
            }
        }
        if (bad) {
            *err = "an empty tag, or one holding \" [ or ]";
            return false;
        }
        Sp *slot = arr_slot(tags_out, p);
        *slot = (Sp){(uint32_t)s, (uint32_t)(e - s)};
        if (j >= inner_end) break;
        i = j + 1;
    }
    return true;
}

bool meta_parse(Pool *p, Sp text, Meta *out, Sp *err) {
    size_t off = text.off, len = text.len, end = off + len;
    int first_nl = meta_nl(p, off, end);
    size_t first_line_end = first_nl >= 0 ? (size_t)first_nl : end;
    if (first_line_end - off != 3 || cmp_bytes(p->base + off, "---", 3) != 0) {
        *err = pool_store(p, (const uint8_t *)"no front matter", 15);
        return false;
    }
    size_t close_start = 0, close_end = end;
    bool found_close = false;
    size_t i = first_line_end;
    if (i < end) i++;
    while (i <= end) {
        int nl = meta_nl(p, i, end);
        size_t le = nl >= 0 ? (size_t)nl : end;
        if (le - i == 3 && cmp_bytes(p->base + i, "---", 3) == 0) {
            close_start = i; close_end = le; found_close = true;
            break;
        }
        if (nl < 0) break;
        i = (size_t)nl + 1;
    }
    if (!found_close) {
        *err = pool_store(p, (const uint8_t *)"the front matter is not closed", 30);
        return false;
    }
    size_t body_start = close_end < end ? close_end + 1 : end;
    Sp body = {(uint32_t)body_start, (uint32_t)(end - body_start)};

    out->title = SP_NONE; out->has_date = false; out->date = SP_NONE;
    arr_new(&out->tags, sizeof(Sp));
    out->draft = false; out->has_layout = false; out->layout = SP_NONE;
    out->has_slug = false; out->slug = SP_NONE;
    out->has_summary = false; out->summary = SP_NONE;
    bool seen[7] = {false, false, false, false, false, false, false};
    bool has_title = false;
    size_t line_no = 2;
    i = first_line_end + 1;
    Str msgb; str_new(&msgb);
    while (i < close_start) {
        int nl = meta_nl(p, i, close_start);
        size_t le = nl >= 0 ? (size_t)nl : close_start;
        if (!line_blank(p, i, le) && p->base[i] != '#') {
            size_t koff, klen, voff, vlen;
            if (!line_split(p, i, le, &koff, &klen, &voff, &vlen)) {
                str_clear(&msgb);
                str_cstr(&msgb, "line ");
                str_i64(&msgb, (int64_t)line_no);
                str_cstr(&msgb, " is not \"key: value\"");
                *err = pool_store(p, msgb.data, msgb.len);
                return false;
            }
            const uint8_t *key = p->base + koff;
            int ki = -1;
            switch (klen) {
                case 4:
                    if (eq_bytes(key, "date", 4)) ki = 1;
                    else if (eq_bytes(key, "tags", 4)) ki = 2;
                    else if (eq_bytes(key, "slug", 4)) ki = 5;
                    break;
                case 5:
                    if (eq_bytes(key, "title", 5)) ki = 0;
                    else if (eq_bytes(key, "draft", 5)) ki = 3;
                    break;
                case 6:
                    if (eq_bytes(key, "layout", 6)) ki = 4;
                    break;
                case 7:
                    if (eq_bytes(key, "summary", 7)) ki = 6;
                    break;
                default: break;
            }
            if (ki < 0) {
                uint32_t st = pool_mark(p);
                pool_put(p, (const uint8_t *)"unknown key \"", 13);
                pool_put(p, key, klen);
                pool_u8(p, '"');
                *err = pool_span(p, st);
                return false;
            }
            if (seen[ki]) {
                uint32_t st = pool_mark(p);
                pool_put(p, (const uint8_t *)"key \"", 5);
                pool_put(p, key, klen);
                pool_put(p, (const uint8_t *)"\" is given twice", 16);
                *err = pool_span(p, st);
                return false;
            }
            seen[ki] = true;
            if (vlen == 0) {
                uint32_t st = pool_mark(p);
                pool_put(p, (const uint8_t *)"key \"", 5);
                pool_put(p, key, klen);
                pool_put(p, (const uint8_t *)"\" has no value", 14);
                *err = pool_span(p, st);
                return false;
            }
            switch (ki) {
                case 0: case 6: {
                    const char *e = 0;
                    Sp v;
                    if (!str_value(p, voff, vlen, &v, &e)) {
                        *err = keyed_err(p, META_KEYS[ki], e);
                        return false;
                    }
                    if (ki == 0) { out->title = v; has_title = true; }
                    else { out->summary = v; out->has_summary = true; }
                    break;
                }
                case 1: {
                    if (!date_ok(p, voff, vlen)) {
                        *err = keyed_err(p, META_KEYS[ki], "not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
                        return false;
                    }
                    out->date = (Sp){(uint32_t)voff, (uint32_t)vlen};
                    out->has_date = true;
                    break;
                }
                case 2: {
                    const char *e = 0;
                    if (!tags_parse(p, voff, vlen, &out->tags, &e)) {
                        *err = keyed_err(p, META_KEYS[ki], e);
                        return false;
                    }
                    break;
                }
                case 3: {
                    const uint8_t *raw = p->base + voff;
                    if (vlen == 4 && cmp_bytes(raw, "true", 4) == 0) out->draft = true;
                    else if (vlen == 5 && cmp_bytes(raw, "false", 5) == 0) out->draft = false;
                    else {
                        *err = keyed_err(p, META_KEYS[ki], "true or false");
                        return false;
                    }
                    break;
                }
                case 4: {
                    const uint8_t *raw = p->base + voff;
                    bool ok = vlen > 0;
                    for (size_t k = 0; ok && k < vlen; k++) {
                        uint8_t c = raw[k];
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = false;
                    }
                    if (!ok) {
                        *err = keyed_err(p, META_KEYS[ki], "not a template name");
                        return false;
                    }
                    out->layout = (Sp){(uint32_t)voff, (uint32_t)vlen};
                    out->has_layout = true;
                    break;
                }
                case 5: {
                    if (vlen > 64 || !slug_ok(p, voff, vlen)) {
                        *err = keyed_err(p, META_KEYS[ki], "not a slug");
                        return false;
                    }
                    out->slug = (Sp){(uint32_t)voff, (uint32_t)vlen};
                    out->has_slug = true;
                    break;
                }
            }
        }
        line_no++;
        if (nl < 0) break;
        i = (size_t)nl + 1;
    }
    if (!has_title) {
        *err = pool_store(p, (const uint8_t *)"no title", 8);
        return false;
    }
    out->body = body;
    return true;
}
