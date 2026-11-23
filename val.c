/* val.c — the template-visible value model and the per-build state:
 * field tables, array access, truthiness, textOf, and the fused
 * {{ }} / {{{ }}} writer. */
#include "zg.h"

int field_id(const uint8_t *name, size_t n) {
    switch (n) {
        case 2:
            if (cmp_bytes(name, "id", 2) == 0) return F_ID;
            break;
        case 3:
            if (cmp_bytes(name, "url", 3) == 0) return F_URL;
            if (cmp_bytes(name, "toc", 3) == 0) return F_TOC;
            break;
        case 4:
            if (cmp_bytes(name, "date", 4) == 0) return F_DATE;
            if (cmp_bytes(name, "tags", 4) == 0) return F_TAGS;
            if (cmp_bytes(name, "slug", 4) == 0) return F_SLUG;
            if (cmp_bytes(name, "name", 4) == 0) return F_NAME;
            if (cmp_bytes(name, "html", 4) == 0) return F_HTML;
            if (cmp_bytes(name, "last", 4) == 0) return F_LAST;
            break;
        case 5:
            if (cmp_bytes(name, "title", 5) == 0) return F_TITLE;
            if (cmp_bytes(name, "count", 5) == 0) return F_COUNT;
            if (cmp_bytes(name, "posts", 5) == 0) return F_POSTS;
            if (cmp_bytes(name, "level", 5) == 0) return F_LEVEL;
            if (cmp_bytes(name, "index", 5) == 0) return F_INDEX;
            if (cmp_bytes(name, "first", 5) == 0) return F_FIRST;
            if (cmp_bytes(name, "total", 5) == 0) return F_TOTAL;
            if (cmp_bytes(name, "newer", 5) == 0) return F_NEWER;
            if (cmp_bytes(name, "older", 5) == 0) return F_OLDER;
            break;
        case 6:
            if (cmp_bytes(name, "length", 6) == 0) return F_LENGTH;
            if (cmp_bytes(name, "number", 6) == 0) return F_NUMBER;
            break;
        case 7:
            if (cmp_bytes(name, "summary", 7) == 0) return F_SUMMARY;
            if (cmp_bytes(name, "excerpt", 7) == 0) return F_EXCERPT;
            if (cmp_bytes(name, "content", 7) == 0) return F_CONTENT;
            if (cmp_bytes(name, "is_post", 7) == 0) return F_IS_POST;
            break;
        case 8:
            if (cmp_bytes(name, "base_url", 8) == 0) return F_BASE_URL;
            break;
        case 9:
            if (cmp_bytes(name, "date_long", 9) == 0) return F_DATE_LONG;
            if (cmp_bytes(name, "newer_url", 9) == 0) return F_NEWER_URL;
            if (cmp_bytes(name, "older_url", 9) == 0) return F_OLDER_URL;
            break;
        case 15:
            if (cmp_bytes(name, "reading_minutes", 15) == 0) return F_READING_MINUTES;
            break;
        default:
            break;
    }
    return -1;
}

