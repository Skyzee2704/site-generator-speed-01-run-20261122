/* zg.h — shared declarations for our freestanding site generator.
 * A from-scratch re-expression of the makefun-site 1.0.0 semantics
 * (front matter, Markdown subset, template dialect, build pipeline),
 * written in freestanding C: raw syscalls only, no libc, no dynamic
 * loader. Same value model and output bytes as the reference; the
 * code layout, names and internals are our own. */
#ifndef ZG_H
#define ZG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <emmintrin.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

typedef struct Dict Dict;
typedef struct Set Set;

/* ---------------- CPU feature flags (runtime CPUID) ---------------- */
extern uint8_t g_has_avx2;
extern uint8_t g_has_popcnt;
void cpu_detect(void);

/* ---------------- raw syscalls / memory ---------------- */
long raw_call(long n, long a, long b, long c, long d, long e, long f);
void die(int code);
long os_open(const char *p, int flags, int mode);
long os_read(int fd, void *b, long n);
long os_write(int fd, const void *b, long n);
long os_close(int fd);
void *os_map(long n);
void *os_map_huge(size_t n);
void *os_alloc(size_t n);
void *copy_bytes(void *d, const void *s, size_t n);
void *fill_bytes(void *d, int c, size_t n);
int cmp_bytes(const void *a, const void *b, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

static inline void copy_fast(void *d, const void *s, size_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    if (__builtin_expect(n <= 16, 1)) {
        if (n >= 8) {
            uint64_t a, b;
            __builtin_memcpy(&a, ss, 8);
            __builtin_memcpy(&b, ss + n - 8, 8);
            __builtin_memcpy(dd, &a, 8);
            __builtin_memcpy(dd + n - 8, &b, 8);
            return;
        }
        if (n >= 4) {
            uint32_t a, b;
            __builtin_memcpy(&a, ss, 4);
            __builtin_memcpy(&b, ss + n - 4, 4);
            __builtin_memcpy(dd, &a, 4);
            __builtin_memcpy(dd + n - 4, &b, 4);
            return;
        }
        if (n >= 2) {
            uint16_t a, b;
            __builtin_memcpy(&a, ss, 2);
            __builtin_memcpy(&b, ss + n - 2, 2);
            __builtin_memcpy(dd, &a, 2);
            __builtin_memcpy(dd + n - 2, &b, 2);
            return;
        }
        if (n == 1) *dd = *ss;
        return;
    }
    copy_bytes(d, s, n);
}

static inline bool eq_bytes(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    if (n <= 8) {
        if (n == 8) { uint64_t u, v; __builtin_memcpy(&u, x, 8); __builtin_memcpy(&v, y, 8); return u == v; }
        if (n >= 4) {
            uint32_t u1, v1, u2, v2;
            __builtin_memcpy(&u1, x, 4); __builtin_memcpy(&v1, y, 4);
            __builtin_memcpy(&u2, x + n - 4, 4); __builtin_memcpy(&v2, y + n - 4, 4);
            return u1 == v1 && u2 == v2;
        }
        if (n >= 2) {
            uint16_t u1, v1;
            __builtin_memcpy(&u1, x, 2); __builtin_memcpy(&v1, y, 2);
            return u1 == v1 && x[n - 1] == y[n - 1];
        }
        return n == 0 || *x == *y;
    }
    return cmp_bytes(a, b, n) == 0;
}

static inline uint64_t hash_bytes(const uint8_t *b, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (uint64_t)n * 0x517cc1b727220a95ULL;
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t w;
        __builtin_memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
        i += 8;
    }
    if (i < n) {
        uint64_t w = 0;
        if (i + 4 <= n) { uint32_t t; __builtin_memcpy(&t, b + i, 4); w = t; i += 4; }
        while (i < n) { w = (w << 8) | b[i++]; }
        h = (h ^ w) * 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
    }
    return h;
}

/* ---------------- arena pool ---------------- */
/* A span is an (offset, length) pair into a pool's bytes. */
typedef struct { uint32_t off; uint32_t len; } Sp;
#define SP_NONE ((Sp){0, 0})

typedef struct {
    uint8_t *base;
    size_t len;
    size_t cap;
} Pool;

