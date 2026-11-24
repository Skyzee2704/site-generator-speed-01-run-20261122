/* tpl.c — the template dialect: tokenize, parse into a node tree,
 * resolve every variable path AT PARSE TIME to a ref, then render
 * straight into the answers buffer. Standalone {% %} tags take their
 * whole line; includes nest at most 8 deep; a for binds its name and
 * `loop` ({index from 1, first, last, length}). */
#include "zg.h"

#define INCLUDE_DEPTH 8

/* ---------------- helpers ---------------- */

static int find_pat(const uint8_t *buf, size_t from, size_t end, const uint8_t *pat, size_t plen) {
    if (plen == 1) {
        return scan_any(buf, end, from, pat, 1);
    }
    if (end < from + plen) return -1;
    for (size_t j = from; j + plen <= end; j++) {
        if (cmp_bytes(buf + j, pat, plen) == 0) return (int)j;
    }
    return -1;
}

/* Trim JS whitespace from both ends of a byte slice. */
static void trim_js(const uint8_t *b, size_t n, size_t *s, size_t *e) {
    size_t a = 0, z = n;
    while (a < z) {
        uint8_t c = b[a];
        if (c < 0x80) {
            if (c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r') a++;
            else break;
        } else {
            uint32_t cp; size_t m = (size_t)u8_decode(b, n, a, &cp);
            if (u_is_space(cp)) a += m;
            else break;
        }
    }
    while (z > a) {
        uint8_t c = b[z - 1];
        if (c < 0x80) {
            if (c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r') z--;
            else break;
        } else {
            size_t k = z - 1;
            while (k > a && (b[k] & 0xc0) == 0x80) k--;
            uint32_t cp; size_t m = (size_t)u8_decode(b, n, k, &cp);
            (void)m;
            if (u_is_space(cp)) z = k;
            else break;
        }
    }
    *s = a; *e = z;
}

static Sp tpl_err(Pool *p, Sp name, const uint8_t *rest, size_t rlen) {
    uint32_t start = pool_mark(p);
    pool_put(p, (const uint8_t *)"template \"", 10);
    pool_copy(p, name);
    pool_put(p, (const uint8_t *)"\": ", 3);
    pool_put(p, rest, rlen);
    return pool_span(p, start);
}

static Sp tpl_err_cstr(Pool *p, Sp name, const char *rest) {
    size_t n = 0; while (rest[n]) n++;
    return tpl_err(p, name, (const uint8_t *)rest, n);
}

/* Append JSON.stringify(`s`) to a Str (quotes included). */
static void json_quote(Str *out, const uint8_t *s, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    str_c(out, '"');
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        switch (c) {
            case '"': str_cstr(out, "\\\""); break;
            case '\\': str_cstr(out, "\\\\"); break;
            case '\n': str_cstr(out, "\\n"); break;
            case '\r': str_cstr(out, "\\r"); break;
            case '\t': str_cstr(out, "\\t"); break;
            case 0x08: str_cstr(out, "\\b"); break;
            case 0x0c: str_cstr(out, "\\f"); break;
            default:
                if (c < 0x20) {
                    str_cstr(out, "\\u00");
                    str_c(out, (uint8_t)HEX[c >> 4]);
                    str_c(out, (uint8_t)HEX[c & 0xf]);
                } else str_c(out, c);
                break;
        }
    }
    str_c(out, '"');
}

static Sp path_err(Pool *p, Sp name, const uint8_t *path, size_t n) {
    Str rest; str_new(&rest);
    json_quote(&rest, path, n);
    str_cstr(&rest, " is not a path");
    return tpl_err(p, name, rest.data, rest.len);
}

static bool path_valid(const uint8_t *b, size_t n) {
    if (n == 0) return false;
    size_t i = 0;
    for (;;) {
        uint8_t c = b[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
        i++;
        while (i < n && ((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= 'a' && b[i] <= 'z')
                         || (b[i] >= '0' && b[i] <= '9') || b[i] == '_')) i++;
        if (i >= n) return true;
        if (b[i] != '.') return false;
        i++;
        if (i >= n) return false;
    }
}

/* ---------------- root fields ---------------- */

int rootfield_id(const uint8_t *name, size_t n) {
    if (n == 3 && cmp_bytes(name, "tag", 3) == 0) return RF_TAG;
    if (n == 4) {
        if (cmp_bytes(name, "site", 4) == 0) return RF_SITE;
        if (cmp_bytes(name, "tags", 4) == 0) return RF_TAGS;
        if (cmp_bytes(name, "page", 4) == 0) return RF_PAGE;
    }
    if (n == 5 && cmp_bytes(name, "posts", 5) == 0) return RF_POSTS;
    if (n == 5 && cmp_bytes(name, "pager", 5) == 0) return RF_PAGER;
    if (n == 6 && cmp_bytes(name, "recent", 6) == 0) return RF_RECENT;
    return -1;
}

uint8_t rootfield_ty(uint8_t rf) {
    switch (rf) {
        case RF_SITE: return TY_SITE;
        case RF_TAGS: return TY_ARR_TAGSUMS;
        case RF_RECENT: return TY_ARR_CARDS;
        case RF_PAGE: return TY_PAGE;
        case RF_POSTS: return TY_ARR_CARDS;
        case RF_PAGER: return TY_PAGER;
        case RF_TAG: return TY_TAG;
        default: return TY_VOID;
    }
}

/* ---------------- resolver ---------------- */

typedef struct { Sp binder; uint8_t elem_ty; } ForBinder;

/* Follow the remaining segments' field types from `ty`; fill the ref's
 * chain. Any missing field makes the whole ref Undef. */
static bool chain_walk(uint8_t ty, const uint8_t *segs, size_t nsegs,
                       const size_t *offs, const size_t *lens, Ref *ref) {
    for (size_t k = 0; k < nsegs; k++) {
        int f = field_id(segs + offs[k], lens[k]);
        if (f < 0 || ref->chain_len >= 15) return false;
        if (ty == TY_VOID) {
            ref->chain[ref->chain_len++] = (uint8_t)f;
            continue;
        }
        uint8_t next = ty_field(ty, (uint8_t)f);
        if (next == TY_VOID) return false;
        ref->chain[ref->chain_len++] = (uint8_t)f;
        ty = next;
    }
    return true;
}

/* Split a path (already validated) into dot segments. Returns count (capped at 16). */
static size_t path_split(const uint8_t *b, size_t n, size_t *offs, size_t *lens) {
    size_t cnt = 0, s = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || b[i] == '.') {
            if (cnt < 16) { offs[cnt] = s; lens[cnt] = i - s; }
            cnt++;
            s = i + 1;
        }
    }
    return cnt;
}

/* Resolve a path's segments to a (Ref, Ty). */
static void slot_resolve(Pool *p, Arr *for_stack, const uint8_t *path, size_t n,
                         Ref *ref_out, uint8_t *ty_out) {
    size_t offs[16], lens[16];
    size_t nseg = path_split(path, n, offs, lens);
    ref_out->kind = REF_VOID;
    ref_out->root_field = 0; ref_out->offset = 0; ref_out->is_loop = 0;
    ref_out->chain_len = 0;
    ref_out->name = SP_NONE;
    if (nseg > 16) { *ty_out = TY_VOID; return; }
    const uint8_t *first = path + offs[0];
    size_t flen = lens[0];
    int rf = rootfield_id(first, flen);
    if (rf >= 0) {
        uint8_t ty = rootfield_ty((uint8_t)rf);
        ref_out->kind = REF_ROOT;
        ref_out->root_field = (uint8_t)rf;
        if (nseg == 1) { *ty_out = ty; return; }
        if (chain_walk(ty, path, nseg - 1, offs + 1, lens + 1, ref_out)) { *ty_out = ty; return; }
        ref_out->kind = REF_VOID;
        *ty_out = TY_VOID;
        return;
    }
    if (flen == 4 && cmp_bytes(first, "loop", 4) == 0 && for_stack->len > 0) {
        ref_out->kind = REF_SCOPE;
        ref_out->offset = 0;
        ref_out->is_loop = 1;
        ref_out->name = pool_store(p, (const uint8_t *)"loop", 4);
        if (nseg == 1) { *ty_out = TY_LOOP; return; }
        if (chain_walk(TY_LOOP, path, nseg - 1, offs + 1, lens + 1, ref_out)) { *ty_out = TY_LOOP; return; }
        ref_out->kind = REF_VOID;
        *ty_out = TY_VOID;
        return;
    }
    for (size_t q = for_stack->len; q-- > 0;) {
        ForBinder *fb = arr_at(for_stack, q);
        const uint8_t *bn = pool_at(p, fb->binder);
        if (bn && fb->binder.len == flen && cmp_bytes(bn, first, flen) == 0) {
            ref_out->kind = REF_SCOPE;
            ref_out->offset = (uint8_t)(for_stack->len - 1 - q);
            ref_out->is_loop = 0;
            ref_out->name = fb->binder;
            if (nseg == 1) { *ty_out = fb->elem_ty; return; }
            if (chain_walk(fb->elem_ty, path, nseg - 1, offs + 1, lens + 1, ref_out)) { *ty_out = fb->elem_ty; return; }
            ref_out->kind = REF_VOID;
            *ty_out = TY_VOID;
            return;
        }
    }
    /* Dynamic: a caller variable (or an unknown name). */
    ref_out->kind = REF_DYN;
    ref_out->name = pool_store(p, first, flen);
    for (size_t k = 1; k < nseg; k++) {
        int f = field_id(path + offs[k], lens[k]);
        if (f < 0 || ref_out->chain_len >= 15) {
            ref_out->kind = REF_VOID;
            *ty_out = TY_VOID;
            return;
        }
        ref_out->chain[ref_out->chain_len++] = (uint8_t)f;
    }
    *ty_out = TY_VOID;
}

/* Pre-escape a template text span at compile time: the rendered output is
 * the JSON-escaped form, a pure function of the static text, so escaping
 * once here replaces one escape scan per render. */
static Sp esc_json_span(Pool *p, const uint8_t *b, size_t n) {
    uint32_t start = pool_mark(p);
    json_escape_into(p, b, n);
    return pool_span(p, start);
}

/* ---------------- tokenize ---------------- */

enum TokKind { TK_TEXT, TK_VAR, TK_TAG };
typedef struct { uint8_t kind; Sp span; bool raw; } Tok;