uint8_t ty_field(uint8_t ty, uint8_t f) {
    switch (ty) {
        case TY_SITE:
            if (f == F_TITLE || f == F_BASE_URL) return TY_STR;
            return TY_VOID;
        case TY_PAGE:
            switch (f) {
                case F_TITLE: case F_URL: case F_DATE: case F_DATE_LONG:
                case F_SUMMARY: case F_EXCERPT: case F_SLUG: case F_CONTENT:
                    return TY_STR;
                case F_READING_MINUTES: return TY_INT;
                case F_TAGS: return TY_ARR_TAGREFS;
                case F_TOC: return TY_ARR_TOC;
                case F_NEWER: case F_OLDER: return TY_LINK;
                case F_IS_POST: return TY_BOOL;
                default: return TY_VOID;
            }
        case TY_CARD:
            switch (f) {
                case F_TITLE: case F_URL: case F_DATE: case F_DATE_LONG:
                case F_SUMMARY: case F_EXCERPT:
                    return TY_STR;
                case F_READING_MINUTES: return TY_INT;
                case F_TAGS: return TY_ARR_TAGREFS;
                default: return TY_VOID;
            }
        case TY_TAGSUM:
            switch (f) {
                case F_NAME: case F_SLUG: case F_URL: return TY_STR;
                case F_COUNT: return TY_INT;
                default: return TY_VOID;
            }
        case TY_TAG:
            switch (f) {
                case F_NAME: case F_SLUG: case F_URL: return TY_STR;
                case F_COUNT: return TY_INT;
                case F_POSTS: return TY_ARR_CARDS;
                default: return TY_VOID;
            }
        case TY_TAGREF:
            switch (f) {
                case F_NAME: case F_SLUG: case F_URL: return TY_STR;
                default: return TY_VOID;
            }
        case TY_TOC:
            switch (f) {
                case F_LEVEL: return TY_INT;
                case F_ID: case F_HTML: return TY_STR;
                default: return TY_VOID;
            }
        case TY_LINK:
            switch (f) {
                case F_TITLE: case F_URL: case F_DATE: case F_DATE_LONG: return TY_STR;
                default: return TY_VOID;
            }
        case TY_LOOP:
            switch (f) {
                case F_INDEX: case F_LENGTH: return TY_INT;
                case F_FIRST: case F_LAST: return TY_BOOL;
                default: return TY_VOID;
            }
        case TY_PAGER:
            switch (f) {
                case F_NUMBER: case F_TOTAL: return TY_INT;
                case F_URL: case F_NEWER_URL: case F_OLDER_URL: return TY_STR;
                default: return TY_VOID;
            }
        case TY_ARR_CARDS: case TY_ARR_TAGSUMS: case TY_ARR_TAGREFS: case TY_ARR_TOC:
            if (f == F_LENGTH) return TY_INT;
            return TY_VOID;
        default:
            return TY_VOID;
    }
}

uint8_t ty_elem(uint8_t ty) {
    switch (ty) {
        case TY_ARR_CARDS: return TY_CARD;
        case TY_ARR_TAGSUMS: return TY_TAGSUM;
        case TY_ARR_TAGREFS: return TY_TAGREF;
        case TY_ARR_TOC: return TY_TOC;
        default: return TY_VOID;
    }
}

size_t ctx_arr_len(Ctx *c, uint8_t kind, uint32_t idx) {
    switch (kind) {
        case A_CARDS: {
            Arr *arr = arr_at(&c->card_arrays, idx);
            return arr->len;
        }
        case A_TAGSUMS:
            return c->tags.len;
        case A_TAGREFS: {
            Range *r = arr_at(&c->tagref_arrays, idx);
            return r->len;
        }
        case A_TOC: {
            Range *r = arr_at(&c->toc_arrays, idx);
            return r->len;
        }
        default:
            return 0;
    }
}

Val ctx_arr_elem(Ctx *c, uint8_t kind, uint32_t idx, size_t i) {
    switch (kind) {
        case A_CARDS: {
            Arr *arr = arr_at(&c->card_arrays, idx);
            uint32_t *page = arr_at(arr, i);
            return val_obj(O_CARD, *page);
        }
        case A_TAGSUMS:
            return val_obj(O_TAGSUM, (uint32_t)i);
        case A_TAGREFS: {
            Range *r = arr_at(&c->tagref_arrays, idx);
            return val_obj(O_TAGREF, r->start + (uint32_t)i);
        }
        case A_TOC: {
            Range *r = arr_at(&c->toc_arrays, idx);
            return val_obj(O_TOC, r->start + (uint32_t)i);
        }
        default:
            return val_void();
    }
}

