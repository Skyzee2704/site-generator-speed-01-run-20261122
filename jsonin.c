/* jsonin.c — zero-copy JSON parser for the fixed site-generator input
 * schema. Input bytes live in the pool; strings without escapes are
 * spans straight into the input. Only strings that contain a `\`
 * escape are decoded into fresh pool bytes. */
#include "zg.h"

static inline uint8_t cur_peek(Cur *c) {
    return c->pos < c->end ? c->pool->base[c->pos] : 0;
}

static inline void cur_ws(Cur *c) {
    while (c->pos < c->end) {
        uint8_t ch = c->pool->base[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->pos++;
        else break;
    }
}

static bool cur_byte(Cur *c, uint8_t b) {
    cur_ws(c);
    if (c->pos < c->end && c->pool->base[c->pos] == b) { c->pos++; return true; }
    return false;
}

static bool cur_lit(Cur *c, const char *lit, size_t n) {
    if (c->end - c->pos >= n && cmp_bytes(c->pool->base + c->pos, lit, n) == 0) {
        c->pos += n;
        return true;
    }
    return false;
}

/* Hex4 reader over a raw buffer (for the in-place unescaper). */
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

static void put_utf8(uint8_t *buf, size_t *w, uint32_t cp) {
    uint8_t tmp[4];
    int n = u8_encode(cp, tmp);
    for (int k = 0; k < n; k++) buf[(*w)++] = tmp[k];
}

/* Handle one escape at buf[*r] == '\\': consume it, write the decoded
 * bytes to buf[*w], advance both cursors. Same semantics as the
 * reference unescaper (lone surrogates -> U+FFFD). */
static bool unescape_one(uint8_t *buf, size_t end, size_t *rp, size_t *wp) {
    size_t r = *rp, w = *wp;
    r++; /* skip '\\' */
    if (r >= end) return false;
    uint8_t e = buf[r++];
    switch (e) {
        case '"': buf[w++] = '"'; break;
        case '\\': buf[w++] = '\\'; break;
        case '/': buf[w++] = '/'; break;
        case 'b': buf[w++] = 0x08; break;
        case 'f': buf[w++] = 0x0c; break;
        case 'n': buf[w++] = '\n'; break;
        case 'r': buf[w++] = '\r'; break;
        case 't': buf[w++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!hex4_buf(buf, end, &r, &cp)) return false;
            if (cp >= 0xd800 && cp < 0xdc00) {
                if (r + 1 < end && buf[r] == '\\' && buf[r + 1] == 'u') {
                    r += 2;
                    uint32_t lo;
                    if (!hex4_buf(buf, end, &r, &lo)) return false;
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        put_utf8(buf, &w, 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00));
                    } else {
                        put_utf8(buf, &w, 0xfffd);
                        if (lo >= 0xd800 && lo < 0xe000) put_utf8(buf, &w, 0xfffd);
                        else put_utf8(buf, &w, lo);
                    }
                } else {
                    put_utf8(buf, &w, 0xfffd);
                }
            } else if (cp >= 0xdc00 && cp < 0xe000) {
                put_utf8(buf, &w, 0xfffd);
            } else {
                put_utf8(buf, &w, cp);
            }
            break;
        }
        default: return false;
    }
    *rp = r; *wp = w;
    return true;
}

static bool parse_string_escaped(Cur *c, size_t start, size_t first_esc, Sp *out);
static bool parse_string(Cur *c, Sp *out) {
    cur_ws(c);
    if (cur_peek(c) != '"') return false;
    c->pos++;
    size_t start = c->pos;
    for (;;) {
        const uint8_t *slice = c->pool->base + c->pos;
        size_t n = c->end - c->pos;
        static const uint8_t needles[2] = {'"', '\\'};
        int j = scan_any(slice, n, 0, needles, 2);
        if (j < 0) return false;
        size_t abs = c->pos + (size_t)j;
        if (c->pool->base[abs] == '"') {
            *out = (Sp){(uint32_t)start, (uint32_t)(abs - start)};
            c->pos = abs + 1;
            return true;
        }
        return parse_string_escaped(c, start, abs, out);
    }
}