void pool_open(Pool *p, size_t cap);
static inline uint8_t *pool_at(Pool *p, Sp s) { return p->base + s.off; }
static inline uint32_t pool_mark(Pool *p) { return (uint32_t)p->len; }
static inline Sp pool_span(Pool *p, uint32_t start) { return (Sp){start, (uint32_t)(p->len - start)}; }
static inline void pool_u8(Pool *p, uint8_t c) { p->base[p->len++] = c; }
static inline void *pool_alloc(Pool *p, size_t n) { void *q = p->base + p->len; p->len += n; return q; }
static inline void pool_put(Pool *p, const uint8_t *src, size_t n) {
    copy_fast(p->base + p->len, src, n);
    p->len += n;
}
#define pool_lit(p, lit) pool_put((p), (const uint8_t *)(lit), sizeof(lit) - 1)
static inline void pool_copy(Pool *p, Sp s) {
    copy_fast(p->base + p->len, p->base + s.off, s.len);
    p->len += s.len;
}
void pool_u64(Pool *p, uint64_t v);
void pool_i64(Pool *p, int64_t n);
static inline Sp pool_store(Pool *p, const uint8_t *src, size_t n) {
    uint32_t off = (uint32_t)p->len;
    copy_fast(p->base + p->len, src, n);
    p->len += n;
    return (Sp){off, (uint32_t)n};
}

/* ---------------- generic array ---------------- */
typedef struct { void *items; size_t len; size_t cap; size_t esz; } Arr;
static inline void arr_new(Arr *a, size_t esz) { a->items = 0; a->len = 0; a->cap = 0; a->esz = esz; }
void arr_grow(Arr *a, Pool *p, size_t n);
static inline void *arr_slot(Arr *a, Pool *p) {
    if (__builtin_expect(a->len >= a->cap, 0)) arr_grow(a, p, a->len + 1);
    void *slot = (uint8_t *)a->items + a->len * a->esz;
    fill_bytes(slot, 0, a->esz);
    a->len++;
    return slot;
}
static inline void *arr_push_raw(Arr *a, Pool *p) {
    if (__builtin_expect(a->len >= a->cap, 0)) arr_grow(a, p, a->len + 1);
    void *slot = (uint8_t *)a->items + a->len * a->esz;
    a->len++;
    return slot;
}
static inline void *arr_at(Arr *a, size_t i) { return (uint8_t *)a->items + i * a->esz; }
void arr_sort(Arr *a, Pool *p, void *ctx, int (*cmp)(void *ctx, const void *x, const void *y));
static inline void arr_clear(Arr *a) { a->len = 0; }

/* ---------------- byte string ---------------- */
typedef struct { uint8_t *data; size_t len; size_t cap; } Str;
static inline void str_new(Str *s) { s->data = 0; s->len = 0; s->cap = 0; }
void str_grow(Str *s, size_t n);
static inline void str_c(Str *s, uint8_t c) {
    if (__builtin_expect(s->len >= s->cap, 0)) str_grow(s, s->len + 1);
    s->data[s->len++] = c;
}
static inline void str_add(Str *s, const uint8_t *p, size_t n) {
    if (__builtin_expect(s->len + n > s->cap, 0)) str_grow(s, s->len + n);
    copy_fast(s->data + s->len, p, n);
    s->len += n;
}
#define str_lit(s, lit) str_add((s), (const uint8_t *)(lit), sizeof(lit) - 1)
static inline void str_cstr(Str *s, const char *t) {
    size_t n = 0; while (t[n]) n++;
    str_add(s, (const uint8_t *)t, n);
}
void str_i64(Str *s, int64_t n);
static inline void str_clear(Str *s) { s->len = 0; }

/* ---------------- SIMD scans (runtime AVX2 / baseline SSE2) ---------------- */
int scan_byte(const uint8_t *b, size_t n, size_t from, uint8_t c);
int scan_two(const uint8_t *b, size_t n, size_t from, uint8_t c1, uint8_t c2);
int scan_any(const uint8_t *b, size_t n, size_t from, const uint8_t *needles, size_t nneedles);
int scan_json_esc(const uint8_t *b, size_t n, size_t from);
int scan_html_esc(const uint8_t *b, size_t n, size_t from);
int scan_inline_md(const uint8_t *b, size_t n, size_t from);
int scan_inline_md_json(const uint8_t *b, size_t n, size_t from);
int scan_nl(const uint8_t *b, size_t n, size_t from);
int scan_html_json_esc(const uint8_t *b, size_t n, size_t from);