Val val_field(Ctx *c, Val v, uint8_t f) {
    if (v.kind == K_OBJ) {
        switch (v.akind) {
            case O_SITE:
                if (f == F_TITLE) return c->site_title_clean ? val_str_json(c->site_title) : val_str(c->site_title);
                if (f == F_BASE_URL) return c->site_base_clean ? val_str_json(c->site_base) : val_str(c->site_base);
                return val_void();
            case O_PAGE: {
                Page *p = arr_at(&c->pages, v.idx);
                switch (f) {
                    case F_TITLE: return (p->clean_mask & 1) ? val_str_json(p->title) : val_str(p->title);
                    case F_URL: return (p->clean_mask & 2) ? val_str_json(p->url) : val_str(p->url);
                    case F_DATE: return val_str_json(p->date);
                    case F_DATE_LONG: return val_str_json(p->date_long);
                    case F_SUMMARY: return (p->clean_mask & 4) ? val_str_json(p->summary) : val_str(p->summary);
                    case F_EXCERPT: return c->md_pre_json ? val_str_json(p->excerpt) : val_str(p->excerpt);
                    case F_READING_MINUTES: return val_int(p->reading_minutes);
                    case F_TAGS: return val_arr(A_TAGREFS, p->tags);
                    case F_SLUG: return val_str_json(p->slug);
                    case F_CONTENT: return c->md_pre_json ? val_str_json(p->html) : val_str(p->html);
                    case F_TOC: return val_arr(A_TOC, p->toc);
                    case F_NEWER: return p->newer >= 0 ? val_obj(O_LINK, (uint32_t)p->newer) : val_null();
                    case F_OLDER: return p->older >= 0 ? val_obj(O_LINK, (uint32_t)p->older) : val_null();
                    case F_IS_POST: return val_bool(p->is_post);
                    default: return val_void();
                }
            }
            case O_CARD: {
                Page *p = arr_at(&c->pages, v.idx);
                switch (f) {
                    case F_TITLE: return (p->clean_mask & 1) ? val_str_json(p->title) : val_str(p->title);
                    case F_URL: return (p->clean_mask & 2) ? val_str_json(p->url) : val_str(p->url);
                    case F_DATE: return val_str_json(p->date);
                    case F_DATE_LONG: return val_str_json(p->date_long);
                    case F_SUMMARY: return (p->clean_mask & 4) ? val_str_json(p->summary) : val_str(p->summary);
                    case F_EXCERPT: return c->md_pre_json ? val_str_json(p->excerpt) : val_str(p->excerpt);
                    case F_READING_MINUTES: return val_int(p->reading_minutes);
                    case F_TAGS: return val_arr(A_TAGREFS, p->tags);
                    default: return val_void();
                }
            }
            case O_TAGSUM: {
                Tag *t = arr_at(&c->tags, v.idx);
                switch (f) {
                    case F_NAME: return (t->clean_mask & 1) ? val_str_json(t->name) : val_str(t->name);
                    case F_SLUG: return val_str_json(t->slug);
                    case F_URL: return (t->clean_mask & 2) ? val_str_json(t->url) : val_str(t->url);
                    case F_COUNT: return val_int((int64_t)t->posts.len);
                    default: return val_void();
                }
            }
            case O_TAG: {
                Tag *t = arr_at(&c->tags, v.idx);
                switch (f) {
                    case F_NAME: return (t->clean_mask & 1) ? val_str_json(t->name) : val_str(t->name);
                    case F_SLUG: return val_str_json(t->slug);
                    case F_URL: return (t->clean_mask & 2) ? val_str_json(t->url) : val_str(t->url);
                    case F_COUNT: return val_int((int64_t)t->posts.len);
                    case F_POSTS: return val_arr(A_CARDS, 1 + v.idx);
                    default: return val_void();
                }
            }
            case O_TAGREF: {
                TagRef *r = arr_at(&c->tagref_store, v.idx);
                switch (f) {
                    case F_NAME: return (r->clean_mask & 1) ? val_str_json(r->name) : val_str(r->name);
                    case F_SLUG: return val_str_json(r->slug);
                    case F_URL: return (r->clean_mask & 2) ? val_str_json(r->url) : val_str(r->url);
                    default: return val_void();
                }
            }
            case O_TOC: {
                Toc *t = arr_at(&c->toc_store, v.idx);
                switch (f) {
                    case F_LEVEL: return val_int(t->level);
                    case F_ID: return val_str_json(t->id);
                    case F_HTML: return c->md_pre_json ? val_str_json(t->html) : val_str(t->html);
                    default: return val_void();
                }
            }
            case O_LINK: {
                Page *p = arr_at(&c->pages, v.idx);
                switch (f) {
                    case F_TITLE: return (p->clean_mask & 1) ? val_str_json(p->title) : val_str(p->title);
                    case F_URL: return (p->clean_mask & 2) ? val_str_json(p->url) : val_str(p->url);
                    case F_DATE: return val_str_json(p->date);
                    case F_DATE_LONG: return val_str_json(p->date_long);
                    default: return val_void();
                }
            }
            case O_PAGER: {
                Pager *p = arr_at(&c->pagers, v.idx);
                switch (f) {
                    case F_NUMBER: return val_int(p->number);
                    case F_TOTAL: return val_int(p->total);
                    case F_URL: return c->site_base_clean ? val_str_json(p->url) : val_str(p->url);
                    case F_NEWER_URL: return c->site_base_clean ? val_str_json(p->newer_url) : val_str(p->newer_url);
                    case F_OLDER_URL: return c->site_base_clean ? val_str_json(p->older_url) : val_str(p->older_url);
                    default: return val_void();
                }
            }
            default:
                return val_void();
        }
    }
    if (v.kind == K_LOOP) {
        switch (f) {
            case F_INDEX: return val_int((int64_t)v.idx);
            case F_FIRST: return val_bool(v.idx == 1);
            case F_LAST: return val_bool((uint64_t)v.idx == v.v.u);
            case F_LENGTH: return val_int((int64_t)v.v.u);
            default: return val_void();
        }
    }
    if (v.kind == K_ARR) {
        if (f == F_LENGTH) return val_int((int64_t)ctx_arr_len(c, v.akind, v.idx));
        return val_void();
    }
    return val_void();
}