static bool lex_tokens(Pool *p, Sp name, Sp src, Arr *toks_out, Sp *err) {
    arr_new(toks_out, sizeof(Tok));
    size_t off = src.off, len = src.len, end = off + len;
    static Str text;
    str_clear(&text);
    size_t i = off;
    static const uint8_t brace[1] = {'{'};
    while (i < end) {
        int open = scan_any(p->base, end, i, brace, 1);
        if (open < 0) {
            str_add(&text, p->base + i, end - i);
            break;
        }
        if ((size_t)open == end - 1) {
            str_add(&text, p->base + i, end - i);
            break;
        }
        uint8_t next = p->base[open + 1];
        if (next != '{' && next != '%') {
            str_add(&text, p->base + i, (size_t)open + 1 - i);
            i = (size_t)open + 1;
            continue;
        }
        str_add(&text, p->base + i, (size_t)open - i);
        if (next == '{') {
            bool raw = p->base[open + 2] == '{';
            const uint8_t *pat = raw ? (const uint8_t *)"}}}" : (const uint8_t *)"}}";
            size_t plen = raw ? 3 : 2;
            int close = find_pat(p->base, (size_t)open + (raw ? 3 : 2), end, pat, plen);
            if (close < 0) {
                *err = tpl_err_cstr(p, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
                return false;
            }
            if (text.len > 0) {
                Tok *t = arr_slot(toks_out, p);
                t->kind = TK_TEXT; t->span = esc_json_span(p, text.data, text.len); t->raw = false;
                str_clear(&text);
            }
            size_t s, e;
            trim_js(p->base + open + (raw ? 3 : 2), (size_t)close - ((size_t)open + (raw ? 3 : 2)), &s, &e);
            /* Vendor tokenize() validates a var/raw path immediately (pathOf),
               so a bad path here beats any later error in the source. */
            const uint8_t *inner = p->base + open + (raw ? 3 : 2) + s;
            size_t inner_n = e - s;
            if (!path_valid(inner, inner_n)) {
                *err = path_err(p, name, inner, inner_n);
                return false;
            }
            Sp path_s = pool_store(p, inner, inner_n);
            Tok *t = arr_slot(toks_out, p);
            t->kind = TK_VAR; t->span = path_s; t->raw = raw;
            i = (size_t)close + plen;
            continue;
        }
        /* {% %} */
        static const uint8_t pct[2] = {'%', '}'};
        int close = find_pat(p->base, (size_t)open + 2, end, pct, 2);
        if (close < 0) {
            *err = tpl_err_cstr(p, name, "a {% is not closed");
            return false;
        }
        size_t tag_end = (size_t)close + 2;
        size_t line_start = off;
        if ((size_t)open > off) {
            size_t j = (size_t)open - 1;
            for (;;) {
                if (p->base[j] == '\n') { line_start = j + 1; break; }
                if (j == off) break;
                j--;
            }
        }
        static const uint8_t nl1[1] = {'\n'};
        int nl = scan_any(p->base, end, tag_end, nl1, 1);
        size_t line_end = nl >= 0 ? (size_t)nl : end;
        bool before_ok = true, after_ok = true;
        for (size_t k = line_start; k < (size_t)open; k++) {
            if (p->base[k] != ' ' && p->base[k] != '\t') { before_ok = false; break; }
        }
        for (size_t k = tag_end; k < line_end; k++) {
            if (p->base[k] != ' ' && p->base[k] != '\t') { after_ok = false; break; }
        }
        bool standalone = before_ok && after_ok;
        if (standalone) {
            size_t cut = (size_t)open - line_start;
            if (cut >= text.len) str_clear(&text);
            else text.len -= cut;
        }
        if (text.len > 0) {
            Tok *t = arr_slot(toks_out, p);
            t->kind = TK_TEXT; t->span = esc_json_span(p, text.data, text.len); t->raw = false;
            str_clear(&text);
        }
        size_t s, e;
        trim_js(p->base + open + 2, (size_t)close - ((size_t)open + 2), &s, &e);
        Sp tag_s = pool_store(p, p->base + open + 2 + s, e - s);
        Tok *t = arr_slot(toks_out, p);
        t->kind = TK_TAG; t->span = tag_s; t->raw = false;
        i = standalone ? (nl >= 0 ? (size_t)nl + 1 : end) : tag_end;
    }
    if (text.len > 0) {
        Tok *t = arr_slot(toks_out, p);
        t->kind = TK_TEXT; t->span = esc_json_span(p, text.data, text.len); t->raw = false;
    }
    return true;
}

/* ---------------- tag parsers ---------------- */

/* Parse `for NAME in PATH` (NAME = [A-Za-z_][A-Za-z0-9_]*, PATH = \S+). */
static bool for_parse(const uint8_t *tag, size_t n, size_t *name_off, size_t *name_len,
                      size_t *path_off, size_t *path_len) {
    if (n < 4 || cmp_bytes(tag, "for ", 4) != 0) return false;
    const uint8_t *rest = tag + 4;
    size_t rn = n - 4;
    if (rn == 0) return false;
    uint8_t c0 = rest[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return false;
    size_t i = 1;
    while (i < rn && ((rest[i] >= 'A' && rest[i] <= 'Z') || (rest[i] >= 'a' && rest[i] <= 'z')
                      || (rest[i] >= '0' && rest[i] <= '9') || rest[i] == '_')) i++;
    if (rn - i < 4 || cmp_bytes(rest + i, " in ", 4) != 0) return false;
    size_t q = i + 4;
    if (q >= rn) return false;
    for (size_t k = q; k < rn; k++) {
        uint8_t c = rest[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r') return false;
    }
    *name_off = 4; *name_len = i;
    *path_off = 4 + q; *path_len = rn - q;
    return true;
}

/* Parse `if [not ]PATH`. */
static bool if_parse(const uint8_t *tag, size_t n, bool *not_out, size_t *path_off, size_t *path_len) {
    if (n < 3 || cmp_bytes(tag, "if ", 3) != 0) return false;
    const uint8_t *rest = tag + 3;
    size_t rn = n - 3;
    bool not = false;
    if (rn >= 4 && cmp_bytes(rest, "not ", 4) == 0) { not = true; rest += 4; rn -= 4; }
    if (rn == 0) return false;
    for (size_t k = 0; k < rn; k++) {
        uint8_t c = rest[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == 0x0b || c == 0x0c || c == '\r') return false;
    }
    *not_out = not;
    *path_off = (size_t)(rest - tag); *path_len = rn;
    return true;
}

/* Parse `include "NAME"` (NAME = [a-z0-9-]+). */
static bool include_parse(const uint8_t *tag, size_t n, size_t *name_off, size_t *name_len) {
    if (n < 11 || cmp_bytes(tag, "include \"", 9) != 0) return false;
    if (tag[n - 1] != '"') return false;
    size_t inner_off = 9, inner_len = n - 10;
    if (inner_len == 0) return false;
    for (size_t k = 0; k < inner_len; k++) {
        uint8_t c = tag[inner_off + k];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    *name_off = inner_off; *name_len = inner_len;
    return true;
}

/* ---------------- parse ---------------- */

enum OpenKind { OP_ROOT, OP_FOR, OP_IF };
typedef struct {
    uint8_t kind;
    Sp binder; Ref list;          /* FOR */
    Ref slot; bool not;           /* IF */
    bool has_else; bool in_else;  /* IF */
    Arr then_nodes; Arr else_nodes; /* IF */
    Arr body;                     /* ROOT / FOR */
} OpenFrame;

static Arr *frame_body_mut(OpenFrame *o) {
    if (o->kind == OP_IF) return o->in_else ? &o->else_nodes : &o->then_nodes;
    return &o->body;
}

static const char *frame_kind_name(uint8_t kind) {
    return kind == OP_FOR ? "for" : kind == OP_IF ? "if" : "root";
}

static void node_init(Node *s) {
    fill_bytes(s, 0, sizeof(Node));
}

bool tpl_parse(Pool *p, Sp name, Sp src, Arr *nodes_out, Sp *err) {
    Arr toks;
    if (!lex_tokens(p, name, src, &toks, err)) return false;
    Arr stack; arr_new(&stack, sizeof(OpenFrame));
    Arr for_stack; arr_new(&for_stack, sizeof(ForBinder));
    OpenFrame *root = arr_slot(&stack, p);
    root->kind = OP_ROOT; root->binder = SP_NONE; root->has_else = false; root->in_else = false;
    arr_new(&root->then_nodes, sizeof(Node));
    arr_new(&root->else_nodes, sizeof(Node));
    arr_new(&root->body, sizeof(Node));
    for (size_t ti = 0; ti < toks.len; ti++) {
        Tok *tok = arr_at(&toks, ti);
        OpenFrame *top = arr_at(&stack, stack.len - 1);
        if (tok->kind == TK_TEXT) {
            Node *s = arr_slot(frame_body_mut(top), p);
            node_init(s);
            s->kind = ND_TEXT; s->u.text = tok->span;
            continue;
        }
        if (tok->kind == TK_VAR) {
            const uint8_t *path = pool_at(p, tok->span);
            size_t n = tok->span.len;
            if (!path_valid(path, n)) {
                *err = path_err(p, name, path, n);
                return false;
            }
            Ref ref; uint8_t ty;
            slot_resolve(p, &for_stack, path, n, &ref, &ty);
            Node *s = arr_slot(frame_body_mut(top), p);
            node_init(s);
            s->kind = ND_VAR; s->u.var.ref = ref; s->raw = tok->raw ? 1 : 0;
            continue;
        }
        /* Tag. */
        const uint8_t *tag = pool_at(p, tok->span);
        size_t n = tok->span.len;
        size_t no, nl, po, pl;
        if (for_parse(tag, n, &no, &nl, &po, &pl)) {
            Sp path_s = pool_store(p, tag + po, pl);
            const uint8_t *pb = pool_at(p, path_s);
            if (!path_valid(pb, pl)) {
                *err = path_err(p, name, pb, pl);
                return false;
            }
            Ref list_ref; uint8_t list_ty;
            slot_resolve(p, &for_stack, pb, pl, &list_ref, &list_ty);
            uint8_t elem_ty = ty_elem(list_ty);
            Sp binder_s = pool_store(p, tag + no, nl);
            ForBinder *fb = arr_slot(&for_stack, p);
            fb->binder = binder_s; fb->elem_ty = elem_ty;
            OpenFrame *o = arr_slot(&stack, p);
            o->kind = OP_FOR; o->binder = binder_s; o->list = list_ref;
            o->has_else = false; o->in_else = false;
            arr_new(&o->then_nodes, sizeof(Node));
            arr_new(&o->else_nodes, sizeof(Node));
            arr_new(&o->body, sizeof(Node));
            continue;
        }
        bool not; size_t ipo, ipl;
        if (if_parse(tag, n, &not, &ipo, &ipl)) {
            Sp path_s = pool_store(p, tag + ipo, ipl);
            const uint8_t *pb = pool_at(p, path_s);
            if (!path_valid(pb, ipl)) {
                *err = path_err(p, name, pb, ipl);
                return false;
            }
            Ref ref; uint8_t ty;
            slot_resolve(p, &for_stack, pb, ipl, &ref, &ty);
            OpenFrame *o = arr_slot(&stack, p);
            o->kind = OP_IF; o->slot = ref; o->not = not;
            o->has_else = false; o->in_else = false;
            arr_new(&o->then_nodes, sizeof(Node));
            arr_new(&o->else_nodes, sizeof(Node));
            arr_new(&o->body, sizeof(Node));
            continue;
        }
        if (n == 4 && cmp_bytes(tag, "else", 4) == 0) {
            if (top->kind == OP_IF && !top->has_else) {
                top->has_else = true;
                top->in_else = true;
            } else {
                *err = tpl_err_cstr(p, name, "an else outside an if");
                return false;
            }
            continue;
        }
        if ((n == 6 && cmp_bytes(tag, "endfor", 6) == 0) || (n == 5 && cmp_bytes(tag, "endif", 5) == 0)) {
            bool is_for = n == 6;
            uint8_t want = is_for ? OP_FOR : OP_IF;
            if (top->kind != want) {
                Str msg; str_new(&msg);
                str_cstr(&msg, "an ");
                str_cstr(&msg, is_for ? "endfor" : "endif");
                str_cstr(&msg, " with no open ");
                str_cstr(&msg, is_for ? "for" : "if");
                *err = tpl_err(p, name, msg.data, msg.len);
                return false;
            }
            OpenFrame done = *top;
            stack.len--;
            if (is_for) for_stack.len--;
            Node *s = arr_slot(frame_body_mut(arr_at(&stack, stack.len - 1)), p);
            node_init(s);
            if (is_for) {
                s->kind = ND_FOR; s->u.nfor.binder = done.binder; s->u.nfor.ref = done.list;
                s->u.nfor.body = done.body;
            } else {
                s->kind = ND_IF; s->u.nif.ref = done.slot; s->neg = done.not ? 1 : 0;
                s->u.nif.then_nodes = done.then_nodes;
                s->u.nif.else_nodes = done.else_nodes;
                s->has_else = done.has_else ? 1 : 0;
            }
            continue;
        }
        size_t io, il;
        if (include_parse(tag, n, &io, &il)) {
            Sp name_s = pool_store(p, tag + io, il);
            Node *s = arr_slot(frame_body_mut(top), p);
            node_init(s);
            s->kind = ND_INCLUDE; s->u.ninc.name = name_s; s->u.ninc.tpl_idx = -1;
            continue;
        }
        Str msg; str_new(&msg);
        str_cstr(&msg, "{% ");
        str_add(&msg, tag, n);
        str_cstr(&msg, " %} is not a tag");
        *err = tpl_err(p, name, msg.data, msg.len);
        return false;
    }
    if (stack.len > 1) {
        OpenFrame *top = arr_at(&stack, stack.len - 1);
        Str msg; str_new(&msg);
        str_cstr(&msg, "a ");
        str_cstr(&msg, frame_kind_name(top->kind));
        str_cstr(&msg, " is not closed");
        *err = tpl_err(p, name, msg.data, msg.len);
        return false;
    }
    OpenFrame *r = arr_at(&stack, 0);
    *nodes_out = r->body;
    return true;
}

/* ---------------- render ---------------- */

/* ---- include cache: dependency analysis ----
 * A template's rendering is a function of (template, depth, the values of
 * its deps). Deps are the SLOT_DYN names and SLOT_ROOT fields the template
 * reads, transitively through its includes. SLOT_FRAME slots are internal:
 * their values derive from the template's own for-lists, whose slots are
 * themselves walked. A template that reads a caller `loop` (SLOT_DYN
 * "loop") is not cacheable: its output depends on the caller's loop state.
 * The cache stores the rendered bytes (already JSON-escaped) in the output
 * pool; a hit copies them, a miss renders in place and records the span. */

static Val root_val(Scope *scopes, uint8_t field);

static void dep_add(Pool *p, Arr *deps, TplNeed d) {
    for (size_t i = 0; i < deps->len; i++) {
        TplNeed *e = arr_at(deps, i);
        if (e->kind != d.kind) continue;
        if (d.kind == 0) {
            if (e->name.len == d.name.len && cmp_bytes(pool_at(p, e->name), pool_at(p, d.name), d.name.len) == 0) return;
        } else if (e->root_field == d.root_field) return;
    }
    TplNeed *slot = arr_slot(deps, p);
    *slot = d;
}

static void dep_collect_ref(Pool *p, Ref *rf, Arr *deps, bool *bad_loop) {
    if (rf->kind == REF_DYN) {
        if (rf->name.len == 4 && cmp_bytes(pool_at(p, rf->name), "loop", 4) == 0) { *bad_loop = true; return; }
        dep_add(p, deps, (TplNeed){0, rf->name, 0});
    } else if (rf->kind == REF_ROOT) {
        dep_add(p, deps, (TplNeed){1, SP_NONE, rf->root_field});
    }
}

static void dep_collect_nodes(Pool *p, Arr *nodes, Arr *deps, bool *bad_loop) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        switch (s->kind) {
            case ND_VAR:
                dep_collect_ref(p, &s->u.var.ref, deps, bad_loop);
                break;
            case ND_IF:
                dep_collect_ref(p, &s->u.nif.ref, deps, bad_loop);
                dep_collect_nodes(p, &s->u.nif.then_nodes, deps, bad_loop);
                dep_collect_nodes(p, &s->u.nif.else_nodes, deps, bad_loop);
                break;
            case ND_FOR:
                dep_collect_ref(p, &s->u.nfor.ref, deps, bad_loop);
                dep_collect_nodes(p, &s->u.nfor.body, deps, bad_loop);
                break;
            default:
                break;
        }
    }
}

static int tpl_find_name(Pool *p, Arr *templates, Sp name) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->name.len == name.len && cmp_bytes(pool_at(p, t->name), pool_at(p, name), name.len) == 0) return (int)k;
    }
    return -1;
}

