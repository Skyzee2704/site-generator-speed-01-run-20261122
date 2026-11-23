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
    Str text; str_new(&text);
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

void tpl_analyze(Pool *p, Arr *templates) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        arr_new(&t->needs, sizeof(TplNeed));
        t->cacheable = 1;
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
                if (depth < INCLUDE_DEPTH && seg->u.ninc.tpl_idx >= 0) {
                    size_t k = (size_t)seg->u.ninc.tpl_idx;
                    Tpl *t = arr_at(templates, k);
                    if (t->cache_mode == 1) {
                        Sp *slot = &c->tpl_global[k * INCLUDE_DEPTH + depth];
                        if (__builtin_expect(slot->off != 0xffffffffu, 1)) {
                            pool_copy(out, *slot);
                        } else {
                            uint32_t start = pool_mark(out);
                            tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, depth + 1);
                            *slot = pool_span(out, start);
                        }
                        break;
                    }
                    if (t->cache_mode == 2 && depth == 0 && c->tpl_card_cache) {
                        Val dv = dep_value_dyn(c, scopes, *nscopes, t->dyn_name);
                        if (__builtin_expect(dv.kind == K_OBJ && dv.akind == O_CARD && dv.idx < c->pages.len, 1)) {
                            Sp *slot = &c->tpl_card_cache[(size_t)t->card_slot * c->pages.len + dv.idx];
                            if (__builtin_expect(slot->off != 0xffffffffu, 1)) {
                                pool_copy(out, *slot);
                            } else {
                                uint32_t start = pool_mark(out);
                                tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, depth + 1);
                                *slot = pool_span(out, start);
                            }
                            break;
                        }
                    }
                    if (t->cache_mode != 0 && c->tpl_index) {
                        uint64_t h = 1469598103934665603ULL;
                        h ^= (uint64_t)k; h *= 1099511628211ULL;
                        h ^= (uint64_t)(depth + 1); h *= 1099511628211ULL;
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
                            if (e->hash == h && e->tpl_idx == (uint32_t)k && e->depth == (uint8_t)(depth + 1) && e->nkeys == nk) {
                                bool eq = true;
                                for (uint8_t di = 0; di < nk; di++) if (!val_equal(e->keys[di], keys[di])) { eq = false; break; }
                                if (eq) {
                                    pool_copy(out, e->bytes); hit = true; break;
                                }
                            }
                            slot = (slot + 1) & mask;
                        }
                        if (!hit) {
                            uint32_t start = pool_mark(out);
                            tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, depth + 1);
                            Sp span = pool_span(out, start);
                            if (c->tpl_entries.len < 4096) {
                                TplHit *e = arr_slot(&c->tpl_entries, c->pool);
                                e->hash = h; e->tpl_idx = (uint32_t)k; e->depth = (uint8_t)(depth + 1);
                                e->nkeys = nk; e->bytes = span;
                                for (uint8_t di = 0; di < nk; di++) e->keys[di] = keys[di];
                                size_t s2 = (size_t)h & mask;
                                while (c->tpl_index[s2] >= 0) s2 = (s2 + 1) & mask;
                                c->tpl_index[s2] = (int32_t)(c->tpl_entries.len - 1);
                            }
                        }
                    } else {
                        tpl_emit(c, templates, &t->nodes, scopes, nscopes, out, scratch, depth + 1);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}