/* In-place JSON string unescape. The decoded output is written into the
 * input buffer itself: every escape maps >=2 input bytes to <=4 output
 * bytes (a \uXXXX pair maps 12 to <=6), so the write cursor never passes
 * the read cursor and no byte is overwritten before it is read. Clean
 * 16-byte chunks are copied with one SIMD load/store; chunks containing
 * '"' or '\\' (and the tail) go through the scalar loop. The returned
 * span points into the input region (same pool, before the tail). */
static bool parse_string_escaped(Cur *c, size_t start, size_t first_esc, Sp *out) {
    uint8_t *buf = c->pool->base;
    size_t end = c->end;
    size_t w = first_esc, r = first_esc;
    for (;;) {
        size_t chunk_end;
        if (r + 16 <= end) {
            __m128i v = _mm_loadu_si128((const __m128i *)(buf + r));
            __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
            __m128i bs = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
            if (_mm_movemask_epi8(_mm_or_si128(q, bs)) == 0) {
                /* Forward-overlap copy: dst <= src and the next load is
                 * past this store, so no byte is read after written. */
                _mm_storeu_si128((__m128i *)(buf + w), v);
                w += 16; r += 16;
                continue;
            }
            chunk_end = r + 16;
        } else {
            chunk_end = end;
        }
        /* Scalar: plain bytes, the closing quote, or one escape at a time.
         * An escape may consume bytes past chunk_end; the outer loop then
         * resumes from the new r. */
        while (r < chunk_end) {
            uint8_t ch = buf[r];
            if (ch == '"') {
                c->pos = r + 1;
                *out = (Sp){(uint32_t)start, (uint32_t)(w - start)};
                return true;
            }
            if (ch == '\\') {
                if (!unescape_one(buf, end, &r, &w)) return false;
            } else {
                buf[w++] = ch; r++;
            }
        }
        if (chunk_end >= end) return false;
    }
}

static bool parse_i64(Cur *c, int64_t *out) {
    cur_ws(c);
    bool neg = false;
    if (cur_peek(c) == '-') { neg = true; c->pos++; }
    int64_t v = 0;
    bool any = false;
    while (c->pos < c->end) {
        uint8_t ch = c->pool->base[c->pos];
        if (ch >= '0' && ch <= '9') {
            /* saturating, like the reference */
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
        case '"': { Sp s; return parse_string(c, &s); }
        case '{': {
            c->pos++;
            cur_ws(c);
            if (cur_peek(c) == '}') { c->pos++; return true; }
            for (;;) {
                Sp k;
                if (!parse_string(c, &k)) return false;
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
                for (;;) {
                    cur_ws(c);
                    if (cur_peek(c) == '}') { c->pos++; break; }
                    Sp fk;
                    if (!parse_string(c, &fk)) return false;
                    if (!cur_byte(c, ':')) return false;
                    const uint8_t *fkb = pool_at(c->pool, fk);
                    if (fk.len == 4 && cmp_bytes(fkb, "path", 4) == 0) {
                        if (!parse_string(c, &path)) return false;
                    } else if (fk.len == 7 && cmp_bytes(fkb, "content", 7) == 0) {
                        if (!parse_string(c, &content)) return false;
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

bool json_read_all(Cur *c, Arr *cases_out) {
    if (!cur_byte(c, '[')) return false;
    for (;;) {
        cur_ws(c);
        if (cur_peek(c) == ']') { c->pos++; return true; }
        if (!cur_byte(c, '{')) return false;
        bool has_input = false;
        CaseInput input;
        for (;;) {
            cur_ws(c);
            if (cur_peek(c) == '}') { c->pos++; break; }
            Sp ck;
            if (!parse_string(c, &ck)) return false;
            if (!cur_byte(c, ':')) return false;
            const uint8_t *ckb = pool_at(c->pool, ck);
            if (ck.len == 5 && cmp_bytes(ckb, "input", 5) == 0) {
                if (!json_read_case(c, &input)) return false;
                has_input = true;
            } else {
                if (!skip_value(c)) return false;
            }
            cur_ws(c);
            if (cur_peek(c) == ',') { c->pos++; continue; }
            if (cur_peek(c) == '}') { c->pos++; break; }
            return false;
        }
        if (!has_input) return false;
        CaseInput *slot = arr_slot(cases_out, c->pool);
        *slot = input;
        cur_ws(c);
        if (cur_peek(c) == ',') { c->pos++; continue; }
        if (cur_peek(c) == ']') { c->pos++; return true; }
        return false;
    }
}