static void include_resolve_all(Pool *p, Arr *templates, Arr *nodes) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        if (s->kind == ND_INCLUDE) s->u.ninc.tpl_idx = tpl_find_name(p, templates, s->u.ninc.name);
        else if (s->kind == ND_IF) {
            include_resolve_all(p, templates, &s->u.nif.then_nodes);
            include_resolve_all(p, templates, &s->u.nif.else_nodes);
        } else if (s->kind == ND_FOR) {
            include_resolve_all(p, templates, &s->u.nfor.body);
        }
    }
}

static bool dep_union_includes(Pool *p, Arr *templates, Arr *nodes, Arr *deps, bool *any_bad) {
    bool changed = false;
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        if (s->kind == ND_INCLUDE) {
            int k = tpl_find_name(p, templates, s->u.ninc.name);
            if (k >= 0) {
                Tpl *u = arr_at(templates, (size_t)k);
                if (!u->cacheable) *any_bad = true;
                size_t before = deps->len;
                for (size_t j = 0; j < u->needs.len; j++) {
                    TplNeed *d = arr_at(&u->needs, j);
                    dep_add(p, deps, *d);
                }
                if (deps->len != before) changed = true;
            }
        } else if (s->kind == ND_IF) {
            if (dep_union_includes(p, templates, &s->u.nif.then_nodes, deps, any_bad)) changed = true;
            if (dep_union_includes(p, templates, &s->u.nif.else_nodes, deps, any_bad)) changed = true;
        } else if (s->kind == ND_FOR) {
            if (dep_union_includes(p, templates, &s->u.nfor.body, deps, any_bad)) changed = true;
        }
    }
    return changed;
}

static bool nodes_use_card_excerpt(Arr *nodes) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        if (s->kind == ND_VAR || s->kind == ND_IF || s->kind == ND_FOR) {
            Ref *rf = s->kind == ND_VAR ? &s->u.var.ref : (s->kind == ND_IF ? &s->u.nif.ref : &s->u.nfor.ref);
            if (!(rf->kind == REF_ROOT && rf->root_field == RF_PAGE)) {
                for (uint8_t c = 0; c < rf->chain_len; c++) {
                    if (rf->chain[c] == F_EXCERPT) return true;
                }
            }
            if (s->kind == ND_IF) {
                if (nodes_use_card_excerpt(&s->u.nif.then_nodes)) return true;
                if (nodes_use_card_excerpt(&s->u.nif.else_nodes)) return true;
            } else if (s->kind == ND_FOR) {
                if (nodes_use_card_excerpt(&s->u.nfor.body)) return true;
            }
        }
    }
    return false;
}

static bool nodes_include_uses_card_excerpt(Arr *templates, Arr *nodes) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        if (s->kind == ND_INCLUDE && s->u.ninc.tpl_idx >= 0) {
            Tpl *u = arr_at(templates, (size_t)s->u.ninc.tpl_idx);
            if (u->uses_card_excerpt) return true;
        } else if (s->kind == ND_IF) {
            if (nodes_include_uses_card_excerpt(templates, &s->u.nif.then_nodes)) return true;
            if (nodes_include_uses_card_excerpt(templates, &s->u.nif.else_nodes)) return true;
        } else if (s->kind == ND_FOR) {
            if (nodes_include_uses_card_excerpt(templates, &s->u.nfor.body)) return true;
        }
    }
    return false;
}

void tpl_analyze(Pool *p, Arr *templates) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        arr_new(&t->needs, sizeof(TplNeed));
        t->cacheable = 1;
        t->uses_card_excerpt = nodes_use_card_excerpt(&t->nodes) ? 1 : 0;
        bool bad = false;
        dep_collect_nodes(p, &t->nodes, &t->needs, &bad);
        if (bad) t->cacheable = 0;
    }
    for (int iter = 0; iter < 16; iter++) {
        bool changed = false;
        for (size_t k = 0; k < templates->len; k++) {
            Tpl *t = arr_at(templates, k);
            bool bad = false;
            if (dep_union_includes(p, templates, &t->nodes, &t->needs, &bad)) changed = true;
            if (bad && t->cacheable) { t->cacheable = 0; changed = true; }
        }
        if (!changed) break;
    }
    uint8_t next_card_slot = 0;
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->needs.len > 8) t->cacheable = 0;
        t->storable = 1;
        uint8_t ndyn = 0;
        Sp dyn_name = SP_NONE;
        for (size_t j = 0; j < t->needs.len; j++) {
            TplNeed *d = arr_at(&t->needs, j);
            if (d->kind == 1) {
                if (d->root_field != RF_SITE && d->root_field != RF_TAGS
                    && d->root_field != RF_RECENT) { t->storable = 0; break; }
            } else {
                ndyn++;
                dyn_name = d->name;
            }
        }
        t->cache_mode = 0;
        t->card_slot = 0;
        t->dyn_name = SP_NONE;
        if (t->cacheable && t->storable) {
            if (ndyn == 0) {
                t->cache_mode = 1; /* Global: depends only on (tpl_idx, depth) */
            } else if (ndyn == 1 && next_card_slot < 8) {
                t->cache_mode = 2; /* 1 dyn dep: O(1) fast path for O_CARD at depth==0 */
                t->card_slot = next_card_slot++;
                t->dyn_name = dyn_name;
            } else {
                t->cache_mode = 3;
            }
        }
    }
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        include_resolve_all(p, templates, &t->nodes);
    }
    for (int iter = 0; iter < 16; iter++) {
        bool changed = false;
        for (size_t k = 0; k < templates->len; k++) {
            Tpl *t = arr_at(templates, k);
            if (!t->uses_card_excerpt && nodes_include_uses_card_excerpt(templates, &t->nodes)) {
                t->uses_card_excerpt = 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

static bool nodes_allow_pre_json_md(Arr *nodes) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *s = arr_at(nodes, i);
        if (s->kind == ND_VAR && !s->raw && s->u.var.ref.chain_len > 0) {
            uint8_t last = s->u.var.ref.chain[s->u.var.ref.chain_len - 1];
            if (last == F_CONTENT || last == F_EXCERPT || last == F_HTML) return false;
        } else if (s->kind == ND_IF) {
            if (!nodes_allow_pre_json_md(&s->u.nif.then_nodes)) return false;
            if (!nodes_allow_pre_json_md(&s->u.nif.else_nodes)) return false;
        } else if (s->kind == ND_FOR) {
            if (!nodes_allow_pre_json_md(&s->u.nfor.body)) return false;
        }
    }
    return true;
}

bool tpl_can_pre_json_md(Arr *templates) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (!nodes_allow_pre_json_md(&t->nodes)) return false;
    }
    return true;
}

static inline Val dep_value_dyn(Ctx *c, Scope *scopes, size_t nscopes, Sp name) {
    const uint8_t *nm = pool_at(c->pool, name);
    for (size_t fi = nscopes; fi-- > 0;) {
        Scope *fr = &scopes[fi];
        if (!fr->is_root) {
            if (fr->name.len == name.len && eq_bytes(pool_at(c->pool, fr->name), nm, name.len)) return fr->value;
        }
    }
    int rf = rootfield_id(nm, name.len);
    if (rf >= 0) return root_val(scopes, (uint8_t)rf);
    return val_void();
}

static inline Val dep_value(Ctx *c, Scope *scopes, size_t nscopes, TplNeed *d) {
    if (d->kind == 1) return root_val(scopes, d->root_field);
    return dep_value_dyn(c, scopes, nscopes, d->name);
}

static inline uint64_t val_hash(uint64_t h, Val v) {
    uint64_t w0, w1;
    __builtin_memcpy(&w0, &v, 8);
    __builtin_memcpy(&w1, (const uint8_t *)&v + 8, 8);
    h = (h ^ w0) * 0x9e3779b97f4a7c15ULL;
    h = (h ^ w1) * 0x9e3779b97f4a7c15ULL;
    return h ^ (h >> 32);
}