/* ---------------- UTF-8 / text ---------------- */
int u8_decode(const uint8_t *b, size_t n, size_t i, uint32_t *cp);
int u8_encode(uint32_t cp, uint8_t *out);
bool u_is_space(uint32_t cp);
int u16_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn);
uint32_t nfkd_find(uint32_t cp, uint32_t *out);
Sp slug_make(Pool *p, Sp text, Dict *cache);
Sp slug_unique(Pool *p, Sp slug, Set *taken);
size_t word_count(const uint8_t *b, size_t n);
Sp date_long(Pool *p, Sp d);
void json_escape_into(Pool *p, const uint8_t *b, size_t n);
void html_json_escape_into(Pool *p, const uint8_t *b, size_t n);

/* ---------------- string dict / set ---------------- */
struct Dict { Arr keys; Arr vals; Arr used; size_t mask; size_t len; };
struct Set { Arr keys; Arr used; size_t mask; size_t len; };
void dict_new(Dict *d);
bool dict_get(Dict *d, Pool *p, const uint8_t *key, size_t klen, Sp *out);
void dict_put(Dict *d, Pool *p, Sp key, Sp val);
bool dict_has(Dict *d, Pool *p, const uint8_t *key, size_t klen);
void set_new(Set *s);
bool set_add(Set *s, Pool *p, Sp val);
bool set_has(Set *s, Pool *p, const uint8_t *key, size_t klen);

/* ---------------- JSON input ---------------- */
typedef struct { Sp a; Sp b; uint8_t has_cr; } SpPair;
typedef struct {
    Sp title; Sp base_url; int64_t per_page; int64_t recent;
} Site;
typedef struct {
    Site site;
    Arr templates; /* SpPair */
    Arr files;     /* SpPair */
} CaseInput;

typedef struct {
    Pool *pool;
    size_t pos;
    size_t end;
} Cur;

bool json_read_case(Cur *c, CaseInput *out);
bool json_read_all(Cur *c, Arr *cases_out); /* CaseInput */

/* ---------------- front matter ---------------- */
typedef struct {
    Sp title; bool has_date; Sp date; Arr tags; /* Sp */
    bool draft; bool has_layout; Sp layout; bool has_slug; Sp slug; bool has_summary; Sp summary;
    Sp body;
} Meta;
bool meta_parse(Pool *p, Sp text, Meta *out, Sp *err);

/* ---------------- value model ---------------- */
enum VKind { K_VOID, K_NULL, K_STR, K_STR_JSON, K_INT, K_BOOL, K_ARR, K_OBJ, K_LOOP };
enum ArrKind { A_CARDS, A_TAGSUMS, A_TAGREFS, A_TOC };
enum ObjKind { O_SITE, O_PAGE, O_CARD, O_TAGSUM, O_TAG, O_TAGREF, O_TOC, O_LINK, O_PAGER };

typedef struct { int64_t index; bool first; bool last; int64_t length; } LoopInfo;

typedef struct {
    uint8_t kind;     /* VKind */
    uint8_t akind;    /* K_ARR: ArrKind; K_OBJ: ObjKind */
    uint16_t pad;
    uint32_t idx;     /* K_ARR / K_OBJ index; K_LOOP 1-based index */
    union {
        Sp s;         /* K_STR / K_STR_JSON */
        int64_t n;    /* K_INT / K_BOOL */
        uint64_t u;   /* K_LOOP length / raw bits */
    } v;
} Val;

static inline Val val_void(void) { Val r; r.kind = K_VOID; r.akind = 0; r.pad = 0; r.idx = 0; r.v.u = 0; return r; }
static inline Val val_null(void) { Val r; r.kind = K_NULL; r.akind = 0; r.pad = 0; r.idx = 0; r.v.u = 0; return r; }
static inline Val val_str(Sp s) { Val r; r.kind = K_STR; r.akind = 0; r.pad = 0; r.idx = 0; r.v.s = s; return r; }
static inline Val val_str_json(Sp s) { Val r; r.kind = K_STR_JSON; r.akind = 0; r.pad = 0; r.idx = 0; r.v.s = s; return r; }
static inline Val val_int(int64_t n) { Val r; r.kind = K_INT; r.akind = 0; r.pad = 0; r.idx = 0; r.v.n = n; return r; }
static inline Val val_bool(bool b) { Val r; r.kind = K_BOOL; r.akind = 0; r.pad = 0; r.idx = 0; r.v.n = b ? 1 : 0; return r; }
static inline Val val_arr(uint8_t kind, uint32_t idx) { Val r; r.kind = K_ARR; r.akind = kind; r.pad = 0; r.idx = idx; r.v.u = 0; return r; }
static inline Val val_obj(uint8_t kind, uint32_t idx) { Val r; r.kind = K_OBJ; r.akind = kind; r.pad = 0; r.idx = idx; r.v.u = 0; return r; }
static inline Val val_loop(LoopInfo lp) { Val r; r.kind = K_LOOP; r.akind = 0; r.pad = 0; r.idx = (uint32_t)lp.index; r.v.u = (uint64_t)lp.length; return r; }

