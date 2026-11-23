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

/* ---------------- raw syscalls / memory ---------------- */
long raw_call(long n, long a, long b, long c, long d, long e, long f);
void die(int code);
long os_open(const char *p, int flags, int mode);
long os_read(int fd, void *b, long n);
long os_write(int fd, const void *b, long n);
long os_close(int fd);
void *os_map(long n);
void *os_alloc(size_t n);
void *copy_bytes(void *d, const void *s, size_t n);
void *fill_bytes(void *d, int c, size_t n);
int cmp_bytes(const void *a, const void *b, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

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
void pool_put(Pool *p, const uint8_t *src, size_t n);
void pool_copy(Pool *p, Sp s);
void pool_u64(Pool *p, uint64_t v);
void pool_i64(Pool *p, int64_t n);
Sp pool_store(Pool *p, const uint8_t *src, size_t n);

/* ---------------- generic array ---------------- */
typedef struct { void *items; size_t len; size_t cap; size_t esz; } Arr;
void arr_new(Arr *a, size_t esz);
void arr_grow(Arr *a, Pool *p, size_t n);
void *arr_slot(Arr *a, Pool *p);
void *arr_at(Arr *a, size_t i);
void arr_sort(Arr *a, Pool *p, void *ctx, int (*cmp)(void *ctx, const void *x, const void *y));
static inline void arr_clear(Arr *a) { a->len = 0; }

/* ---------------- byte string ---------------- */
typedef struct { uint8_t *data; size_t len; size_t cap; } Str;
void str_new(Str *s);
void str_grow(Str *s, size_t n);
void str_c(Str *s, uint8_t c);
void str_add(Str *s, const uint8_t *p, size_t n);
void str_cstr(Str *s, const char *t);
void str_i64(Str *s, int64_t n);
static inline void str_clear(Str *s) { s->len = 0; }

/* ---------------- SSE2 scans ---------------- */
int scan_any(const uint8_t *b, size_t n, size_t from, const uint8_t *needles, size_t nneedles);
int scan_json_esc(const uint8_t *b, size_t n, size_t from);
int scan_html_esc(const uint8_t *b, size_t n, size_t from);
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
typedef struct { Sp a; Sp b; } SpPair;
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
enum VKind { K_VOID, K_NULL, K_STR, K_INT, K_BOOL, K_ARR, K_OBJ, K_LOOP };
enum ArrKind { A_CARDS, A_TAGSUMS, A_TAGREFS, A_TOC };
enum ObjKind { O_SITE, O_PAGE, O_CARD, O_TAGSUM, O_TAG, O_TAGREF, O_TOC, O_LINK, O_PAGER };

typedef struct { int64_t index; bool first; bool last; int64_t length; } LoopInfo;

typedef struct {
    uint8_t kind;     /* VKind */
    Sp s;             /* K_STR */
    int64_t n;        /* K_INT */
    bool b;           /* K_BOOL */
    uint8_t akind;    /* K_ARR: ArrKind; K_OBJ: ObjKind */
    uint32_t idx;     /* K_ARR / K_OBJ index */
    LoopInfo lp;      /* K_LOOP */
} Val;

static inline Val val_void(void) { Val v; v.kind = K_VOID; v.s = SP_NONE; v.n = 0; v.b = false; v.akind = 0; v.idx = 0; v.lp = (LoopInfo){0, false, false, 0}; return v; }
static inline Val val_null(void) { Val v = val_void(); v.kind = K_NULL; return v; }
static inline Val val_str(Sp s) { Val v = val_void(); v.kind = K_STR; v.s = s; return v; }
static inline Val val_int(int64_t n) { Val v = val_void(); v.kind = K_INT; v.n = n; return v; }
static inline Val val_bool(bool b) { Val v = val_void(); v.kind = K_BOOL; v.b = b; return v; }
static inline Val val_arr(uint8_t kind, uint32_t idx) { Val v = val_void(); v.kind = K_ARR; v.akind = kind; v.idx = idx; return v; }
static inline Val val_obj(uint8_t kind, uint32_t idx) { Val v = val_void(); v.kind = K_OBJ; v.akind = kind; v.idx = idx; return v; }
static inline Val val_loop(LoopInfo lp) { Val v = val_void(); v.kind = K_LOOP; v.lp = lp; return v; }

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
    bool is_post; Sp layout; Sp body;
} Page;

typedef struct { Sp name; Sp slug; Sp url; Arr posts; /* u32 */ } Tag;
typedef struct { Sp name; Sp slug; Sp url; } TagRef;
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
    bool is_loop;       /* REF_SCOPE */
    Sp name;            /* REF_SCOPE / REF_DYN */
    Arr chain;          /* uint8_t FId */
} Ref;

enum NodeKind { ND_TEXT, ND_VAR, ND_IF, ND_FOR, ND_INCLUDE };
typedef struct Node Node;
struct Node {
    uint8_t kind;       /* NodeKind */
    Sp text;            /* ND_TEXT */
    Ref ref;            /* ND_VAR / ND_IF */
    bool raw;           /* ND_VAR: {{{ }}} */
    bool neg;           /* ND_IF: "not" */
    Arr then_nodes;     /* ND_IF */
    Arr else_nodes;     /* ND_IF (len 0 when absent) */
    bool has_else;      /* ND_IF */
    Sp binder;          /* ND_FOR */
    Arr body;           /* ND_FOR */
    Sp name;            /* ND_INCLUDE */
    int32_t tpl_idx;    /* ND_INCLUDE: template index resolved at compile
                         * time, -1 = missing */
};

typedef struct { Sp name; Arr nodes; /* Node */ Arr needs; /* TplNeed */ uint8_t cacheable; uint8_t storable; } Tpl;

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
void tpl_cache_open(Ctx *c, Pool *p);
Val tpl_eval(Ctx *c, Scope *scopes, size_t nscopes, Ref *ref);
void tpl_emit(Ctx *c, Arr *templates, Arr *nodes, Scope *scopes, size_t *nscopes,
              Pool *out, Str *scratch, uint32_t depth);

/* ---------------- markdown ---------------- */
typedef struct {
    Page *pages; size_t npages;
    SpPair *statics; size_t nstatics;
    Sp base; Sp dir; Sp src_path;
    Arr *broken; /* Sp, pool spans */
    Str *path_buf; /* reused scratch for resolve_path */
    Str *item_buf; /* reused scratch for the current list item's text */
    Str *item_scratch; /* reused scratch for accumulating a list item */
} LinkCtx;
bool md_build(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
              Str *scratch, Str *tmp, Str *text_tmp,
              Sp *html_out, Arr *toc_out, Sp *excerpt_out);

/* ---------------- build ---------------- */
bool case_build(Pool *p, CaseInput *c, Pool *out);
bool cases_build(Pool *p, const uint8_t *input, size_t n, Pool *out);

#endif