static inline bool val_equal(Val a, Val b) {
    uint64_t a0, a1, b0, b1;
    __builtin_memcpy(&a0, &a, 8); __builtin_memcpy(&a1, (const uint8_t *)&a + 8, 8);
    __builtin_memcpy(&b0, &b, 8); __builtin_memcpy(&b1, (const uint8_t *)&b + 8, 8);
    return a0 == b0 && a1 == b1;
}

void tpl_cache_open(Ctx *c, Pool *p, Arr *templates, size_t npages) {
    size_t cap = 8192;
    c->tpl_index = pool_alloc(p, cap * sizeof(int32_t));
    fill_bytes(c->tpl_index, 0xff, cap * sizeof(int32_t));
    c->tpl_mask = cap - 1;
    arr_new(&c->tpl_entries, sizeof(TplHit));
    size_t gsz = templates->len * INCLUDE_DEPTH;
    if (gsz > 0) {
        c->tpl_global = pool_alloc(p, gsz * sizeof(Sp));
        fill_bytes(c->tpl_global, 0xff, gsz * sizeof(Sp));
    } else {
        c->tpl_global = 0;
    }
    size_t ncards = 0;
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->cache_mode == 2 && (size_t)t->card_slot + 1 > ncards) ncards = (size_t)t->card_slot + 1;
    }
    if (ncards > 0 && npages > 0) {
        size_t csz = ncards * npages;
        c->tpl_card_cache = pool_alloc(p, csz * sizeof(Sp));
        fill_bytes(c->tpl_card_cache, 0xff, csz * sizeof(Sp));
    } else {
        c->tpl_card_cache = 0;
    }
}

static inline Val root_val(Scope *scopes, uint8_t field) {
    Scope *root = &scopes[0];
    if (!root->is_root) return val_void();
    switch (field) {
        case RF_SITE: return val_obj(O_SITE, 0);
        case RF_TAGS: return val_arr(A_TAGSUMS, 0);
        case RF_RECENT: return val_arr(A_CARDS, 0);
        case RF_PAGE: return root->page >= 0 ? val_obj(O_PAGE, (uint32_t)root->page) : val_void();
        case RF_POSTS: return root->posts >= 0 ? val_arr(A_CARDS, (uint32_t)root->posts) : val_void();
        case RF_PAGER: return root->pager >= 0 ? val_obj(O_PAGER, (uint32_t)root->pager) : val_void();
        case RF_TAG: return root->tag >= 0 ? val_obj(O_TAG, (uint32_t)root->tag) : val_void();
        default: return val_void();
    }
}

static inline Val walk_chain(Ctx *c, Val v, const uint8_t *chain, uint8_t clen) {
    if (__builtin_expect(clen == 1, 1)) {
        return val_field(c, v, chain[0]);
    }
    for (uint8_t k = 0; k < clen; k++) {
        v = val_field(c, v, chain[k]);
        if (v.kind == K_VOID) return val_void();
    }
    return v;
}

static Val dyn_eval(Ctx *c, Scope *scopes, size_t nscopes, const uint8_t *name, size_t nlen, const uint8_t *chain, uint8_t clen) {
    Val v = val_void();
    for (size_t fi = nscopes; fi-- > 0;) {
        Scope *fr = &scopes[fi];
        if (!fr->is_root) {
            if (nlen == 4 && eq_bytes(name, "loop", 4)) { v = val_loop(fr->lp); break; }
            if (fr->name.len == nlen && eq_bytes(pool_at(c->pool, fr->name), name, nlen)) { v = fr->value; break; }
        } else {
            int rf = rootfield_id(name, nlen);
            if (rf >= 0) { v = root_val(scopes, (uint8_t)rf); break; }
        }
    }
    if (clen == 0 || v.kind == K_VOID) return v;
    return walk_chain(c, v, chain, clen);
}

Val tpl_eval(Ctx *c, Scope *scopes, size_t nscopes, Ref *ref) {
    switch (ref->kind) {
        case REF_VOID:
            return val_void();
        case REF_ROOT: {
            Val v = root_val(scopes, ref->root_field);
            if (ref->chain_len == 0) return v;
            return walk_chain(c, v, ref->chain, ref->chain_len);
        }
        case REF_SCOPE: {
            size_t idx = nscopes >= (size_t)1 + ref->offset ? nscopes - 1 - ref->offset : 0;
            Scope *fr = &scopes[idx];
            if (!fr->is_root && fr->name.len == ref->name.len
                && eq_bytes(pool_at(c->pool, fr->name), pool_at(c->pool, ref->name), ref->name.len)) {
                Val v = ref->is_loop ? val_loop(fr->lp) : fr->value;
                if (ref->chain_len == 0) return v;
                return walk_chain(c, v, ref->chain, ref->chain_len);
            }
            return dyn_eval(c, scopes, nscopes, pool_at(c->pool, ref->name), ref->name.len, ref->chain, ref->chain_len);
        }
        case REF_DYN:
            return dyn_eval(c, scopes, nscopes, pool_at(c->pool, ref->name), ref->name.len, ref->chain, ref->chain_len);
        default:
            return val_void();
    }
}