/* Field ids. */
enum FId {
    F_TITLE, F_URL, F_DATE, F_DATE_LONG, F_SUMMARY, F_EXCERPT, F_READING_MINUTES,
    F_TAGS, F_SLUG, F_CONTENT, F_TOC, F_NEWER, F_OLDER, F_IS_POST, F_NAME, F_COUNT,
    F_POSTS, F_LEVEL, F_ID, F_HTML, F_INDEX, F_FIRST, F_LAST, F_LENGTH, F_BASE_URL,
    F_NUMBER, F_TOTAL, F_NEWER_URL, F_OLDER_URL
};
int field_id(const uint8_t *name, size_t n); /* -1 when unknown */

/* Static types. */
enum TyId {
    TY_VOID, TY_STR, TY_INT, TY_BOOL, TY_SITE, TY_PAGE, TY_CARD, TY_TAGSUM, TY_TAG,
    TY_TAGREF, TY_TOC, TY_LINK, TY_LOOP, TY_PAGER, TY_ARR_CARDS, TY_ARR_TAGSUMS,
    TY_ARR_TAGREFS, TY_ARR_TOC, TY_OBJ
};
uint8_t ty_field(uint8_t ty, uint8_t f); /* TY_VOID when the field does not exist */
uint8_t ty_elem(uint8_t ty);

typedef struct {
    Sp path; Sp title; Sp date; bool has_date; Sp date_long; Sp summary; Sp slug;
    Sp out_dir; Sp url; Sp html; Sp excerpt; int64_t reading_minutes;
    uint32_t toc;      /* index into toc_arrays */
    uint32_t tags;     /* index into tagref_arrays */
    int32_t newer;     /* page index or -1 */
    int32_t older;     /* page index or -1 */
    bool is_post; uint8_t clean_mask; /* bit0=title, bit1=url, bit2=summary */
    int32_t layout_tpl;
    Sp layout; Sp body;
} Page;

typedef struct { Sp name; Sp slug; Sp url; Arr posts; /* u32 */ uint8_t clean_mask; /* bit0=name, bit1=url */ } Tag;
typedef struct { Sp name; Sp slug; Sp url; uint8_t clean_mask; /* bit0=name, bit1=url */ } TagRef;
typedef struct { int64_t level; Sp id; Sp html; } Toc;
typedef struct { int64_t number; int64_t total; Sp url; Sp newer_url; Sp older_url; } Pager;
typedef struct { uint32_t start; uint32_t len; } Range;

typedef struct {
    Pool *pool;
    Sp site_title; Sp site_base;
    Arr pages;         /* Page */
    Arr post_order;    /* u32 */
    Arr recent;        /* u32 */
    Arr tags;          /* Tag */
    Arr tagref_store;  /* TagRef */
    Arr toc_store;     /* Toc */
    Arr card_arrays;   /* Arr<u32> (each element is an Arr struct) */
    Arr tagref_arrays; /* Range */
    Arr toc_arrays;    /* Range */
    Arr pagers;        /* Pager */
    int32_t *tpl_index; /* open-addressing index into tpl_entries, -1 empty */
    size_t tpl_mask;
    Arr tpl_entries; /* TplHit */
    Sp *tpl_global;     /* [ntpls * 8], off==0xFFFFFFFFu means empty */
    Sp *tpl_card_cache; /* [n_card_slots * npages], off==0xFFFFFFFFu means empty */
    bool md_pre_json;
    bool site_title_clean;
    bool site_base_clean;
} Ctx;

size_t ctx_arr_len(Ctx *c, uint8_t kind, uint32_t idx);
Val ctx_arr_elem(Ctx *c, uint8_t kind, uint32_t idx, size_t i);
Val val_field(Ctx *c, Val v, uint8_t f);
bool val_true(Ctx *c, Val v);
void val_text(Ctx *c, Val v, Str *scratch);
void val_write(Ctx *c, Val v, Pool *out, bool esc, Str *scratch);