bool val_true(Ctx *c, Val v) {
    switch (v.kind) {
        case K_ARR: return ctx_arr_len(c, v.akind, v.idx) > 0;
        case K_VOID: case K_NULL: return false;
        case K_BOOL: return v.v.n != 0;
        case K_INT: return v.v.n != 0;
        case K_STR: case K_STR_JSON: return v.v.s.len > 0;
        case K_OBJ: case K_LOOP: return true;
        default: return false;
    }
}

void val_text(Ctx *c, Val v, Str *scratch) {
    switch (v.kind) {
        case K_STR: case K_STR_JSON:
            str_add(scratch, pool_at(c->pool, v.v.s), v.v.s.len);
            return;
        case K_INT:
            str_i64(scratch, v.v.n);
            return;
        case K_BOOL:
            str_cstr(scratch, v.v.n ? "true" : "false");
            return;
        case K_ARR: {
            size_t len = ctx_arr_len(c, v.akind, v.idx);
            for (size_t i = 0; i < len; i++) {
                if (i > 0) str_cstr(scratch, ", ");
                val_text(c, ctx_arr_elem(c, v.akind, v.idx, i), scratch);
            }
            return;
        }
        default:
            return;
    }
}

void val_write(Ctx *c, Val v, Pool *out, bool esc, Str *scratch) {
    switch (v.kind) {
        case K_STR_JSON:
            pool_put(out, pool_at(c->pool, v.v.s), v.v.s.len);
            return;
        case K_STR: {
            const uint8_t *b = pool_at(c->pool, v.v.s);
            if (esc) html_json_escape_into(out, b, v.v.s.len);
            else json_escape_into(out, b, v.v.s.len);
            return;
        }
        case K_INT:
            pool_i64(out, v.v.n);
            return;
        case K_BOOL:
            pool_put(out, (const uint8_t *)(v.v.n ? "true" : "false"), v.v.n ? 4 : 5);
            return;
        case K_ARR:
            str_clear(scratch);
            val_text(c, v, scratch);
            if (esc) html_json_escape_into(out, scratch->data, scratch->len);
            else json_escape_into(out, scratch->data, scratch->len);
            return;
        default:
            return;
    }
}