void tpl_emit(Ctx *c, Arr *templates, Arr *nodes, Scope *scopes, size_t *nscopes,
              Pool *out, Str *scratch, uint32_t depth) {
    Node *arr = (Node *)nodes->items;
    size_t nn = nodes->len;
    for (size_t si = 0; si < nn; si++) {
        Node *seg = &arr[si];
        switch (seg->kind) {
            case ND_TEXT:
                pool_put(out, c->pool->base + seg->u.text.off, seg->u.text.len);
                break;
            case ND_VAR: {
                Val v = tpl_eval(c, scopes, *nscopes, &seg->u.var.ref);
                if (__builtin_expect(v.kind == K_STR_JSON, 1)) {
                    pool_put(out, c->pool->base + v.v.s.off, v.v.s.len);
                } else if (v.kind == K_CUR_HTML) {
                    pool_put(out, c->cur_html + v.v.s.off, v.v.s.len);
                } else {
                    val_write(c, v, out, !seg->raw, scratch);
                }
                break;
            }
            case ND_IF: {
                Val v = tpl_eval(c, scopes, *nscopes, &seg->u.nif.ref);
                bool t = val_true(c, v);
                Arr *branch = 0;
                if (t != (bool)seg->neg) branch = &seg->u.nif.then_nodes;
                else if (seg->has_else) branch = &seg->u.nif.else_nodes;
                if (branch) tpl_emit(c, templates, branch, scopes, nscopes, out, scratch, depth);
                break;
            }
            case ND_FOR: {
                Val v = tpl_eval(c, scopes, *nscopes, &seg->u.nfor.ref);
                if (v.kind == K_ARR) {
                    size_t len = ctx_arr_len(c, v.akind, v.idx);
                    for (size_t i = 0; i < len; i++) {
                        Val elem = ctx_arr_elem(c, v.akind, v.idx, i);
                        LoopInfo lp = {(int64_t)i + 1, i == 0, i + 1 == len, (int64_t)len};
                        Scope *fr = &scopes[(*nscopes)++];
                        fr->is_root = false;
                        fr->name = seg->u.nfor.binder;
                        fr->value = elem;
                        fr->lp = lp;
                        fr->page = -1; fr->posts = -1; fr->pager = -1; fr->tag = -1;
                        tpl_emit(c, templates, &seg->u.nfor.body, scopes, nscopes, out, scratch, depth);
                        (*nscopes)--;
                    }
                }
                break;
            }
            case ND_INCLUDE: {
                uint32_t eff_depth = depth + (uint32_t)seg->u.ninc.depth_add;
                if (eff_depth < INCLUDE_DEPTH && seg->u.ninc.tpl_idx >= 0) {
                    size_t k = (size_t)seg->u.ninc.tpl_idx;
                    Tpl *t = arr_at(templates, k);
                    if (t->cache_mode == 1) {
                        Sp *slot = &c->tpl_global[k * INCLUDE_DEPTH + eff_depth];
                        if (__builtin_expect(slot->off != 0xffffffffu, 1)) {
                            pool_put(out, pool_at(c->cache_pool, *slot), slot->len);
                        } else if (out == c->cache_pool) {
                            uint32_t start = pool_mark(out);
                            tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, eff_depth + 1);
                            *slot = pool_span(out, start);
                        } else {
                            uint32_t start = pool_mark(c->cache_pool);
                            tpl_emit(c, templates, &t->nodes, scopes, nscopes, c->cache_pool, scratch, eff_depth + 1);
                            *slot = pool_span(c->cache_pool, start);
                            pool_put(out, pool_at(c->cache_pool, *slot), slot->len);
                        }
                        break;
                    }
                    if (t->cache_mode == 2 && eff_depth == 0 && c->tpl_card_cache) {
                        Val dv = dep_value_dyn(c, scopes, *nscopes, t->dyn_name);
                        if (__builtin_expect(dv.kind == K_OBJ && dv.akind == O_CARD && dv.idx < c->pages.len, 1)) {
                            Sp *slot = &c->tpl_card_cache[(size_t)t->card_slot * c->pages.len + dv.idx];
                            if (__builtin_expect(slot->off != 0xffffffffu, 1)) {
                                pool_put(out, pool_at(c->cache_pool, *slot), slot->len);
                            } else if (out == c->cache_pool) {
                                uint32_t start = pool_mark(out);
                                tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, eff_depth + 1);
                                *slot = pool_span(out, start);
                            } else {
                                uint32_t start = pool_mark(c->cache_pool);
                                tpl_emit(c, templates, &t->nodes, scopes, nscopes, c->cache_pool, scratch, eff_depth + 1);
                                *slot = pool_span(c->cache_pool, start);
                                pool_put(out, pool_at(c->cache_pool, *slot), slot->len);
                            }
                            break;
                        }
                    }
                    if (t->cache_mode != 0 && c->tpl_index) {
                        uint64_t h = 1469598103934665603ULL;
                        h ^= (uint64_t)k; h *= 1099511628211ULL;
                        h ^= (uint64_t)(eff_depth + 1); h *= 1099511628211ULL;
                        Val keys[8];
                        uint8_t nk = (uint8_t)t->needs.len;
                        for (uint8_t di = 0; di < nk; di++) {
                            TplNeed *d = arr_at(&t->needs, di);
                            keys[di] = dep_value(c, scopes, *nscopes, d);
                            h = val_hash(h, keys[di]);
                        }
                        size_t mask = c->tpl_mask;
                        size_t slot = (size_t)h & mask;
                        bool hit = false;
                        while (c->tpl_index[slot] >= 0) {
                            TplHit *e = arr_at(&c->tpl_entries, (size_t)c->tpl_index[slot]);
                            if (e->hash == h && e->tpl_idx == (uint32_t)k && e->depth == (uint8_t)(eff_depth + 1) && e->nkeys == nk) {
                                bool eq = true;
                                for (uint8_t di = 0; di < nk; di++) if (!val_equal(e->keys[di], keys[di])) { eq = false; break; }
                                if (eq) {
                                    pool_put(out, pool_at(c->cache_pool, e->bytes), e->bytes.len); hit = true; break;
                                }
                            }
                            slot = (slot + 1) & mask;
                        }
                        if (!hit) {
                            Sp span;
                            if (out == c->cache_pool) {
                                uint32_t start = pool_mark(out);
                                tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, eff_depth + 1);
                                span = pool_span(out, start);
                            } else {
                                uint32_t start = pool_mark(c->cache_pool);
                                tpl_emit(c, templates, &t->nodes, scopes, nscopes, c->cache_pool, scratch, eff_depth + 1);
                                span = pool_span(c->cache_pool, start);
                                pool_put(out, pool_at(c->cache_pool, span), span.len);
                            }
                            if (c->tpl_entries.len < 4096) {
                                TplHit *e = arr_slot(&c->tpl_entries, c->pool);
                                e->hash = h; e->tpl_idx = (uint32_t)k; e->depth = (uint8_t)(eff_depth + 1);
                                e->nkeys = nk; e->bytes = span;
                                for (uint8_t di = 0; di < nk; di++) e->keys[di] = keys[di];
                                size_t s2 = (size_t)h & mask;
                                while (c->tpl_index[s2] >= 0) s2 = (s2 + 1) & mask;
                                c->tpl_index[s2] = (int32_t)(c->tpl_entries.len - 1);
                            }
                        }
                    } else {
                        tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, eff_depth + 1);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

/* Compile-time template optimization:
 * - Constant-fold `{{ site.title }}` / `{{ site.base_url }}` (both raw & escaped).
 * - Pre-render `cache_mode == 1` (global) includes into static text blobs.
 * - Inline small `cache_mode == 0` includes (like `head`, `meta`, `toc`, `aside-*`)
 *   while preserving exact `INCLUDE_DEPTH` semantics via `depth_add`.
 * - Coalesce all adjacent `ND_TEXT` nodes into single contiguous spans. */
static void opt_push_text(Pool *p, Arr *out, const uint8_t *bytes, size_t len) {
    if (len == 0) return;
    if (out->len > 0) {
        Node *prev = arr_at(out, out->len - 1);
        if (prev->kind == ND_TEXT) {
            if (prev->u.text.off + prev->u.text.len == p->len) {
                pool_put(p, bytes, len);
                prev->u.text.len += (uint32_t)len;
                return;
            }
            uint32_t st = pool_mark(p);
            pool_copy(p, prev->u.text);
            pool_put(p, bytes, len);
            prev->u.text = pool_span(p, st);
            return;
        }
    }
    Node *nd = arr_slot(out, p);
    nd->kind = ND_TEXT;
    nd->u.text = pool_store(p, bytes, len);
}

static void opt_nodes(Ctx *c, Arr *templates, Arr *orig_nodes, Arr *in_nodes, Arr *out,
                      uint32_t depth, Str *scratch, uint32_t *budget,
                      bool in_card_tpl, Sp card_dyn_name) {
    Pool *p = c->pool;
    for (size_t i = 0; i < in_nodes->len; i++) {
        Node *seg = arr_at(in_nodes, i);
        switch (seg->kind) {
            case ND_TEXT:
                opt_push_text(p, out, pool_at(p, seg->u.text), seg->u.text.len);
                break;
            case ND_VAR: {
                Ref *rf = &seg->u.var.ref;
                if (rf->kind == REF_ROOT && rf->root_field == RF_SITE && rf->chain_len == 1
                    && (rf->chain[0] == F_TITLE || rf->chain[0] == F_BASE_URL)) {
                    Val v = val_field(c, val_obj(O_SITE, 0), rf->chain[0]);
                    uint32_t st = pool_mark(c->cache_pool);
                    val_write(c, v, c->cache_pool, !seg->raw, scratch);
                    Sp rendered = pool_span(c->cache_pool, st);
                    opt_push_text(p, out, pool_at(c->cache_pool, rendered), rendered.len);
                    break;
                }
                Node *nd = arr_slot(out, p);
                *nd = *seg;
                break;
            }
            case ND_IF: {
                Arr then_opt, else_opt;
                arr_new(&then_opt, sizeof(Node));
                arr_new(&else_opt, sizeof(Node));
                opt_nodes(c, templates, orig_nodes, &seg->u.nif.then_nodes, &then_opt, depth, scratch, budget, in_card_tpl, card_dyn_name);
                if (seg->has_else) {
                    opt_nodes(c, templates, orig_nodes, &seg->u.nif.else_nodes, &else_opt, depth, scratch, budget, in_card_tpl, card_dyn_name);
                }
                Node *nd = arr_slot(out, p);
                *nd = *seg;
                nd->u.nif.then_nodes = then_opt;
                nd->u.nif.else_nodes = else_opt;
                break;
            }
            case ND_FOR: {
                Arr body_opt;
                arr_new(&body_opt, sizeof(Node));
                opt_nodes(c, templates, orig_nodes, &seg->u.nfor.body, &body_opt, depth, scratch, budget, in_card_tpl, card_dyn_name);
                Node *nd = arr_slot(out, p);
                *nd = *seg;
                nd->u.nfor.body = body_opt;
                break;
            }
            case ND_INCLUDE: {
                uint32_t eff_depth = depth + (uint32_t)seg->u.ninc.depth_add;
                if (eff_depth >= INCLUDE_DEPTH || seg->u.ninc.tpl_idx < 0) {
                    /* Always emits nothing! */
                    break;
                }
                size_t k = (size_t)seg->u.ninc.tpl_idx;
                Tpl *u = arr_at(templates, k);
                if (u->cache_mode == 1) {
                    Sp *slot = &c->tpl_global[k * INCLUDE_DEPTH + eff_depth];
                    if (slot->off == 0xffffffffu) {
                        Scope frames[16];
                        frames[0].is_root = true;
                        frames[0].name = SP_NONE; frames[0].value = val_void();
                        frames[0].lp = (LoopInfo){0, false, false, 0};
                        frames[0].page = -1; frames[0].posts = -1; frames[0].pager = -1; frames[0].tag = -1;
                        size_t nf = 1;
                        uint32_t st = pool_mark(c->cache_pool);
                        tpl_emit(c, templates, &u->nodes, frames, &nf, c->cache_pool, scratch, eff_depth + 1);
                        *slot = pool_span(c->cache_pool, st);
                    }
                    opt_push_text(p, out, pool_at(c->cache_pool, *slot), slot->len);
                    break;
                }
                Arr *u_orig = arr_at(orig_nodes, k);
                bool can_inline_same_card = in_card_tpl && u->cache_mode == 2
                    && u->dyn_name.len == card_dyn_name.len
                    && eq_bytes(pool_at(p, u->dyn_name), pool_at(p, card_dyn_name), card_dyn_name.len);
                if ((u->cache_mode == 0 || can_inline_same_card) && u_orig->len <= 48 && *budget > u_orig->len) {
                    *budget -= (uint32_t)u_orig->len;
                    opt_nodes(c, templates, orig_nodes, u_orig, out, eff_depth + 1, scratch, budget, in_card_tpl, card_dyn_name);
                    break;
                }
                Node *nd = arr_slot(out, p);
                *nd = *seg;
                nd->u.ninc.depth_add = (uint8_t)eff_depth;
                break;
            }
            default:
                break;
        }
    }
}

/* ---------------- Flat bytecode compiler & direct-pointer streaming VM ---------------- */

enum OpKind {
    OP_TEXT,
    OP_PAGE_STR,
    OP_PAGE_CONTENT,
    OP_PAGE_INT,
    OP_PAGE_LINK_STR,
    OP_IF_PAGE_JMP,
    OP_TAG_STR,
    OP_TAG_INT,
    OP_IF_TAG_JMP,
    OP_PAGER_STR,
    OP_PAGER_INT,
    OP_IF_PAGER_JMP,
    OP_SCOPE_STR,
    OP_SCOPE_INT,
    OP_IF_SCOPE_JMP,
    OP_IF_LOOP_JMP,
    OP_JMP,
    OP_FOR_BEGIN,
    OP_FOR_NEXT,
    OP_INCLUDE_CARD,
    OP_VAR_GEN,
    OP_IF_GEN_JMP,
    OP_INCLUDE_GEN
};

enum ForListKind {
    FL_PAGE_TAGS = 0,
    FL_PAGE_TOC  = 1,
    FL_POSTS     = 2,
    FL_TAG_POSTS = 3,
    FL_TAGS      = 4,
    FL_RECENT    = 5,
    FL_GEN       = 6
};

typedef struct {
    uint8_t op;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint32_t off;  /* OP_TEXT offset / OP_INCLUDE_CARD tpl_idx / jmp target */
    uint32_t len;  /* OP_TEXT len / OP_INCLUDE_CARD card_slot */
    Node *node;    /* fallback node pointer for *_GEN / OP_FOR_BEGIN */
} Instr;

typedef struct {
    Sp binder;
    uint8_t obj_kind; /* O_TAGREF, O_TOC, O_CARD, O_TAGSUM, or 0xff (unknown) */
    uint32_t begin_pc;
    bool need_scope;
} CompLoop;

typedef struct {
    Ctx *c;
    Pool *p;
    Arr *templates;
    Arr *code;
    bool is_card_mode;
    Sp card_dyn;
    CompLoop loops[8];
    int ldepth;
} CompCtx;

static void comp_mark_scopes(CompCtx *cc) {
    for (int i = 0; i < cc->ldepth; i++) cc->loops[i].need_scope = true;
}

static Instr *comp_emit(CompCtx *cc, uint8_t op) {
    Instr *ins = arr_slot(cc->code, cc->p);
    ins->op = op;
    return ins;
}

static bool is_card_field_ok(uint8_t f) {
    return f == F_TITLE || f == F_URL || f == F_DATE || f == F_DATE_LONG
        || f == F_SUMMARY || f == F_EXCERPT || f == F_READING_MINUTES || f == F_TAGS;
}

/* Check if ref accesses cur_page (either RF_PAGE in normal mode, or card_dyn in card mode). */
static bool ref_is_page_obj(CompCtx *cc, Ref *rf) {
    if (!cc->is_card_mode) return rf->kind == REF_ROOT && rf->root_field == RF_PAGE;
    if (rf->kind == REF_DYN && rf->name.len == cc->card_dyn.len
        && eq_bytes(pool_at(cc->p, rf->name), pool_at(cc->p, cc->card_dyn), cc->card_dyn.len)) {
        /* Make sure no enclosing for-loop shadows card_dyn */
        for (int i = 0; i < cc->ldepth; i++) {
            if (cc->loops[i].binder.len == cc->card_dyn.len
                && eq_bytes(pool_at(cc->p, cc->loops[i].binder), pool_at(cc->p, cc->card_dyn), cc->card_dyn.len))
                return false;
        }
        return true;
    }
    return false;
}

static int comp_find_scope(CompCtx *cc, Ref *rf) {
    if (rf->kind != REF_SCOPE) return -1;
    if ((int)rf->offset >= cc->ldepth) return -1;
    int slot = cc->ldepth - 1 - (int)rf->offset;
    if (rf->is_loop) return slot;
    if (cc->loops[slot].binder.len == rf->name.len
        && eq_bytes(pool_at(cc->p, cc->loops[slot].binder), pool_at(cc->p, rf->name), rf->name.len))
        return slot;
    return -1;
}

static void comp_nodes(CompCtx *cc, Arr *nodes) {
    for (size_t i = 0; i < nodes->len; i++) {
        Node *seg = arr_at(nodes, i);
        switch (seg->kind) {
            case ND_TEXT: {
                if (seg->u.text.len == 0) break;
                Instr *ins = comp_emit(cc, OP_TEXT);
                ins->off = seg->u.text.off;
                ins->len = seg->u.text.len;
                break;
            }
            case ND_VAR: {
                Ref *rf = &seg->u.var.ref;
                uint8_t esc = seg->raw ? 0 : 1;
                if (ref_is_page_obj(cc, rf)) {
                    if (rf->chain_len == 1) {
                        uint8_t f = rf->chain[0];
                        if (!cc->is_card_mode || is_card_field_ok(f)) {
                            if (f == F_TITLE || f == F_URL || f == F_DATE || f == F_DATE_LONG
                                || f == F_SUMMARY || f == F_SLUG
                                || (f == F_EXCERPT && cc->c->md_pre_json)) {
                                Instr *ins = comp_emit(cc, OP_PAGE_STR);
                                ins->a = f; ins->b = esc;
                                break;
                            }
                            if (f == F_CONTENT && cc->c->md_pre_json && !cc->is_card_mode) {
                                Instr *ins = comp_emit(cc, OP_PAGE_CONTENT);
                                ins->b = esc;
                                break;
                            }
                            if (f == F_READING_MINUTES) {
                                Instr *ins = comp_emit(cc, OP_PAGE_INT);
                                ins->a = f;
                                break;
                            }
                        }
                    } else if (rf->chain_len == 2 && !cc->is_card_mode
                               && (rf->chain[0] == F_NEWER || rf->chain[0] == F_OLDER)) {
                        uint8_t f2 = rf->chain[1];
                        if (f2 == F_TITLE || f2 == F_URL || f2 == F_DATE || f2 == F_DATE_LONG) {
                            Instr *ins = comp_emit(cc, OP_PAGE_LINK_STR);
                            ins->a = rf->chain[0]; ins->b = f2; ins->c = esc;
                            break;
                        }
                    }
                } else if (rf->kind == REF_ROOT && rf->root_field == RF_TAG && rf->chain_len == 1) {
                    uint8_t f = rf->chain[0];
                    if (f == F_NAME || f == F_SLUG || f == F_URL) {
                        Instr *ins = comp_emit(cc, OP_TAG_STR);
                        ins->a = f; ins->b = esc;
                        break;
                    }
                    if (f == F_COUNT) {
                        Instr *ins = comp_emit(cc, OP_TAG_INT);
                        ins->a = f;
                        break;
                    }
                } else if (rf->kind == REF_ROOT && rf->root_field == RF_PAGER && rf->chain_len == 1) {
                    uint8_t f = rf->chain[0];
                    if (f == F_URL || f == F_NEWER_URL || f == F_OLDER_URL) {
                        Instr *ins = comp_emit(cc, OP_PAGER_STR);
                        ins->a = f; ins->b = esc;
                        break;
                    }
                    if (f == F_NUMBER || f == F_TOTAL) {
                        Instr *ins = comp_emit(cc, OP_PAGER_INT);
                        ins->a = f;
                        break;
                    }
                } else if (rf->kind == REF_SCOPE && rf->chain_len == 1) {
                    int slot = comp_find_scope(cc, rf);
                    if (slot >= 0) {
                        uint8_t f = rf->chain[0];
                        if (rf->is_loop) {
                            if (f == F_INDEX || f == F_LENGTH) {
                                Instr *ins = comp_emit(cc, OP_SCOPE_INT);
                                ins->a = (uint8_t)slot; ins->b = f; ins->c = K_LOOP;
                                break;
                            }
                        } else {
                            uint8_t ok = cc->loops[slot].obj_kind;
                            if ((ok == O_TAGREF && (f == F_NAME || f == F_SLUG || f == F_URL))
                                || (ok == O_TAGSUM && (f == F_NAME || f == F_SLUG || f == F_URL))
                                || (ok == O_TOC && (f == F_ID || (f == F_HTML && cc->c->md_pre_json)))
                                || (ok == O_CARD && (f == F_TITLE || f == F_URL || f == F_DATE || f == F_DATE_LONG
                                                     || f == F_SUMMARY || (f == F_EXCERPT && cc->c->md_pre_json)))) {
                                Instr *ins = comp_emit(cc, OP_SCOPE_STR);
                                ins->a = (uint8_t)slot; ins->b = f; ins->c = ok; ins->len = esc;
                                break;
                            }
                            if ((ok == O_TOC && f == F_LEVEL)
                                || (ok == O_TAGSUM && f == F_COUNT)
                                || (ok == O_CARD && f == F_READING_MINUTES)) {
                                Instr *ins = comp_emit(cc, OP_SCOPE_INT);
                                ins->a = (uint8_t)slot; ins->b = f; ins->c = ok;
                                break;
                            }
                        }
                    }
                }
                comp_mark_scopes(cc);
                Instr *ins = comp_emit(cc, OP_VAR_GEN);
                ins->node = seg;
                break;
            }
            case ND_IF: {
                Ref *rf = &seg->u.nif.ref;
                uint32_t jpc = (uint32_t)cc->code->len;
                bool spec = false;
                if (ref_is_page_obj(cc, rf)) {
                    if (rf->chain_len == 0 && !cc->is_card_mode) {
                        Instr *ins = comp_emit(cc, OP_IF_PAGE_JMP);
                        ins->a = 0xff; ins->b = seg->neg; spec = true;
                    } else if (rf->chain_len == 1) {
                        uint8_t f = rf->chain[0];
                        if ((!cc->is_card_mode || is_card_field_ok(f))
                            && (f == F_DATE || f == F_TAGS || f == F_TOC || f == F_NEWER || f == F_OLDER
                                || f == F_SUMMARY || f == F_TITLE || f == F_IS_POST || f == F_EXCERPT)) {
                            Instr *ins = comp_emit(cc, OP_IF_PAGE_JMP);
                            ins->a = f; ins->b = seg->neg; spec = true;
                        }
                    }
                } else if (rf->kind == REF_ROOT && rf->root_field == RF_TAG) {
                    if (rf->chain_len == 0) {
                        Instr *ins = comp_emit(cc, OP_IF_TAG_JMP);
                        ins->a = 0xff; ins->b = seg->neg; spec = true;
                    } else if (rf->chain_len == 1 && (rf->chain[0] == F_POSTS || rf->chain[0] == F_NAME)) {
                        Instr *ins = comp_emit(cc, OP_IF_TAG_JMP);
                        ins->a = rf->chain[0]; ins->b = seg->neg; spec = true;
                    }
                } else if (rf->kind == REF_ROOT && rf->root_field == RF_PAGER && rf->chain_len == 1
                           && (rf->chain[0] == F_NEWER_URL || rf->chain[0] == F_OLDER_URL || rf->chain[0] == F_URL)) {
                    Instr *ins = comp_emit(cc, OP_IF_PAGER_JMP);
                    ins->a = rf->chain[0]; ins->b = seg->neg; spec = true;
                } else if (rf->kind == REF_SCOPE && rf->chain_len == 1) {
                    int slot = comp_find_scope(cc, rf);
                    if (slot >= 0) {
                        uint8_t f = rf->chain[0];
                        if (rf->is_loop && (f == F_FIRST || f == F_LAST || f == F_INDEX)) {
                            Instr *ins = comp_emit(cc, OP_IF_LOOP_JMP);
                            ins->a = (uint8_t)slot; ins->b = f; ins->c = seg->neg; spec = true;
                        } else if (!rf->is_loop) {
                            uint8_t ok = cc->loops[slot].obj_kind;
                            if ((ok == O_TAGREF && (f == F_URL || f == F_NAME))
                                || (ok == O_CARD && (f == F_SUMMARY || f == F_DATE || f == F_TAGS))) {
                                Instr *ins = comp_emit(cc, OP_IF_SCOPE_JMP);
                                ins->a = (uint8_t)slot; ins->b = f; ins->c = ok; ins->len = seg->neg; spec = true;
                            }
                        }
                    }
                }
                if (!spec) {
                    comp_mark_scopes(cc);
                    Instr *ins = comp_emit(cc, OP_IF_GEN_JMP);
                    ins->node = seg;
                }
                comp_nodes(cc, &seg->u.nif.then_nodes);
                if (seg->has_else) {
                    uint32_t ejpc = (uint32_t)cc->code->len;
                    comp_emit(cc, OP_JMP);
                    ((Instr *)arr_at(cc->code, jpc))->off = (uint32_t)cc->code->len;
                    comp_nodes(cc, &seg->u.nif.else_nodes);
                    ((Instr *)arr_at(cc->code, ejpc))->off = (uint32_t)cc->code->len;
                } else {
                    ((Instr *)arr_at(cc->code, jpc))->off = (uint32_t)cc->code->len;
                }
                break;
            }
            case ND_FOR: {
                if (cc->ldepth >= 8) {
                    comp_mark_scopes(cc);
                    /* Rare deep nesting: wrap as generic */
                    Instr *ins = comp_emit(cc, OP_INCLUDE_GEN);
                    ins->node = seg;
                    break;
                }
                Ref *rf = &seg->u.nfor.ref;
                uint8_t lkind = FL_GEN;
                uint8_t okind = 0xff;
                if (ref_is_page_obj(cc, rf) && rf->chain_len == 1) {
                    if (rf->chain[0] == F_TAGS) { lkind = FL_PAGE_TAGS; okind = O_TAGREF; }
                    else if (rf->chain[0] == F_TOC && !cc->is_card_mode) { lkind = FL_PAGE_TOC; okind = O_TOC; }
                } else if (rf->kind == REF_ROOT) {
                    if (rf->root_field == RF_POSTS && rf->chain_len == 0) { lkind = FL_POSTS; okind = O_CARD; }
                    else if (rf->root_field == RF_TAG && rf->chain_len == 1 && rf->chain[0] == F_POSTS) { lkind = FL_TAG_POSTS; okind = O_CARD; }
                    else if (rf->root_field == RF_TAGS && rf->chain_len == 0) { lkind = FL_TAGS; okind = O_TAGSUM; }
                    else if (rf->root_field == RF_RECENT && rf->chain_len == 0) { lkind = FL_RECENT; okind = O_CARD; }
                }
                if (lkind == FL_GEN) comp_mark_scopes(cc);
                int slot = cc->ldepth++;
                uint32_t bpc = (uint32_t)cc->code->len;
                cc->loops[slot].binder = seg->u.nfor.binder;
                cc->loops[slot].obj_kind = okind;
                cc->loops[slot].begin_pc = bpc;
                cc->loops[slot].need_scope = false;
                Instr *beg = comp_emit(cc, OP_FOR_BEGIN);
                beg->a = (uint8_t)slot;
                beg->b = lkind;
                beg->node = seg;
                uint32_t body_pc = (uint32_t)cc->code->len;
                comp_nodes(cc, &seg->u.nfor.body);
                bool ns = cc->loops[slot].need_scope;
                cc->ldepth--;
                Instr *nxt = comp_emit(cc, OP_FOR_NEXT);
                nxt->a = (uint8_t)slot;
                nxt->c = ns ? 1 : 0;
                nxt->off = body_pc;
                Instr *beg_fix = arr_at(cc->code, bpc);
                beg_fix->c = ns ? 1 : 0;
                beg_fix->off = (uint32_t)cc->code->len;
                break;
            }
            case ND_INCLUDE: {
                if (seg->u.ninc.tpl_idx >= 0 && seg->u.ninc.depth_add == 0 && cc->c->tpl_card_cache) {
                    Tpl *u = arr_at(cc->templates, (size_t)seg->u.ninc.tpl_idx);
                    if (u->cache_mode == 2 && cc->ldepth > 0) {
                        /* Check if innermost matching binder is an O_CARD loop */
                        for (int s = cc->ldepth - 1; s >= 0; s--) {
                            if (cc->loops[s].binder.len == u->dyn_name.len
                                && eq_bytes(pool_at(cc->p, cc->loops[s].binder), pool_at(cc->p, u->dyn_name), u->dyn_name.len)) {
                                if (cc->loops[s].obj_kind == O_CARD) {
                                    Instr *ins = comp_emit(cc, OP_INCLUDE_CARD);
                                    ins->a = (uint8_t)s;
                                    ins->b = u->card_slot;
                                    ins->off = (uint32_t)seg->u.ninc.tpl_idx;
                                    ins->node = seg;
                                    goto inc_done;
                                }
                                break;
                            }
                        }
                    }
                }
                comp_mark_scopes(cc);
                {
                    Instr *ins = comp_emit(cc, OP_INCLUDE_GEN);
                    ins->node = seg;
                }
            inc_done:
                break;
            }
            default:
                break;
        }
    }
}

static void tpl_compile_into(Ctx *c, Arr *templates, Arr *nodes, Arr *code_out, bool is_card_mode, Sp card_dyn) {
    arr_new(code_out, sizeof(Instr));
    CompCtx cc;
    cc.c = c;
    cc.p = c->pool;
    cc.templates = templates;
    cc.code = code_out;
    cc.is_card_mode = is_card_mode;
    cc.card_dyn = card_dyn;
    cc.ldepth = 0;
    comp_nodes(&cc, nodes);
}

typedef struct {
    uint8_t akind;
    uint32_t idx;
    uint32_t i;
    uint32_t len;
    uint32_t start; /* for Range arrays */
    const uint32_t *card_ptr; /* for A_CARDS */
    Sp binder;
} VmLoop;

#define VM_EMIT_BYTES(src, slen) do { \
    size_t _n = (slen); \
    if (__builtin_expect(dst + _n <= dst_end, 1)) { \
        copy_fast(dst, (src), _n); \
        dst += _n; \
    } else { \
        out->len = (size_t)(dst - out->base); \
        pool_put_slow(out, (src), _n); \
        dst = out->base + out->len; \
    } \
} while (0)

#define VM_EMIT_STR(sp, is_clean, do_esc) do { \
    Sp _sp = (sp); \
    if (__builtin_expect((is_clean), 1)) { \
        VM_EMIT_BYTES(pbase + _sp.off, _sp.len); \
    } else { \
        out->len = (size_t)(dst - out->base); \
        if (do_esc) html_json_escape_into(out, pbase + _sp.off, _sp.len); \
        else json_escape_into(out, pbase + _sp.off, _sp.len); \
        dst = out->base + out->len; \
    } \
} while (0)

