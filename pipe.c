/* pipe.c — the build pipeline: readSources -> frontMatter -> permalinks
 * -> markdown -> collections -> tags -> layouts -> report.
 * The output object is written as compact JSON; the template renderer
 * writes its (JSON-escaped) HTML directly into the answers buffer. */
#include "zg.h"

static const uint8_t RESERVED_DIRS[3][6] = {"posts", "tags", "page"};
static const size_t RESERVED_LEN[3] = {5, 4, 4};

static Sp base_strip(Pool *p, Sp base) {
    const uint8_t *b = pool_at(p, base);
    size_t len = base.len;
    while (len > 0 && b[len - 1] == '/') len--;
    return (Sp){base.off, (uint32_t)len};
}

static Sp path_dir(Pool *p, Sp path) {
    const uint8_t *b = pool_at(p, path);
    for (size_t i = path.len; i-- > 0;) {
        if (b[i] == '/') return (Sp){path.off, (uint32_t)i};
    }
    return SP_NONE;
}

static bool path_is_md(Pool *p, Sp path) {
    const uint8_t *b = pool_at(p, path);
    return path.len >= 3 && b[path.len - 3] == '.' && b[path.len - 2] == 'm' && b[path.len - 1] == 'd';
}

/* Normalize "\r\n" and "\r" to "\n". Returns the original span when there
 * is no '\r' (zero-copy). */
static Sp crlf_norm(Pool *p, Sp content) {
    const uint8_t *b = pool_at(p, content);
    size_t off = content.off, len = content.len, end = off + len;
    /* Fast SIMD scan for any '\r'; the common case has none (zero-copy). */
    bool has_cr = scan_byte(p->base, end, off, '\r') >= 0;
    if (!has_cr) return content;
    (void)b;
    uint32_t start = pool_mark(p);
    size_t i = off;
    while (i < end) {
        uint8_t c = p->base[i];
        if (c == '\r') {
            pool_u8(p, '\n');
            if (i + 1 < end && p->base[i + 1] == '\n') i += 2;
            else i += 1;
        } else {
            pool_u8(p, c);
            i++;
        }
    }
    return pool_span(p, start);
}

static int cmp_rawfile(void *ctx, const void *x, const void *y) {
    Pool *p = ctx;
    const RawFile *a = x, *b = y;
    return u16_cmp(pool_at(p, a->path), a->path.len, pool_at(p, b->path), b->path.len);
}

static int cmp_pair(void *ctx, const void *x, const void *y) {
    Pool *p = ctx;
    const SpPair *a = x, *b = y;
    return u16_cmp(pool_at(p, a->a), a->a.len, pool_at(p, b->a), b->a.len);
}

static int cmp_sp(void *ctx, const void *x, const void *y) {
    Pool *p = ctx;
    const Sp *a = x, *b = y;
    return u16_cmp(pool_at(p, *a), a->len, pool_at(p, *b), b->len);
}

typedef struct { Pool *pool; Arr *pages; } PostCtx;
static int cmp_posts(void *ctx, const void *x, const void *y) {
    PostCtx *c = ctx;
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    Page *pa = arr_at(c->pages, a);
    Page *pb = arr_at(c->pages, b);
    int d = u16_cmp(pool_at(c->pool, pb->date), pb->date.len, pool_at(c->pool, pa->date), pa->date.len);
    if (d) return d;
    return u16_cmp(pool_at(c->pool, pa->slug), pa->slug.len, pool_at(c->pool, pb->slug), pb->slug.len);
}

static int cmp_tags(void *ctx, const void *x, const void *y) {
    Pool *p = ctx;
    const Tag *a = x, *b = y;
    if (b->posts.len != a->posts.len) return b->posts.len > a->posts.len ? 1 : -1;
    return u16_cmp(pool_at(p, a->slug), a->slug.len, pool_at(p, b->slug), b->slug.len);
}

static int32_t tpl_find_idx(Arr *templates, Pool *p, const uint8_t *name, size_t n) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->name.len == n && eq_bytes(pool_at(p, t->name), name, n)) return (int32_t)k;
    }
    return -1;
}

static Tpl *tpl_find(Arr *templates, Pool *p, const uint8_t *name, size_t n) {
    int32_t idx = tpl_find_idx(templates, p, name, n);
    return idx >= 0 ? (Tpl *)arr_at(templates, (size_t)idx) : 0;
}

static Sp idx_url(Pool *p, Sp base, int64_t k) {
    uint32_t start = pool_mark(p);
    pool_copy(p, base);
    if (k == 1) {
        pool_u8(p, '/');
    } else {
        pool_put(p, (const uint8_t *)"/page/", 6);
        pool_i64(p, k);
        pool_u8(p, '/');
    }
    return pool_span(p, start);
}