/* ---------------- templates ---------------- */
enum RootField { RF_SITE, RF_TAGS, RF_RECENT, RF_PAGE, RF_POSTS, RF_PAGER, RF_TAG };
int rootfield_id(const uint8_t *name, size_t n); /* -1 when unknown */
uint8_t rootfield_ty(uint8_t rf);

enum RefKind { REF_VOID, REF_ROOT, REF_SCOPE, REF_DYN };
typedef struct {
    uint8_t kind;       /* RefKind */
    uint8_t root_field; /* REF_ROOT */
    uint8_t offset;     /* REF_SCOPE: scopes below the current top */
    uint8_t is_loop;    /* REF_SCOPE */
    uint8_t chain_len;
    uint8_t chain[15];  /* uint8_t FId */
    Sp name;            /* REF_SCOPE / REF_DYN */
} Ref;

enum NodeKind { ND_TEXT, ND_VAR, ND_IF, ND_FOR, ND_INCLUDE };
typedef struct Node Node;
struct Node {
    uint8_t kind;       /* NodeKind */
    uint8_t raw;        /* ND_VAR: {{{ }}} */
    uint8_t neg;        /* ND_IF: "not" */
    uint8_t has_else;   /* ND_IF */
    union {
        Sp text;        /* ND_TEXT */
        struct { Ref ref; } var; /* ND_VAR */
        struct { Ref ref; Arr then_nodes; Arr else_nodes; } nif; /* ND_IF */
        struct { Ref ref; Sp binder; Arr body; } nfor; /* ND_FOR */
        struct { Sp name; int32_t tpl_idx; } ninc; /* ND_INCLUDE */
    } u;
};

typedef struct {
    Sp name; Arr nodes; /* Node */ Arr needs; /* TplNeed */
    uint8_t cacheable; uint8_t storable; uint8_t cache_mode; uint8_t card_slot;
    Sp dyn_name;
} Tpl;

/* A template's dependencies: the caller/root values its rendering can
 * read (transitively through includes). REF_SCOPE refs are internal
 * (their values derive from the template's own for-lists). */
typedef struct { uint8_t kind; /* 0=dyn name, 1=root field */ Sp name; uint8_t root_field; } TplNeed;

typedef struct { uint64_t hash; uint32_t tpl_idx; uint8_t depth; uint8_t nkeys; Val keys[8]; Sp bytes; } TplHit;

typedef struct {
    bool is_root;
    /* root scope */
    int32_t page; int32_t posts; int32_t pager; int32_t tag;
    /* for scope */
    Sp name; Val value; LoopInfo lp;
} Scope;

bool tpl_parse(Pool *p, Sp name, Sp src, Arr *nodes_out, Sp *err);
void tpl_analyze(Pool *p, Arr *templates);
bool tpl_can_pre_json_md(Arr *templates);
void tpl_cache_open(Ctx *c, Pool *p, Arr *templates, size_t npages);
Val tpl_eval(Ctx *c, Scope *scopes, size_t nscopes, Ref *ref);
void tpl_emit(Ctx *c, Arr *templates, Arr *nodes, Scope *scopes, size_t *nscopes,
              Pool *out, Str *scratch, uint32_t depth);

/* ---------------- markdown ---------------- */
typedef struct {
    Page *pages; size_t npages;
    SpPair *statics; size_t nstatics;
    Dict *page_map;  /* path -> Sp{page_idx, 0} */
    Set *static_set; /* static paths (when nstatics > 0) */
    Sp base; Sp dir; Sp src_path;
    const uint8_t *dir_parts[64];
    size_t dir_plens[64];
    size_t dir_np;
    Arr *broken; /* Sp, pool spans */
    Str *path_buf; /* reused scratch for resolve_path */
    Str *item_buf; /* reused scratch for the current list item's text */
    Str *item_scratch; /* reused scratch for accumulating a list item */
} LinkCtx;
bool md_build(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
              Str *scratch, Str *tmp, Str *text_tmp,
              Sp *html_out, Arr *toc_out, Sp *excerpt_out, bool json_esc);

/* ---------------- build ---------------- */
bool case_build(Pool *p, CaseInput *c, Pool *out);
bool cases_build(Pool *p, const uint8_t *input, size_t n, Pool *out);

#endif