#define VM_EMIT_INT(val_i64) do { \
    int64_t _iv = (val_i64); \
    if (__builtin_expect(_iv >= 0 && _iv < 10 && dst < dst_end, 1)) { \
        *dst++ = (uint8_t)('0' + _iv); \
    } else if (__builtin_expect(_iv >= 10 && _iv < 100 && dst + 2 <= dst_end, 1)) { \
        dst[0] = (uint8_t)('0' + _iv / 10); \
        dst[1] = (uint8_t)('0' + _iv % 10); \
        dst += 2; \
    } else { \
        out->len = (size_t)(dst - out->base); \
        pool_i64(out, _iv); \
        dst = out->base + out->len; \
    } \
} while (0)

static void tpl_exec_code(Ctx *c, Arr *templates, Arr *code, Page *cur_page,
                          Scope *scopes, size_t *nscopes, Pool *out, Str *scratch, uint32_t depth) {
    Instr *ins_arr = (Instr *)code->items;
    size_t nins = code->len;
    const uint8_t *pbase = c->pool->base;
    uint8_t *dst = out->base + out->len;
    uint8_t *dst_end = out->base + out->cap;
    Scope *root = &scopes[0];
    Page *pages = (Page *)c->pages.items;
    Tag *tags = (Tag *)c->tags.items;
    Tag *cur_tag = (root->tag >= 0) ? &tags[root->tag] : 0;
    Pager *cur_pager = (root->pager >= 0) ? (Pager *)c->pagers.items + root->pager : 0;
    TagRef *tagrefs = (TagRef *)c->tagref_store.items;
    Range *tagref_ranges = (Range *)c->tagref_arrays.items;
    Toc *tocs_lazy = (Toc *)c->cur_toc->items;
    Toc *tocs_store = (Toc *)c->toc_store.items;
    Range *toc_ranges = (Range *)c->toc_arrays.items;
    VmLoop loops[8];

    size_t pc = 0;
    while (pc < nins) {
        Instr *ins = &ins_arr[pc++];
        switch (ins->op) {
            case OP_TEXT:
                VM_EMIT_BYTES(pbase + ins->off, ins->len);
                break;
            case OP_PAGE_STR: {
                if (__builtin_expect(!cur_page, 0)) break;
                switch (ins->a) {
                    case F_TITLE:     VM_EMIT_STR(cur_page->title, (cur_page->clean_mask & 1), ins->b); break;
                    case F_URL:       VM_EMIT_STR(cur_page->url, (cur_page->clean_mask & 2), ins->b); break;
                    case F_DATE:      VM_EMIT_BYTES(pbase + cur_page->date.off, cur_page->date.len); break;
                    case F_DATE_LONG: VM_EMIT_BYTES(pbase + cur_page->date_long.off, cur_page->date_long.len); break;
                    case F_SUMMARY:   VM_EMIT_STR(cur_page->summary, (cur_page->clean_mask & 4), ins->b); break;
                    case F_SLUG:      VM_EMIT_BYTES(pbase + cur_page->slug.off, cur_page->slug.len); break;
                    case F_EXCERPT:   VM_EMIT_BYTES(pbase + cur_page->excerpt.off, cur_page->excerpt.len); break;
                    default: break;
                }
                break;
            }
            case OP_PAGE_CONTENT: {
                if (__builtin_expect(!cur_page, 0)) break;
                if (c->lazy_md) {
                    VM_EMIT_BYTES(c->cur_html, c->cur_html_len);
                } else {
                    VM_EMIT_BYTES(pbase + cur_page->html.off, cur_page->html.len);
                }
                break;
            }
            case OP_PAGE_INT: {
                if (__builtin_expect(!cur_page, 0)) break;
                VM_EMIT_INT(cur_page->reading_minutes);
                break;
            }
            case OP_PAGE_LINK_STR: {
                if (__builtin_expect(!cur_page, 0)) break;
                int32_t li = (ins->a == F_NEWER) ? cur_page->newer : cur_page->older;
                if (__builtin_expect(li < 0, 0)) break;
                Page *lp = &pages[li];
                switch (ins->b) {
                    case F_TITLE:     VM_EMIT_STR(lp->title, (lp->clean_mask & 1), ins->c); break;
                    case F_URL:       VM_EMIT_STR(lp->url, (lp->clean_mask & 2), ins->c); break;
                    case F_DATE:      VM_EMIT_BYTES(pbase + lp->date.off, lp->date.len); break;
                    case F_DATE_LONG: VM_EMIT_BYTES(pbase + lp->date_long.off, lp->date_long.len); break;
                    default: break;
                }
                break;
            }
            case OP_IF_PAGE_JMP: {
                bool truth = false;
                if (cur_page) {
                    switch (ins->a) {
                        case 0xff:      truth = true; break;
                        case F_DATE:    truth = cur_page->date.len > 0; break;
                        case F_TAGS:    truth = tagref_ranges[cur_page->tags].len > 0; break;
                        case F_TOC:     truth = c->lazy_md ? (c->cur_toc->len > 0) : (toc_ranges[cur_page->toc].len > 0); break;
                        case F_NEWER:   truth = cur_page->newer >= 0; break;
                        case F_OLDER:   truth = cur_page->older >= 0; break;
                        case F_SUMMARY: truth = cur_page->summary.len > 0; break;
                        case F_TITLE:   truth = cur_page->title.len > 0; break;
                        case F_IS_POST: truth = cur_page->is_post; break;
                        case F_EXCERPT: truth = cur_page->excerpt.len > 0; break;
                        default: break;
                    }
                }
                if (truth == (bool)ins->b) pc = ins->off;
                break;
            }
            case OP_TAG_STR: {
                if (__builtin_expect(!cur_tag, 0)) break;
                if (ins->a == F_NAME) VM_EMIT_STR(cur_tag->name, (cur_tag->clean_mask & 1), ins->b);
                else if (ins->a == F_URL) VM_EMIT_STR(cur_tag->url, (cur_tag->clean_mask & 2), ins->b);
                else VM_EMIT_BYTES(pbase + cur_tag->slug.off, cur_tag->slug.len);
                break;
            }
            case OP_TAG_INT: {
                if (__builtin_expect(!cur_tag, 0)) break;
                VM_EMIT_INT((int64_t)cur_tag->posts.len);
                break;
            }
            case OP_IF_TAG_JMP: {
                bool truth = false;
                if (cur_tag) {
                    if (ins->a == 0xff) truth = true;
                    else if (ins->a == F_POSTS) truth = cur_tag->posts.len > 0;
                    else if (ins->a == F_NAME) truth = cur_tag->name.len > 0;
                }
                if (truth == (bool)ins->b) pc = ins->off;
                break;
            }
            case OP_PAGER_STR: {
                if (__builtin_expect(!cur_pager, 0)) break;
                Sp s = (ins->a == F_URL) ? cur_pager->url : (ins->a == F_NEWER_URL ? cur_pager->newer_url : cur_pager->older_url);
                VM_EMIT_STR(s, c->site_base_clean, ins->b);
                break;
            }
            case OP_PAGER_INT: {
                if (__builtin_expect(!cur_pager, 0)) break;
                VM_EMIT_INT(ins->a == F_NUMBER ? cur_pager->number : cur_pager->total);
                break;
            }
            case OP_IF_PAGER_JMP: {
                bool truth = false;
                if (cur_pager) {
                    Sp s = (ins->a == F_NEWER_URL) ? cur_pager->newer_url : (ins->a == F_OLDER_URL ? cur_pager->older_url : cur_pager->url);
                    truth = s.len > 0;
                }
                if (truth == (bool)ins->b) pc = ins->off;
                break;
            }
            case OP_SCOPE_STR: {
                VmLoop *lp = &loops[ins->a];
                uint8_t esc = (uint8_t)ins->len;
                switch (ins->c) {
                    case O_TAGREF: {
                        TagRef *r = &tagrefs[lp->start + lp->i];
                        if (ins->b == F_NAME) VM_EMIT_STR(r->name, (r->clean_mask & 1), esc);
                        else if (ins->b == F_URL) VM_EMIT_STR(r->url, (r->clean_mask & 2), esc);
                        else VM_EMIT_BYTES(pbase + r->slug.off, r->slug.len);
                        break;
                    }
                    case O_TOC: {
                        Toc *t = c->lazy_md ? &tocs_lazy[lp->i] : &tocs_store[lp->start + lp->i];
                        if (ins->b == F_ID) {
                            VM_EMIT_BYTES(pbase + t->id.off, t->id.len);
                        } else {
                            if (c->lazy_md) VM_EMIT_BYTES(c->cur_html + t->html.off, t->html.len);
                            else VM_EMIT_BYTES(pbase + t->html.off, t->html.len);
                        }
                        break;
                    }
                    case O_TAGSUM: {
                        Tag *t = &tags[lp->i];
                        if (ins->b == F_NAME) VM_EMIT_STR(t->name, (t->clean_mask & 1), esc);
                        else if (ins->b == F_URL) VM_EMIT_STR(t->url, (t->clean_mask & 2), esc);
                        else VM_EMIT_BYTES(pbase + t->slug.off, t->slug.len);
                        break;
                    }
                    case O_CARD: {
                        Page *pg = &pages[lp->card_ptr[lp->i]];
                        switch (ins->b) {
                            case F_TITLE:     VM_EMIT_STR(pg->title, (pg->clean_mask & 1), esc); break;
                            case F_URL:       VM_EMIT_STR(pg->url, (pg->clean_mask & 2), esc); break;
                            case F_DATE:      VM_EMIT_BYTES(pbase + pg->date.off, pg->date.len); break;
                            case F_DATE_LONG: VM_EMIT_BYTES(pbase + pg->date_long.off, pg->date_long.len); break;
                            case F_SUMMARY:   VM_EMIT_STR(pg->summary, (pg->clean_mask & 4), esc); break;
                            case F_EXCERPT:   VM_EMIT_BYTES(pbase + pg->excerpt.off, pg->excerpt.len); break;
                            default: break;
                        }
                        break;
                    }
                    default: break;
                }
                break;
            }
            case OP_SCOPE_INT: {
                VmLoop *lp = &loops[ins->a];
                if (ins->c == K_LOOP) {
                    VM_EMIT_INT(ins->b == F_INDEX ? (int64_t)(lp->i + 1) : (int64_t)lp->len);
                } else if (ins->c == O_TOC) {
                    Toc *t = c->lazy_md ? &tocs_lazy[lp->i] : &tocs_store[lp->start + lp->i];
                    VM_EMIT_INT(t->level);
                } else if (ins->c == O_TAGSUM) {
                    VM_EMIT_INT((int64_t)tags[lp->i].posts.len);
                } else if (ins->c == O_CARD) {
                    VM_EMIT_INT(pages[lp->card_ptr[lp->i]].reading_minutes);
                }
                break;
            }
            case OP_IF_SCOPE_JMP: {
                VmLoop *lp = &loops[ins->a];
                bool truth = false;
                if (ins->c == O_TAGREF) {
                    TagRef *r = &tagrefs[lp->start + lp->i];
                    truth = (ins->b == F_URL) ? (r->url.len > 0) : (r->name.len > 0);
                } else if (ins->c == O_CARD) {
                    Page *pg = &pages[lp->card_ptr[lp->i]];
                    if (ins->b == F_SUMMARY) truth = pg->summary.len > 0;
                    else if (ins->b == F_DATE) truth = pg->date.len > 0;
                    else if (ins->b == F_TAGS) truth = tagref_ranges[pg->tags].len > 0;
                }
                if (truth == (bool)ins->len) pc = ins->off;
                break;
            }
            case OP_IF_LOOP_JMP: {
                VmLoop *lp = &loops[ins->a];
                bool truth = (ins->b == F_FIRST) ? (lp->i == 0)
                           : (ins->b == F_LAST)  ? (lp->i + 1 == lp->len)
                           : true;
                if (truth == (bool)ins->c) pc = ins->off;
                break;
            }
            case OP_JMP:
                pc = ins->off;
                break;
            case OP_FOR_BEGIN: {
                VmLoop *lp = &loops[ins->a];
                lp->i = 0;
                lp->len = 0;
                lp->start = 0;
                lp->card_ptr = 0;
                lp->binder = ins->node->u.nfor.binder;
                switch (ins->b) {
                    case FL_PAGE_TAGS:
                        if (cur_page) {
                            Range *r = &tagref_ranges[cur_page->tags];
                            lp->akind = A_TAGREFS; lp->idx = cur_page->tags;
                            lp->start = r->start; lp->len = r->len;
                        }
                        break;
                    case FL_PAGE_TOC:
                        if (cur_page) {
                            lp->akind = A_TOC; lp->idx = cur_page->toc;
                            if (c->lazy_md) { lp->start = 0; lp->len = (uint32_t)c->cur_toc->len; }
                            else { Range *r = &toc_ranges[cur_page->toc]; lp->start = r->start; lp->len = r->len; }
                        }
                        break;
                    case FL_POSTS:
                        if (root->posts >= 0) {
                            Arr *ca = arr_at(&c->card_arrays, (size_t)root->posts);
                            lp->akind = A_CARDS; lp->idx = (uint32_t)root->posts;
                            lp->card_ptr = (const uint32_t *)ca->items; lp->len = (uint32_t)ca->len;
                        }
                        break;
                    case FL_TAG_POSTS:
                        if (cur_tag) {
                            lp->akind = A_CARDS; lp->idx = 1 + (uint32_t)root->tag;
                            lp->card_ptr = (const uint32_t *)cur_tag->posts.items; lp->len = (uint32_t)cur_tag->posts.len;
                        }
                        break;
                    case FL_TAGS:
                        lp->akind = A_TAGSUMS; lp->idx = 0;
                        lp->len = (uint32_t)c->tags.len;
                        break;
                    case FL_RECENT: {
                        Arr *ca = arr_at(&c->card_arrays, 0);
                        lp->akind = A_CARDS; lp->idx = 0;
                        lp->card_ptr = (const uint32_t *)ca->items; lp->len = (uint32_t)ca->len;
                        break;
                    }
                    default: {
                        Val v = tpl_eval(c, scopes, *nscopes, &ins->node->u.nfor.ref);
                        if (v.kind == K_ARR) {
                            lp->akind = v.akind; lp->idx = v.idx;
                            lp->len = (uint32_t)ctx_arr_len(c, v.akind, v.idx);
                            if (v.akind == A_CARDS) lp->card_ptr = (const uint32_t *)((Arr *)arr_at(&c->card_arrays, v.idx))->items;
                            else if (v.akind == A_TAGREFS) lp->start = tagref_ranges[v.idx].start;
                            else if (v.akind == A_TOC && !c->lazy_md) lp->start = toc_ranges[v.idx].start;
                        }
                        break;
                    }
                }
                if (lp->len == 0) {
                    pc = ins->off;
                } else if (ins->c) {
                    Scope *fr = &scopes[(*nscopes)++];
                    fr->is_root = false;
                    fr->name = lp->binder;
                    fr->value = ctx_arr_elem(c, lp->akind, lp->idx, 0);
                    fr->lp = (LoopInfo){1, true, lp->len == 1, (int64_t)lp->len};
                    fr->page = -1; fr->posts = -1; fr->pager = -1; fr->tag = -1;
                }
                break;
            }
            case OP_FOR_NEXT: {
                VmLoop *lp = &loops[ins->a];
                lp->i++;
                if (lp->i < lp->len) {
                    if (ins->c) {
                        Scope *fr = &scopes[*nscopes - 1];
                        fr->value = ctx_arr_elem(c, lp->akind, lp->idx, lp->i);
                        fr->lp.index = (int64_t)lp->i + 1;
                        fr->lp.first = false;
                        fr->lp.last = (lp->i + 1 == lp->len);
                    }
                    pc = ins->off;
                } else if (ins->c) {
                    (*nscopes)--;
                }
                break;
            }
            case OP_INCLUDE_CARD: {
                if (__builtin_expect(depth == 0, 1)) {
                    uint32_t pidx = loops[ins->a].card_ptr[loops[ins->a].i];
                    Sp *slot = &c->tpl_card_cache[(size_t)ins->b * c->pages.len + pidx];
                    if (__builtin_expect(slot->off != 0xffffffffu, 1)) {
                        VM_EMIT_BYTES(c->cache_pool->base + slot->off, slot->len);
                    } else {
                        Tpl *ct = arr_at(templates, ins->off);
                        uint32_t st = pool_mark(c->cache_pool);
                        if (ct->card_code.len > 0) {
                            /* Fast flat bytecode execution for O_CARD directly into cache_pool! */
                            Scope cframes[16];
                            cframes[0] = scopes[0];
                            cframes[1].is_root = false;
                            cframes[1].name = ct->dyn_name;
                            cframes[1].value = val_obj(O_CARD, pidx);
                            cframes[1].lp = (LoopInfo){1, true, true, 1};
                            cframes[1].page = -1; cframes[1].posts = -1; cframes[1].pager = -1; cframes[1].tag = -1;
                            size_t cnf = 2;
                            tpl_exec_code(c, templates, &ct->card_code, &pages[pidx], cframes, &cnf, c->cache_pool, scratch, 1);
                        } else {
                            Scope cframes[16];
                            cframes[0] = scopes[0];
                            cframes[1].is_root = false;
                            cframes[1].name = ct->dyn_name;
                            cframes[1].value = val_obj(O_CARD, pidx);
                            cframes[1].lp = (LoopInfo){1, true, true, 1};
                            cframes[1].page = -1; cframes[1].posts = -1; cframes[1].pager = -1; cframes[1].tag = -1;
                            size_t cnf = 2;
                            tpl_emit(c, templates, &ct->nodes, cframes, &cnf, c->cache_pool, scratch, 1);
                        }
                        *slot = pool_span(c->cache_pool, st);
                        VM_EMIT_BYTES(c->cache_pool->base + slot->off, slot->len);
                    }
                    break;
                }
                /* depth > 0 fallback */
                out->len = (size_t)(dst - out->base);
                {
                    Arr one = {ins->node, 1, 1, sizeof(Node)};
                    tpl_emit(c, templates, &one, scopes, nscopes, out, scratch, depth);
                }
                dst = out->base + out->len;
                break;
            }
            case OP_VAR_GEN: {
                out->len = (size_t)(dst - out->base);
                Val v = tpl_eval(c, scopes, *nscopes, &ins->node->u.var.ref);
                if (__builtin_expect(v.kind == K_STR_JSON, 1)) {
                    pool_put(out, pbase + v.v.s.off, v.v.s.len);
                } else if (v.kind == K_CUR_HTML) {
                    pool_put(out, c->cur_html + v.v.s.off, v.v.s.len);
                } else {
                    val_write(c, v, out, !ins->node->raw, scratch);
                }
                dst = out->base + out->len;
                break;
            }
            case OP_IF_GEN_JMP: {
                Val v = tpl_eval(c, scopes, *nscopes, &ins->node->u.nif.ref);
                bool t = val_true(c, v);
                if (t == (bool)ins->node->neg) pc = ins->off;
                break;
            }
            case OP_INCLUDE_GEN: {
                out->len = (size_t)(dst - out->base);
                Arr one = {ins->node, 1, 1, sizeof(Node)};
                tpl_emit(c, templates, &one, scopes, nscopes, out, scratch, depth);
                dst = out->base + out->len;
                break;
            }
            default:
                break;
        }
    }
    out->len = (size_t)(dst - out->base);
}