/* ownTags: a page's tag names, each slug once, in its own order. */
typedef struct { Sp name; Sp slug; } NameSlug;
static size_t page_tags(Pool *p, Arr *names, Dict *cache, NameSlug *out, size_t max_out) {
    size_t nout = 0;
    Sp seen[64];
    size_t nseen = 0;
    for (size_t i = 0; i < names->len; i++) {
        Sp *nm = arr_at(names, i);
        Sp slug = slug_make(p, *nm, cache);
        bool dup = false;
        for (size_t k = 0; k < nseen; k++) {
            if (seen[k].len == slug.len && cmp_bytes(pool_at(p, seen[k]), pool_at(p, slug), slug.len) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (nseen < 64) seen[nseen++] = slug;
        if (nout < max_out) { out[nout].name = *nm; out[nout].slug = slug; nout++; }
    }
    return nout;
}

typedef struct { Sp dir; Set set; } DirTaken;

static inline bool span_is_clean(Pool *p, Sp s) {
    return s.len == 0 || scan_html_json_esc(pool_at(p, s), s.len, 0) < 0;
}

static inline void maybe_flush(Pool *out) {
    if (out->fd >= 0 && out->len >= 96 * 1024) out_flush(out);
}

static void setup_link_env(Pool *p, LinkCtx *env, Page *pages_ptr, size_t npages,
                           RawFile *statics_ptr, size_t nstatics,
                           Dict *page_map, Set *static_set, Sp base,
                           Page *pd, Arr *broken,
                           Str *path_buf, Str *item_buf, Str *item_scratch) {
    env->pages = pages_ptr;
    env->npages = npages;
    env->statics = statics_ptr;
    env->nstatics = nstatics;
    env->page_map = page_map;
    env->static_set = static_set;
    env->base = base;
    env->dir = path_dir(p, pd->path);
    env->dir_np = 0;
    if (env->dir.len > 0) {
        const uint8_t *db = pool_at(p, env->dir);
        size_t ds = 0;
        for (size_t di = 0; di <= env->dir.len; di++) {
            if (di == env->dir.len || db[di] == '/') {
                if (env->dir_np < 64) { env->dir_parts[env->dir_np] = db + ds; env->dir_plens[env->dir_np] = di - ds; env->dir_np++; }
                ds = di + 1;
            }
        }
    }
    env->src_path = pd->path;
    env->broken = broken;
    env->path_buf = path_buf;
    env->item_buf = item_buf;
    env->item_scratch = item_scratch;
}

bool case_build(Pool *p, const uint8_t *in_base, CaseInput *c, Pool *out) {
    /* readSources: sort files by path (code-unit order). */
    arr_sort(&c->files, p, p, cmp_rawfile);
    arr_sort(&c->templates, p, p, cmp_pair);
    Sp base = base_strip(p, c->site.base_url);
    bool site_title_clean = span_is_clean(p, c->site.title);
    bool site_base_clean = span_is_clean(p, base);

    /* templates: parse in name order; failures go to the report. */
    Arr templates; arr_new(&templates, sizeof(Tpl));
    Arr report; arr_new(&report, sizeof(Sp));
    for (size_t i = 0; i < c->templates.len; i++) {
        SpPair *pr = arr_at(&c->templates, i);
        Arr nodes;
        Sp err;
        if (tpl_parse(p, pr->a, pr->b, &nodes, &err)) {
            Tpl *t = arr_slot(&templates, p);
            t->name = pr->a;
            t->nodes = nodes;
            arr_new(&t->needs, sizeof(TplNeed));
            t->cacheable = 1;
        } else {
            Sp *line = arr_slot(&report, p);
            *line = err;
        }
    }
    tpl_analyze(p, &templates);
    bool md_pre_json = tpl_can_pre_json_md(&templates);

    /* frontMatter + permalinks (per source, in sorted path order). */
    Arr pages; arr_new(&pages, sizeof(Page));
    Arr fm_tags; arr_new(&fm_tags, sizeof(Arr)); /* Arr<Sp> per page */
    Arr statics; arr_new(&statics, sizeof(RawFile));
    Set taken_root; set_new(&taken_root);
    for (int r = 0; r < 3; r++) {
        Sp rs = pool_store(p, RESERVED_DIRS[r], RESERVED_LEN[r]);
        set_add(&taken_root, p, rs);
    }
    Set taken_posts; set_new(&taken_posts);
    Arr taken_sets; arr_new(&taken_sets, sizeof(DirTaken));
    Dict slug_cache; dict_new(&slug_cache);
    Sp lit_posts_dir = pool_store(p, (const uint8_t *)"posts", 5);
    Sp lit_post = pool_store(p, (const uint8_t *)"post", 4);
    Sp lit_page = pool_store(p, (const uint8_t *)"page", 4);
    int32_t post_tpl_idx = tpl_find_idx(&templates, p, (const uint8_t *)"post", 4);
    int32_t page_tpl_idx = tpl_find_idx(&templates, p, (const uint8_t *)"page", 4);
    (void)in_base;

    for (size_t fi = 0; fi < c->files.len; fi++) {
        RawFile *fp = arr_at(&c->files, fi);
        Sp path = fp->path;
        if (!path_is_md(p, path)) {
            RawFile *st = arr_slot(&statics, p);
            *st = *fp;
            continue;
        }
        Sp text = fp->content;
        if (!(fp->flags & RF_CRLF_NORM)) {
            if (fp->flags & RF_HAS_CR) text = crlf_norm(p, text);
            else if (!(fp->flags & RF_NO_ESC)) text = crlf_norm(p, text);
        }
        Meta fm;
        Sp fm_err;
        bool fm_ok = (fp->flags & RF_HAS_FM)
            ? meta_parse_hint(p, text, fp->fm_close_off, fp->body_rel_off, &fm, &fm_err)
            : meta_parse(p, text, &fm, &fm_err);
        if (!fm_ok) {
            uint32_t st = pool_mark(p);
            pool_put(p, (const uint8_t *)"refused ", 8);
            pool_copy(p, path);
            pool_put(p, (const uint8_t *)": ", 2);
            pool_copy(p, fm_err);
            Sp span_st = pool_span(p, st);
            Sp *line = arr_slot(&report, p);
            *line = span_st;
            continue;
        }
        const uint8_t *pb = pool_at(p, path);
        bool is_post = path.len >= 6 && cmp_bytes(pb, "posts/", 6) == 0;
        /* Reserved own directory (tags/ or page/). */
        const uint8_t *own = 0; size_t own_len = 0;
        static const uint8_t d_tags[4] = {'t', 'a', 'g', 's'};
        static const uint8_t d_page[4] = {'p', 'a', 'g', 'e'};
        if (path.len > 5 && cmp_bytes(pb, d_tags, 4) == 0 && pb[4] == '/') { own = d_tags; own_len = 4; }
        else if (path.len > 5 && cmp_bytes(pb, d_page, 4) == 0 && pb[4] == '/') { own = d_page; own_len = 4; }
        if (own) {
            uint32_t st = pool_mark(p);
            pool_put(p, (const uint8_t *)"refused ", 8);
            pool_copy(p, path);
            pool_put(p, (const uint8_t *)": ", 2);
            pool_put(p, own, own_len);
            pool_put(p, (const uint8_t *)"/ is the generator's own", 24);
            Sp span_st = pool_span(p, st);
            Sp *line = arr_slot(&report, p);
            *line = span_st;
            continue;
        }
        if (is_post && !fm.has_date) {
            uint32_t st = pool_mark(p);
            pool_put(p, (const uint8_t *)"refused ", 8);
            pool_copy(p, path);
            pool_put(p, (const uint8_t *)": a post needs a date", 21);
            Sp span_st = pool_span(p, st);
            Sp *line = arr_slot(&report, p);
            *line = span_st;
            continue;
        }
        Sp layout;
        int32_t layout_idx;
        if (fm.has_layout) {
            layout = fm.layout;
            layout_idx = tpl_find_idx(&templates, p, pool_at(p, layout), layout.len);
        } else {
            layout = is_post ? lit_post : lit_page;
            layout_idx = is_post ? post_tpl_idx : page_tpl_idx;
        }
        if (layout_idx < 0) {
            uint32_t st = pool_mark(p);
            pool_put(p, (const uint8_t *)"refused ", 8);
            pool_copy(p, path);
            pool_put(p, (const uint8_t *)": layout \"", 10);
            pool_copy(p, layout);
            pool_put(p, (const uint8_t *)"\" is no template that parses", 28);
            Sp span_st = pool_span(p, st);
            Sp *line = arr_slot(&report, p);
            *line = span_st;
            continue;
        }
        if (fm.draft) {
            uint32_t st = pool_mark(p);
            pool_put(p, (const uint8_t *)"draft ", 6);
            pool_copy(p, path);
            Sp span_st = pool_span(p, st);
            Sp *line = arr_slot(&report, p);
            *line = span_st;
            continue;
        }
        /* Permalinks: the slug, unique in its directory. */
        Sp dir = is_post ? lit_posts_dir : path_dir(p, path);
        Sp slug_base = fm.has_slug ? fm.slug : slug_make(p, fm.title, &slug_cache);
        Sp slug;
        if (is_post) {
            slug = slug_unique(p, slug_base, &taken_posts);
        } else if (dir.len == 0) {
            slug = slug_unique(p, slug_base, &taken_root);
        } else if (dir.len == 5 && eq_bytes(pool_at(p, dir), "posts", 5)) {
            slug = slug_unique(p, slug_base, &taken_posts);
        } else {
            int idx = -1;
            for (size_t k = 0; k < taken_sets.len; k++) {
                DirTaken *td = arr_at(&taken_sets, k);
                if (td->dir.len == dir.len && eq_bytes(pool_at(p, td->dir), pool_at(p, dir), dir.len)) { idx = (int)k; break; }
            }
            if (idx < 0) {
                DirTaken *td = arr_slot(&taken_sets, p);
                td->dir = dir;
                set_new(&td->set);
                idx = (int)taken_sets.len - 1;
            }
            DirTaken *td = arr_at(&taken_sets, (size_t)idx);
            slug = slug_unique(p, slug_base, &td->set);
        }
        Sp out_dir;
        if (dir.len == 0) {
            out_dir = slug;
        } else {
            uint32_t st = pool_mark(p);
            pool_copy(p, dir);
            pool_u8(p, '/');
            pool_copy(p, slug);
            out_dir = pool_span(p, st);
        }
        Sp url;
        {
            uint32_t st = pool_mark(p);
            pool_copy(p, base);
            pool_u8(p, '/');
            pool_copy(p, out_dir);
            pool_u8(p, '/');
            url = pool_span(p, st);
        }
        Sp dlong = fm.has_date ? date_long(p, fm.date) : SP_NONE;
        Page *pd = arr_slot(&pages, p);
        pd->path = path; pd->title = fm.title;
        pd->date = fm.has_date ? fm.date : SP_NONE; pd->has_date = fm.has_date;
        pd->date_long = dlong;
        pd->summary = fm.has_summary ? fm.summary : SP_NONE;
        pd->slug = slug; pd->out_dir = out_dir; pd->url = url;
        pd->html = SP_NONE; pd->excerpt = SP_NONE;
        if (fp->flags & RF_HAS_WC) {
            int64_t rm = (int64_t)(((size_t)fp->word_cnt + 199) / 200);
            pd->reading_minutes = rm < 1 ? 1 : rm;
        } else {
            size_t w = word_count(pool_at(p, fm.body), fm.body.len);
            int64_t rm = (int64_t)((w + 199) / 200);
            pd->reading_minutes = rm < 1 ? 1 : rm;
        }
        pd->toc = 0; pd->tags = 0; pd->newer = -1; pd->older = -1;
        pd->is_post = is_post;
        pd->clean_mask = (uint8_t)((span_is_clean(p, pd->title) ? 1 : 0)
            | (span_is_clean(p, pd->url) ? 2 : 0)
            | (span_is_clean(p, pd->summary) ? 4 : 0));
        pd->layout_tpl = layout_idx;
        pd->layout = layout;
        pd->body = fm.body;
        Arr *ft = arr_slot(&fm_tags, p);
        *ft = fm.tags;
    }

    /* Check if lazy per-page Markdown rendering is sound. */
    bool lazy_md = true;
    for (size_t pi = 0; pi < pages.len; pi++) {
        Page *pd = arr_at(&pages, pi);
        Tpl *tpl = arr_at(&templates, (size_t)pd->layout_tpl);
        if (tpl->uses_card_md) { lazy_md = false; break; }
    }

    /* markdown lookup tables (needed for both eager and lazy md_build). */
    Arr broken; arr_new(&broken, sizeof(Sp));
    Arr toc_store; arr_new(&toc_store, sizeof(Toc));
    Arr toc_arrays; arr_new(&toc_arrays, sizeof(Range));
    Dict md_slug_cache; dict_new(&md_slug_cache);
    Dict page_map; dict_new(&page_map);
    for (size_t pi = 0; pi < pages.len; pi++) {
        Page *pd = arr_at(&pages, pi);
        dict_put(&page_map, p, pd->path, (Sp){(uint32_t)pi, 0});
    }
    Set static_set; set_new(&static_set);
    for (size_t si = 0; si < statics.len; si++) {
        RawFile *st = arr_at(&statics, si);
        set_add(&static_set, p, st->path);
    }
    static Str scratch, tmp, text_tmp, path_buf, item_buf, item_scratch;
    str_clear(&scratch); str_clear(&tmp); str_clear(&text_tmp);
    str_clear(&path_buf); str_clear(&item_buf); str_clear(&item_scratch);
    if (!lazy_md) {
        Page *pages_ptr = pages.len ? arr_at(&pages, 0) : 0;
        RawFile *statics_ptr = statics.len ? arr_at(&statics, 0) : 0;
        for (size_t pi = 0; pi < pages.len; pi++) {
            Page *pd = arr_at(&pages, pi);
            LinkCtx env;
            setup_link_env(p, &env, pages_ptr, pages.len, statics_ptr, statics.len,
                           &page_map, &static_set, base, pd, &broken,
                           &path_buf, &item_buf, &item_scratch);
            Sp html, excerpt;
            uint32_t toc_start = (uint32_t)toc_store.len;
            md_build_buf(p, pool_at(p, pd->body), pd->body.len, &env, &md_slug_cache, &scratch, &tmp, &text_tmp, &html, &toc_store, &excerpt, md_pre_json, true);
            Range *ra = arr_slot(&toc_arrays, p);
            ra->start = toc_start; ra->len = (uint32_t)toc_store.len - toc_start;
            pd->html = html;
            pd->excerpt = excerpt;
            pd->toc = (uint32_t)pi;
        }
    }

    /* collections: posts newest first, then by slug. */
    Arr post_order; arr_new(&post_order, sizeof(uint32_t));
    for (size_t i = 0; i < pages.len; i++) {
        Page *pd = arr_at(&pages, i);
        if (pd->is_post) {
            uint32_t *slot = arr_slot(&post_order, p);
            *slot = (uint32_t)i;
        }
    }
    PostCtx pctx = {p, &pages};
    arr_sort(&post_order, p, &pctx, cmp_posts);
    for (size_t k = 0; k < post_order.len; k++) {
        uint32_t *pi = arr_at(&post_order, k);
        Page *pd = arr_at(&pages, *pi);
        pd->newer = k == 0 ? -1 : (int32_t)*(uint32_t *)arr_at(&post_order, k - 1);
        pd->older = k + 1 == post_order.len ? -1 : (int32_t)*(uint32_t *)arr_at(&post_order, k + 1);
    }

    /* tags: grouped by slug, named as first met in post order, ordered by
     * count desc then slug asc. */
    Arr tags; arr_new(&tags, sizeof(Tag));
    Arr tagref_store; arr_new(&tagref_store, sizeof(TagRef));
    Arr tagref_arrays; arr_new(&tagref_arrays, sizeof(Range));
    for (size_t i = 0; i < pages.len; i++) {
        Range *r = arr_slot(&tagref_arrays, p);
        r->start = 0; r->len = 0;
    }
    Dict tag_by_slug; dict_new(&tag_by_slug);
    Dict tag_slug_cache; dict_new(&tag_slug_cache);
    NameSlug own[64];
    /* Pass 1: the posts, in post order. */
    for (size_t k = 0; k < post_order.len; k++) {
        uint32_t pi = *(uint32_t *)arr_at(&post_order, k);
        Arr *names = arr_at(&fm_tags, pi);
        size_t nown = page_tags(p, names, &tag_slug_cache, own, 64);
        uint32_t start = (uint32_t)tagref_store.len;
        for (size_t j = 0; j < nown; j++) {
            uint32_t ti = 0;
            Sp hit;
            if (dict_get(&tag_by_slug, p, pool_at(p, own[j].slug), own[j].slug.len, &hit)) {
                ti = hit.off;
            } else {
                uint32_t st = pool_mark(p);
                pool_copy(p, base);
                pool_put(p, (const uint8_t *)"/tags/", 6);
                pool_copy(p, own[j].slug);
                pool_u8(p, '/');
                Sp url = pool_span(p, st);
                ti = (uint32_t)tags.len;
                Tag *t = arr_slot(&tags, p);
                t->name = own[j].name; t->slug = own[j].slug; t->url = url;
                t->clean_mask = (uint8_t)((span_is_clean(p, t->name) ? 1 : 0) | (site_base_clean ? 2 : 0));
                arr_new(&t->posts, sizeof(uint32_t));
                dict_put(&tag_by_slug, p, own[j].slug, (Sp){ti, 0});
            }
            Tag *t = arr_at(&tags, ti);
            uint32_t *slot = arr_slot(&t->posts, p);
            *slot = pi;
            TagRef *r = arr_slot(&tagref_store, p);
            r->name = t->name; r->slug = t->slug; r->url = t->url; r->clean_mask = t->clean_mask;
        }
        Range *ra = arr_at(&tagref_arrays, pi);
        ra->start = start; ra->len = (uint32_t)tagref_store.len - start;
        Page *pd = arr_at(&pages, pi);
        pd->tags = pi;
    }
    /* Pass 2: the non-post pages, in source order. */
    for (size_t pi = 0; pi < pages.len; pi++) {
        Page *pd = arr_at(&pages, pi);
        if (pd->is_post) continue;
        Arr *names = arr_at(&fm_tags, pi);
        size_t nown = page_tags(p, names, &tag_slug_cache, own, 64);
        uint32_t start = (uint32_t)tagref_store.len;
        for (size_t j = 0; j < nown; j++) {
            Tag *t = 0;
            Sp hit;
            if (dict_get(&tag_by_slug, p, pool_at(p, own[j].slug), own[j].slug.len, &hit)) {
                t = arr_at(&tags, hit.off);
            }
            TagRef *r = arr_slot(&tagref_store, p);
            if (t) { r->name = t->name; r->slug = t->slug; r->url = t->url; r->clean_mask = t->clean_mask; }
            else { r->name = own[j].name; r->slug = own[j].slug; r->url = SP_NONE; r->clean_mask = (uint8_t)((span_is_clean(p, r->name) ? 1 : 0) | 2); }
        }
        Range *ra = arr_at(&tagref_arrays, pi);
        ra->start = start; ra->len = (uint32_t)tagref_store.len - start;
        pd->tags = (uint32_t)pi;
    }
    arr_sort(&tags, p, p, cmp_tags);

    /* card arrays: [0] = recent, [1..1+tags] = per-tag, then per index page. */
    Arr card_arrays; arr_new(&card_arrays, sizeof(Arr));
    Arr *recent_arr = arr_slot(&card_arrays, p);
    arr_new(recent_arr, sizeof(uint32_t));
    {
        int64_t n = c->site.recent;
        int64_t len = (int64_t)post_order.len;
        int64_t end = n < 0 ? (len + n > 0 ? len + n : 0) : (n < len ? n : len);
        if (end < 0) end = 0;
        for (int64_t i = 0; i < end; i++) {
            uint32_t *slot = arr_slot(recent_arr, p);
            *slot = *(uint32_t *)arr_at(&post_order, (size_t)i);
        }
    }
    for (size_t ti = 0; ti < tags.len; ti++) {
        Tag *t = arr_at(&tags, ti);
        Arr *arr = arr_slot(&card_arrays, p);
        arr_new(arr, sizeof(uint32_t));
        for (size_t k = 0; k < t->posts.len; k++) {
            uint32_t *slot = arr_slot(arr, p);
            *slot = *(uint32_t *)arr_at(&t->posts, k);
        }
    }
    int64_t per = c->site.per_page < 1 ? 1 : c->site.per_page;
    int64_t npages = (int64_t)((post_order.len + (size_t)per - 1) / (size_t)per);
    if (npages < 1) npages = 1;
    Arr pagers; arr_new(&pagers, sizeof(Pager));
    for (int64_t k = 1; k <= npages; k++) {
        size_t start = (size_t)((k - 1) * per);
        int64_t e = k * per;
        if (e > (int64_t)post_order.len) e = (int64_t)post_order.len;
        Arr *arr = arr_slot(&card_arrays, p);
        arr_new(arr, sizeof(uint32_t));
        for (size_t i = start; i < (size_t)e; i++) {
            uint32_t *slot = arr_slot(arr, p);
            *slot = *(uint32_t *)arr_at(&post_order, i);
        }
        Sp url = idx_url(p, base, k);
        Sp newer_url = k == 1 ? SP_NONE : idx_url(p, base, k - 1);
        Sp older_url = k == npages ? SP_NONE : idx_url(p, base, k + 1);
        Pager *pg = arr_slot(&pagers, p);
        pg->number = k; pg->total = npages; pg->url = url;
        pg->newer_url = newer_url; pg->older_url = older_url;
    }

    static Pool cache_pool = {0, 0, 0, -1};
    if (!cache_pool.base) pool_open(&cache_pool, 1 << 23); /* 8 MB virtual, ~200 KB touched */
    cache_pool.len = 0;

    Ctx st;
    st.pool = p;
    st.cache_pool = &cache_pool;
    st.md_pre_json = md_pre_json;
    st.lazy_md = lazy_md;
    st.cur_html = 0;
    st.cur_html_len = 0;
    st.cur_toc = 0;
    st.site_title_clean = site_title_clean;
    st.site_base_clean = site_base_clean;
    tpl_cache_open(&st, p, &templates, pages.len);
    st.site_title = c->site.title;
    st.site_base = base;
    st.pages = pages;
    st.post_order = post_order;
    /* recent: a copy of card_arrays[0] */
    arr_new(&st.recent, sizeof(uint32_t));
    {
        Arr *r0 = arr_at(&card_arrays, 0);
        for (size_t i = 0; i < r0->len; i++) {
            uint32_t *slot = arr_slot(&st.recent, p);
            *slot = *(uint32_t *)arr_at(r0, i);
        }
    }
    st.tags = tags;
    st.tagref_store = tagref_store;
    st.toc_store = toc_store;
    st.card_arrays = card_arrays;
    st.tagref_arrays = tagref_arrays;
    st.toc_arrays = toc_arrays;
    st.pagers = pagers;

    /* layouts: render into the answers object. */
    Arr written_keys; arr_new(&written_keys, sizeof(Sp));
    static Str tscratch; str_clear(&tscratch);
    Scope frames[128];
    size_t nframes;
    pool_u8(out, '{');
    bool first = true;
    /* Pages, in source order. */
    Arr page_toc; arr_new(&page_toc, sizeof(Toc));
    Page *pages_ptr = st.pages.len ? arr_at(&st.pages, 0) : 0;
    RawFile *statics_ptr = statics.len ? arr_at(&statics, 0) : 0;
    for (size_t pi = 0; pi < st.pages.len; pi++) {
        Page *pd = arr_at(&st.pages, pi);
        if (pd->layout_tpl < 0) continue;
        if (lazy_md) {
            LinkCtx env;
            setup_link_env(p, &env, pages_ptr, st.pages.len, statics_ptr, statics.len,
                           &page_map, &static_set, base, pd, &broken,
                           &path_buf, &item_buf, &item_scratch);
            arr_clear(&page_toc);
            Sp html, excerpt;
            md_build_buf(p, pool_at(p, pd->body), pd->body.len, &env, &md_slug_cache, &scratch, &tmp, &text_tmp, &html, &page_toc, &excerpt, md_pre_json, false);
            pd->excerpt = excerpt;
            st.cur_html = scratch.data;
            st.cur_html_len = (uint32_t)scratch.len;
            st.cur_toc = &page_toc;
        }
        Tpl *tpl = arr_at(&templates, (size_t)pd->layout_tpl);
        uint32_t kst = pool_mark(p);
        pool_copy(p, pd->out_dir);
        pool_put(p, (const uint8_t *)"/index.html", 11);
        Sp key = pool_span(p, kst);
        if (!first) pool_u8(out, ',');
        first = false;
        pool_u8(out, '"');
        if (pd->clean_mask & 2) pool_put(out, pool_at(p, key), key.len);
        else json_escape_into(out, pool_at(p, key), key.len);
        pool_put(out, (const uint8_t *)"\":\"", 3);
        Sp *wk = arr_slot(&written_keys, p);
        *wk = key;
        nframes = 1;
        frames[0].is_root = true;
        frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
        frames[0].page = (int32_t)pi; frames[0].posts = -1; frames[0].pager = -1; frames[0].tag = -1;
        tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
        pool_u8(out, '"');
        maybe_flush(out);
    }
    /* Index pages. */
    {
        Tpl *tpl_idx = tpl_find(&templates, p, (const uint8_t *)"index", 5);
        for (int64_t k = 1; tpl_idx && k <= npages; k++) {
            Sp key;
            if (k == 1) {
                key = pool_store(p, (const uint8_t *)"index.html", 10);
            } else {
                uint32_t kst = pool_mark(p);
                pool_put(p, (const uint8_t *)"page/", 5);
                pool_i64(p, k);
                pool_put(p, (const uint8_t *)"/index.html", 11);
                key = pool_span(p, kst);
            }
            if (!first) pool_u8(out, ',');
            first = false;
            pool_u8(out, '"');
            pool_put(out, pool_at(p, key), key.len);
            pool_put(out, (const uint8_t *)"\":\"", 3);
            Sp *wk = arr_slot(&written_keys, p);
            *wk = key;
            nframes = 1;
            frames[0].is_root = true;
            frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
            frames[0].page = -1;
            frames[0].posts = (int32_t)(1 + st.tags.len + (size_t)(k - 1));
            frames[0].pager = (int32_t)(k - 1);
            frames[0].tag = -1;
            tpl_emit(&st, &templates, &tpl_idx->nodes, frames, &nframes, out, &tscratch, 0);
            pool_u8(out, '"');
            maybe_flush(out);
        }
    }
    /* Tag pages, in tag order. */
    {
        Tpl *tpl_tag = tpl_find(&templates, p, (const uint8_t *)"tag", 3);
        for (size_t ti = 0; tpl_tag && ti < st.tags.len; ti++) {
            Tag *t = arr_at(&st.tags, ti);
            uint32_t kst = pool_mark(p);
            pool_put(p, (const uint8_t *)"tags/", 5);
            pool_copy(p, t->slug);
            pool_put(p, (const uint8_t *)"/index.html", 11);
            Sp key = pool_span(p, kst);
            if (!first) pool_u8(out, ',');
            first = false;
            pool_u8(out, '"');
            pool_put(out, pool_at(p, key), key.len);
            pool_put(out, (const uint8_t *)"\":\"", 3);
            Sp *wk = arr_slot(&written_keys, p);
            *wk = key;
            nframes = 1;
            frames[0].is_root = true;
            frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
            frames[0].page = -1; frames[0].posts = -1; frames[0].pager = -1;
            frames[0].tag = (int32_t)ti;
            tpl_emit(&st, &templates, &tpl_tag->nodes, frames, &nframes, out, &tscratch, 0);
            pool_u8(out, '"');
            maybe_flush(out);
        }
    }
    /* Tags index. */
    {
        Tpl *tpl = tpl_find(&templates, p, (const uint8_t *)"tags", 4);
        if (tpl) {
            Sp key = pool_store(p, (const uint8_t *)"tags/index.html", 15);
            if (!first) pool_u8(out, ',');
            first = false;
            pool_put(out, (const uint8_t *)"\"tags/index.html\":\"", 19);
            Sp *wk = arr_slot(&written_keys, p);
            *wk = key;
            nframes = 1;
            frames[0].is_root = true;
            frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
            frames[0].page = -1; frames[0].posts = -1; frames[0].pager = -1; frames[0].tag = -1;
            tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
            pool_u8(out, '"');
            maybe_flush(out);
        }
    }
    /* Statics, in path order; shadowed ones are reported, not written. */
    if (statics.len > 0) {
        Set written_set; set_new(&written_set);
        for (size_t k = 0; k < written_keys.len; k++) {
            set_add(&written_set, p, *(Sp *)arr_at(&written_keys, k));
        }
        for (size_t i = 0; i < statics.len; i++) {
            RawFile *sp = arr_at(&statics, i);
            if (!set_add(&written_set, p, sp->path)) {
                uint32_t st2 = pool_mark(p);
                pool_put(p, (const uint8_t *)"shadowed ", 9);
                pool_copy(p, sp->path);
                pool_put(p, (const uint8_t *)": a built page has its path", 27);
                Sp span_st2 = pool_span(p, st2);
                Sp *line = arr_slot(&report, p);
                *line = span_st2;
                continue;
            }
            if (!first) pool_u8(out, ',');
            first = false;
            pool_u8(out, '"');
            json_escape_into(out, pool_at(p, sp->path), sp->path.len);
            pool_put(out, (const uint8_t *)"\":\"", 3);
            if ((sp->flags & RF_NO_ESC) && scan_json_esc(pool_at(p, sp->content), sp->content.len, 0) < 0) {
                pool_put(out, pool_at(p, sp->content), sp->content.len);
            } else {
                json_escape_into(out, pool_at(p, sp->content), sp->content.len);
            }
            pool_u8(out, '"');
            maybe_flush(out);
        }
    }
    /* report: sorted lines (broken links deduped) + the counts line. */
    if (broken.len > 1) arr_sort(&broken, p, p, cmp_sp);
    if (broken.len > 0) {
        size_t w = 0;
        for (size_t r = 1; r < broken.len; r++) {
            Sp *a = arr_at(&broken, w);
            Sp *b = arr_at(&broken, r);
            if (a->len != b->len || cmp_bytes(pool_at(p, *a), pool_at(p, *b), a->len) != 0) {
                w++;
                Sp *dst = arr_at(&broken, w);
                *dst = *b;
            }
        }
        broken.len = w + 1;
    }
    for (size_t i = 0; i < broken.len; i++) {
        Sp *line = arr_slot(&report, p);
        *line = *(Sp *)arr_at(&broken, i);
    }
    if (report.len > 1) arr_sort(&report, p, p, cmp_sp);
    size_t npages_count = 0;
    for (size_t i = 0; i < pages.len; i++) {
        Page *pd = arr_at(&pages, i);
        if (!pd->is_post) npages_count++;
    }
    {
        uint32_t st2 = pool_mark(p);
        pool_put(p, (const uint8_t *)"built ", 6);
        pool_u64(p, npages_count);
        pool_put(p, (const uint8_t *)" pages, ", 8);
        pool_u64(p, post_order.len);
        pool_put(p, (const uint8_t *)" posts, ", 8);
        pool_u64(p, tags.len);
        pool_put(p, (const uint8_t *)" tags and ", 10);
        pool_u64(p, statics.len);
        pool_put(p, (const uint8_t *)" static files", 13);
        Sp span_st2 = pool_span(p, st2);
        Sp *line = arr_slot(&report, p);
        *line = span_st2;
    }
    /* build-report.txt (lines joined by '\n', trailing '\n'). */
    {
        Sp report_key = pool_store(p, (const uint8_t *)"build-report.txt", 16);
        if (!first) pool_u8(out, ',');
        pool_u8(out, '"');
        json_escape_into(out, pool_at(p, report_key), report_key.len);
        pool_put(out, (const uint8_t *)"\":", 2);
        pool_u8(out, '"');
        bool first_line = true;
        for (size_t i = 0; i < report.len; i++) {
            if (!first_line) {
                pool_u8(out, '\\');
                pool_u8(out, 'n');
            }
            first_line = false;
            Sp *line = arr_at(&report, i);
            json_escape_into(out, pool_at(p, *line), line->len);
        }
        pool_u8(out, '\\');
        pool_u8(out, 'n');
        pool_u8(out, '"');
    }
    pool_u8(out, '}');
    return true;
}

bool cases_build(Pool *p, const uint8_t *input, size_t n, Pool *out) {
    Cur cur = {input, p, 0, n};
    if (!json_all_begin(&cur)) return false;
    size_t reset_len = p->len;
    pool_u8(out, '[');
    bool first = true;
    for (;;) {
        p->len = reset_len;
        CaseInput c;
        int st = json_all_next(&cur, &c);
        if (st < 0) return false;
        if (st == 0) break;
        if (!first) pool_u8(out, ',');
        first = false;
        if (!case_build(p, input, &c, out)) return false;
    }
    pool_u8(out, ']');
    return true;
}
