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
    /* Fast SSE scan for any '\r'; the common case has none (zero-copy). */
    static const uint8_t cr[1] = {'\r'};
    bool has_cr = scan_any(p->base, end, off, cr, 1) >= 0;
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

static bool tpl_has(Arr *templates, Pool *p, const uint8_t *name, size_t n) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->name.len == n && cmp_bytes(pool_at(p, t->name), name, n) == 0) return true;
    }
    return false;
}

static Tpl *tpl_find(Arr *templates, Pool *p, const uint8_t *name, size_t n) {
    for (size_t k = 0; k < templates->len; k++) {
        Tpl *t = arr_at(templates, k);
        if (t->name.len == n && cmp_bytes(pool_at(p, t->name), name, n) == 0) return t;
    }
    return 0;
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

bool case_build(Pool *p, CaseInput *c, Pool *out) {
    /* readSources: sort files by path (code-unit order). */
    arr_sort(&c->files, p, p, cmp_pair);
    arr_sort(&c->templates, p, p, cmp_pair);
    Sp base = base_strip(p, c->site.base_url);

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

    /* frontMatter + permalinks (per source, in sorted path order). */
    Arr pages; arr_new(&pages, sizeof(Page));
    Arr fm_tags; arr_new(&fm_tags, sizeof(Arr)); /* Arr<Sp> per page */
    Arr statics; arr_new(&statics, sizeof(SpPair));
    Set taken_root; set_new(&taken_root);
    for (int r = 0; r < 3; r++) {
        Sp rs = pool_store(p, RESERVED_DIRS[r], RESERVED_LEN[r]);
        set_add(&taken_root, p, rs);
    }
    Arr taken_sets; arr_new(&taken_sets, sizeof(DirTaken));
    Dict slug_cache; dict_new(&slug_cache);

    for (size_t fi = 0; fi < c->files.len; fi++) {
        SpPair *fp = arr_at(&c->files, fi);
        Sp path = fp->a, content = fp->b;
        if (!path_is_md(p, path)) {
            SpPair *st = arr_slot(&statics, p);
            st->a = path; st->b = content;
            continue;
        }
        Sp text = crlf_norm(p, content);
        Meta fm;
        Sp fm_err;
        if (!meta_parse(p, text, &fm, &fm_err)) {
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
        Sp layout = fm.has_layout ? fm.layout
            : pool_store(p, (const uint8_t *)(is_post ? "post" : "page"), 4);
        if (!tpl_has(&templates, p, pool_at(p, layout), layout.len)) {
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
        Sp dir = is_post ? pool_store(p, (const uint8_t *)"posts", 5) : path_dir(p, path);
        Sp slug_base = fm.has_slug ? fm.slug : slug_make(p, fm.title, &slug_cache);
        Sp slug;
        if (dir.len == 0) {
            slug = slug_unique(p, slug_base, &taken_root);
        } else {
            int idx = -1;
            for (size_t k = 0; k < taken_sets.len; k++) {
                DirTaken *td = arr_at(&taken_sets, k);
                if (td->dir.len == dir.len && cmp_bytes(pool_at(p, td->dir), pool_at(p, dir), dir.len) == 0) { idx = (int)k; break; }
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
        pd->html = SP_NONE; pd->excerpt = SP_NONE; pd->reading_minutes = 0;
        pd->toc = 0; pd->tags = 0; pd->newer = -1; pd->older = -1;
        pd->is_post = is_post; pd->layout = layout; pd->body = fm.body;
        Arr *ft = arr_slot(&fm_tags, p);
        *ft = fm.tags;
    }

    /* markdown: render each page's body, links resolved against the kept
     * pages (by source path) and the statics. */
    Arr broken; arr_new(&broken, sizeof(Sp));
    Arr toc_store; arr_new(&toc_store, sizeof(Toc));
    Arr toc_arrays; arr_new(&toc_arrays, sizeof(Range));
    Dict md_slug_cache; dict_new(&md_slug_cache);
    Str scratch; str_new(&scratch);
    Str tmp; str_new(&tmp);
    Str text_tmp; str_new(&text_tmp);
    Str path_buf; str_new(&path_buf);
    Str item_buf; str_new(&item_buf);
    Str item_scratch; str_new(&item_scratch);
    for (size_t pi = 0; pi < pages.len; pi++) {
        Page *pd = arr_at(&pages, pi);
        LinkCtx env;
        env.pages = arr_at(&pages, 0);
        env.npages = pages.len;
        env.statics = statics.len ? arr_at(&statics, 0) : 0;
        env.nstatics = statics.len;
        env.base = base;
        env.dir = path_dir(p, pd->path);
        env.src_path = pd->path;
        env.broken = &broken;
        env.path_buf = &path_buf;
        env.item_buf = &item_buf;
        env.item_scratch = &item_scratch;
        Sp html, excerpt;
        Arr toc; arr_new(&toc, sizeof(Toc));
        md_build(p, pd->body, &env, &md_slug_cache, &scratch, &tmp, &text_tmp, &html, &toc, &excerpt);
        uint32_t start = (uint32_t)toc_store.len;
        for (size_t k = 0; k < toc.len; k++) {
            Toc *t = arr_slot(&toc_store, p);
            *t = *(Toc *)arr_at(&toc, k);
        }
        Range *ra = arr_slot(&toc_arrays, p);
        ra->start = start; ra->len = (uint32_t)toc.len;
        pd->html = html;
        pd->excerpt = excerpt;
        pd->toc = (uint32_t)pi;
        size_t w = word_count(pool_at(p, pd->body), pd->body.len);
        int64_t rm = (int64_t)((w + 199) / 200);
        pd->reading_minutes = rm < 1 ? 1 : rm;
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
    typedef struct { Sp slug; uint32_t idx; } TagBySlug;
    Arr tag_by_slug; arr_new(&tag_by_slug, sizeof(TagBySlug));
    Dict tag_slug_cache; dict_new(&tag_slug_cache);
    NameSlug own[64];
    /* Pass 1: the posts, in post order. */
    for (size_t k = 0; k < post_order.len; k++) {
        uint32_t pi = *(uint32_t *)arr_at(&post_order, k);
        Arr *names = arr_at(&fm_tags, pi);
        size_t nown = page_tags(p, names, &tag_slug_cache, own, 64);
        uint32_t start = (uint32_t)tagref_store.len;
        for (size_t j = 0; j < nown; j++) {
            uint32_t ti = 0; bool found = false;
            for (size_t m = 0; m < tag_by_slug.len; m++) {
                TagBySlug *e = arr_at(&tag_by_slug, m);
                if (e->slug.len == own[j].slug.len && cmp_bytes(pool_at(p, e->slug), pool_at(p, own[j].slug), own[j].slug.len) == 0) {
                    ti = e->idx; found = true; break;
                }
            }
            if (!found) {
                uint32_t st = pool_mark(p);
                pool_copy(p, base);
                pool_put(p, (const uint8_t *)"/tags/", 6);
                pool_copy(p, own[j].slug);
                pool_u8(p, '/');
                Sp url = pool_span(p, st);
                ti = (uint32_t)tags.len;
                Tag *t = arr_slot(&tags, p);
                t->name = own[j].name; t->slug = own[j].slug; t->url = url;
                arr_new(&t->posts, sizeof(uint32_t));
                TagBySlug *e = arr_slot(&tag_by_slug, p);
                e->slug = own[j].slug; e->idx = ti;
            }
            Tag *t = arr_at(&tags, ti);
            uint32_t *slot = arr_slot(&t->posts, p);
            *slot = pi;
            TagRef *r = arr_slot(&tagref_store, p);
            r->name = t->name; r->slug = t->slug; r->url = t->url;
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
            for (size_t m = 0; m < tag_by_slug.len; m++) {
                TagBySlug *e = arr_at(&tag_by_slug, m);
                if (e->slug.len == own[j].slug.len && cmp_bytes(pool_at(p, e->slug), pool_at(p, own[j].slug), own[j].slug.len) == 0) {
                    t = arr_at(&tags, e->idx);
                    break;
                }
            }
            TagRef *r = arr_slot(&tagref_store, p);
            if (t) { r->name = t->name; r->slug = t->slug; r->url = t->url; }
            else { r->name = own[j].name; r->slug = own[j].slug; r->url = SP_NONE; }
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

    Ctx st;
    st.pool = p;
    tpl_cache_open(&st, p);
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
    Str tscratch; str_new(&tscratch);
    Scope frames[128];
    size_t nframes;
    pool_u8(out, '{');
    bool first = true;
    /* Pages, in source order. */
    for (size_t pi = 0; pi < st.pages.len; pi++) {
        Page *pd = arr_at(&st.pages, pi);
        Tpl *tpl = tpl_find(&templates, p, pool_at(p, pd->layout), pd->layout.len);
        if (!tpl) continue;
        uint32_t kst = pool_mark(p);
        pool_copy(p, pd->out_dir);
        pool_put(p, (const uint8_t *)"/index.html", 11);
        Sp key = pool_span(p, kst);
        if (!first) pool_u8(out, ',');
        first = false;
        pool_u8(out, '"');
        json_escape_into(out, pool_at(p, key), key.len);
        pool_put(out, (const uint8_t *)"\":", 2);
        pool_u8(out, '"');
        Sp *wk = arr_slot(&written_keys, p);
        *wk = key;
        nframes = 1;
        frames[0].is_root = true;
        frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
        frames[0].page = (int32_t)pi; frames[0].posts = -1; frames[0].pager = -1; frames[0].tag = -1;
        tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
        pool_u8(out, '"');
    }
    /* Index pages. */
    for (int64_t k = 1; k <= npages; k++) {
        Tpl *tpl = tpl_find(&templates, p, (const uint8_t *)"index", 5);
        if (!tpl) break;
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
        json_escape_into(out, pool_at(p, key), key.len);
        pool_put(out, (const uint8_t *)"\":", 2);
        pool_u8(out, '"');
        Sp *wk = arr_slot(&written_keys, p);
        *wk = key;
        nframes = 1;
        frames[0].is_root = true;
        frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
        frames[0].page = -1;
        frames[0].posts = (int32_t)(1 + st.tags.len + (size_t)(k - 1));
        frames[0].pager = (int32_t)(k - 1);
        frames[0].tag = -1;
        tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
        pool_u8(out, '"');
    }
    /* Tag pages, in tag order. */
    for (size_t ti = 0; ti < st.tags.len; ti++) {
        Tpl *tpl = tpl_find(&templates, p, (const uint8_t *)"tag", 3);
        if (!tpl) break;
        Tag *t = arr_at(&st.tags, ti);
        uint32_t kst = pool_mark(p);
        pool_put(p, (const uint8_t *)"tags/", 5);
        pool_copy(p, t->slug);
        pool_put(p, (const uint8_t *)"/index.html", 11);
        Sp key = pool_span(p, kst);
        if (!first) pool_u8(out, ',');
        first = false;
        pool_u8(out, '"');
        json_escape_into(out, pool_at(p, key), key.len);
        pool_put(out, (const uint8_t *)"\":", 2);
        pool_u8(out, '"');
        Sp *wk = arr_slot(&written_keys, p);
        *wk = key;
        nframes = 1;
        frames[0].is_root = true;
        frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
        frames[0].page = -1; frames[0].posts = -1; frames[0].pager = -1;
        frames[0].tag = (int32_t)ti;
        tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
        pool_u8(out, '"');
    }
    /* Tags index. */
    {
        Tpl *tpl = tpl_find(&templates, p, (const uint8_t *)"tags", 4);
        if (tpl) {
            Sp key = pool_store(p, (const uint8_t *)"tags/index.html", 15);
            if (!first) pool_u8(out, ',');
            first = false;
            pool_u8(out, '"');
            json_escape_into(out, pool_at(p, key), key.len);
            pool_put(out, (const uint8_t *)"\":", 2);
            pool_u8(out, '"');
            Sp *wk = arr_slot(&written_keys, p);
            *wk = key;
            nframes = 1;
            frames[0].is_root = true;
            frames[0].name = SP_NONE; frames[0].value = val_void(); frames[0].lp = (LoopInfo){0, false, false, 0};
            frames[0].page = -1; frames[0].posts = -1; frames[0].pager = -1; frames[0].tag = -1;
            tpl_emit(&st, &templates, &tpl->nodes, frames, &nframes, out, &tscratch, 0);
            pool_u8(out, '"');
        }
    }
    /* Statics, in path order; shadowed ones are reported, not written. */
    for (size_t i = 0; i < statics.len; i++) {
        SpPair *sp = arr_at(&statics, i);
        bool shadowed = false;
        for (size_t k = 0; k < written_keys.len; k++) {
            Sp *wk = arr_at(&written_keys, k);
            if (wk->len == sp->a.len && cmp_bytes(pool_at(p, *wk), pool_at(p, sp->a), sp->a.len) == 0) { shadowed = true; break; }
        }
        if (shadowed) {
            uint32_t st2 = pool_mark(p);
            pool_put(p, (const uint8_t *)"shadowed ", 9);
            pool_copy(p, sp->a);
            pool_put(p, (const uint8_t *)": a built page has its path", 27);
            Sp span_st2 = pool_span(p, st2);
            Sp *line = arr_slot(&report, p);
            *line = span_st2;
            continue;
        }
        if (!first) pool_u8(out, ',');
        first = false;
        pool_u8(out, '"');
        json_escape_into(out, pool_at(p, sp->a), sp->a.len);
        pool_put(out, (const uint8_t *)"\":", 2);
        pool_u8(out, '"');
        json_escape_into(out, pool_at(p, sp->b), sp->b.len);
        pool_u8(out, '"');
        /* Vendor checks Object.hasOwn(output) for statics, so a written
           static's key must join written_keys (the reference C omits this
           and lets a duplicate static overwrite; the vendor keeps the first
           and reports "shadowed"). */
        Sp *wk2 = arr_slot(&written_keys, p);
        *wk2 = sp->a;
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
    /* The input already lives in the pool (entry.c reads it straight
     * there); its span starts at the input pointer's pool offset. */
    Sp input_s = {(uint32_t)(input - p->base), (uint32_t)n};
    Cur cur = {p, input_s.off, input_s.off + input_s.len};
    Arr cases; arr_new(&cases, sizeof(CaseInput));
    if (!json_read_all(&cur, &cases)) return false;
    pool_u8(out, '[');
    for (size_t i = 0; i < cases.len; i++) {
        if (i > 0) pool_u8(out, ',');
        CaseInput *c = arr_at(&cases, i);
        if (!case_build(p, c, out)) return false;
    }
    pool_u8(out, ']');
    return true;
}