void tpl_run(Ctx *c, Arr *templates, Tpl *tpl, Scope *scopes, size_t *nscopes,
             Pool *out, Str *scratch, uint32_t depth) {
    if (__builtin_expect(tpl->code.len > 0, 1)) {
        Page *cur_page = (scopes[0].page >= 0) ? (Page *)c->pages.items + scopes[0].page : 0;
        tpl_exec_code(c, templates, &tpl->code, cur_page, scopes, nscopes, out, scratch, depth);
        return;
    }
    tpl_emit(c, templates, &tpl->nodes, scopes, nscopes, out, scratch, depth);
}

void tpl_optimize(Ctx *c, Arr *templates) {
    Pool *p = c->pool;
    Arr orig_nodes;
    arr_new(&orig_nodes, sizeof(Arr));
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        Arr *slot = arr_slot(&orig_nodes, p);
        *slot = t->nodes;
        arr_new(&t->code, sizeof(Instr));
        arr_new(&t->card_code, sizeof(Instr));
    }
    static Str opt_scratch;
    str_clear(&opt_scratch);
    /* Optimize & compile entry templates and cache_mode==2 card templates. */
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->cache_mode == 1) continue; /* pre-rendered via tpl_global */
        Arr opt;
        arr_new(&opt, sizeof(Node));
        uint32_t budget = 256;
        Arr *src = arr_at(&orig_nodes, k);
        opt_nodes(c, templates, &orig_nodes, src, &opt, 0, &opt_scratch, &budget,
                  t->cache_mode == 2, t->dyn_name);
        t->nodes = opt;
        if (t->cache_mode == 2) {
            tpl_compile_into(c, templates, &t->nodes, &t->card_code, true, t->dyn_name);
        }
        tpl_compile_into(c, templates, &t->nodes, &t->code, false, SP_NONE);
    }
}
