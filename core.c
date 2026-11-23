/* core.c — freestanding runtime: raw syscalls, mmap allocator, the
 * bump pool, generic arrays, byte strings, SSE2 scans.
 * No libc: every byte we need comes from a raw syscall. */
#include "zg.h"

/* ---------------- raw syscalls ---------------- */
long raw_call(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

void die(int code) { raw_call(60, code, 0, 0, 0, 0, 0); __builtin_unreachable(); }
long os_open(const char *p, int flags, int mode) { return raw_call(2, (long)p, flags, mode, 0, 0, 0); }
long os_read(int fd, void *b, long n) { return raw_call(0, fd, (long)b, n, 0, 0, 0); }
long os_write(int fd, const void *b, long n) { return raw_call(1, fd, (long)b, n, 0, 0, 0); }
long os_close(int fd) { return raw_call(3, fd, 0, 0, 0, 0, 0); }

void *os_map(long n) {
    long r = raw_call(9, 0, n, 3, 0x22, -1, 0);
    if (r < 0 && r > -4096) return 0;
    return (void *)r;
}

void *os_alloc(size_t n) {
    size_t pages = (n + 4095) / 4096;
    return os_map((long)(pages * 4096));
}

/* ---------------- byte primitives (also satisfy gcc codegen) ---------------- */
void *copy_bytes(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n >= 8) {
        *(uint64_t *)dd = *(const uint64_t *)ss; dd += 8; ss += 8; n -= 8;
    }
    while (n--) *dd++ = *ss++;
    return d;
}

void *fill_bytes(void *d, int c, size_t n) {
    uint8_t *dd = d; uint8_t v = (uint8_t)c;
    uint64_t w = (uint64_t)v * 0x0101010101010101ull;
    while (n >= 8) { *(uint64_t *)dd = w; dd += 8; n -= 8; }
    while (n--) *dd++ = v;
    return d;
}

int cmp_bytes(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

void *memcpy(void *d, const void *s, size_t n) { return copy_bytes(d, s, n); }
void *memset(void *d, int c, size_t n) { return fill_bytes(d, c, n); }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd < ss) return copy_bytes(d, s, n);
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) { return cmp_bytes(a, b, n); }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------------- bump pool ---------------- */
void pool_open(Pool *p, size_t cap) {
    p->base = os_map((long)cap);
    p->len = 0;
    p->cap = cap;
}

void pool_put(Pool *p, const uint8_t *src, size_t n) {
    copy_bytes(p->base + p->len, src, n);
    p->len += n;
}

void pool_copy(Pool *p, Sp s) {
    /* Source ends at or before the tail (spans were created earlier),
     * so the source region lies entirely before the destination. */
    copy_bytes(p->base + p->len, p->base + s.off, s.len);
    p->len += s.len;
}

void pool_u64(Pool *p, uint64_t v) {
    uint8_t tmp[20]; int i = 20;
    if (v == 0) { p->base[p->len++] = '0'; return; }
    while (v > 0) { tmp[--i] = (uint8_t)('0' + v % 10); v /= 10; }
    copy_bytes(p->base + p->len, tmp + i, (size_t)(20 - i));
    p->len += (size_t)(20 - i);
}

void pool_i64(Pool *p, int64_t n) {
    if (n == 0) { p->base[p->len++] = '0'; return; }
    if (n < 0) {
        p->base[p->len++] = '-';
        pool_u64(p, (uint64_t)(-(n + 1)) + 1);
        return;
    }
    pool_u64(p, (uint64_t)n);
}

Sp pool_store(Pool *p, const uint8_t *src, size_t n) {
    uint32_t off = (uint32_t)p->len;
    copy_bytes(p->base + p->len, src, n);
    p->len += n;
    return (Sp){off, (uint32_t)n};
}

/* ---------------- generic array ---------------- */
void arr_new(Arr *a, size_t esz) { a->items = 0; a->len = 0; a->cap = 0; a->esz = esz; }

void arr_grow(Arr *a, Pool *p, size_t n) {
    if (n <= a->cap) return;
    size_t ncap = a->cap ? a->cap * 2 : 16;
    while (ncap < n) ncap *= 2;
    void *np = pool_alloc(p, ncap * a->esz);
    if (a->items && a->len) copy_bytes(np, a->items, a->len * a->esz);
    a->items = np;
    a->cap = ncap;
}

void *arr_slot(Arr *a, Pool *p) {
    arr_grow(a, p, a->len + 1);
    void *slot = (uint8_t *)a->items + a->len * a->esz;
    fill_bytes(slot, 0, a->esz);
    a->len++;
    return slot;
}

void *arr_at(Arr *a, size_t i) { return (uint8_t *)a->items + i * a->esz; }

/* Stable mergesort over an array's elements. */
static void sort_merge(uint8_t *a, uint8_t *tmp, size_t esz, size_t lo, size_t mid, size_t hi,
                       void *ctx, int (*cmp)(void *, const void *, const void *)) {
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (cmp(ctx, a + i * esz, a + j * esz) <= 0) { copy_bytes(tmp + k * esz, a + i * esz, esz); i++; }
        else { copy_bytes(tmp + k * esz, a + j * esz, esz); j++; }
        k++;
    }
    while (i < mid) { copy_bytes(tmp + k * esz, a + i * esz, esz); i++; k++; }
    while (j < hi) { copy_bytes(tmp + k * esz, a + j * esz, esz); j++; k++; }
    copy_bytes(a + lo * esz, tmp + lo * esz, (hi - lo) * esz);
}

static void sort_rec(uint8_t *a, uint8_t *tmp, size_t esz, size_t lo, size_t hi,
                     void *ctx, int (*cmp)(void *, const void *, const void *)) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    sort_rec(a, tmp, esz, lo, mid, ctx, cmp);
    sort_rec(a, tmp, esz, mid, hi, ctx, cmp);
    sort_merge(a, tmp, esz, lo, mid, hi, ctx, cmp);
}

void arr_sort(Arr *a, Pool *p, void *ctx, int (*cmp)(void *ctx, const void *x, const void *y)) {
    if (a->len < 2) return;
    uint8_t *tmp = pool_alloc(p, a->len * a->esz);
    sort_rec(a->items, tmp, a->esz, 0, a->len, ctx, cmp);
}

/* ---------------- byte string (mmap-backed) ---------------- */
void str_new(Str *s) { s->data = 0; s->len = 0; s->cap = 0; }

void str_grow(Str *s, size_t n) {
    if (n <= s->cap) return;
    size_t ncap = s->cap ? s->cap * 2 : 512;
    while (ncap < n) ncap *= 2;
    uint8_t *np = os_alloc(ncap);
    if (s->data && s->len) copy_bytes(np, s->data, s->len);
    s->data = np;
    s->cap = ncap;
}

void str_c(Str *s, uint8_t c) {
    str_grow(s, s->len + 1);
    s->data[s->len++] = c;
}

void str_add(Str *s, const uint8_t *p, size_t n) {
    str_grow(s, s->len + n);
    copy_bytes(s->data + s->len, p, n);
    s->len += n;
}

void str_cstr(Str *s, const char *t) {
    size_t n = 0; while (t[n]) n++;
    str_add(s, (const uint8_t *)t, n);
}

void str_i64(Str *s, int64_t n) {
    if (n == 0) { str_c(s, '0'); return; }
    if (n < 0) {
        str_c(s, '-');
        uint64_t v = (uint64_t)(-(n + 1)) + 1;
        uint8_t tmp[20]; int i = 20;
        while (v > 0) { tmp[--i] = (uint8_t)('0' + v % 10); v /= 10; }
        str_add(s, tmp + i, (size_t)(20 - i));
        return;
    }
    uint64_t v = (uint64_t)n;
    uint8_t tmp[20]; int i = 20;
    while (v > 0) { tmp[--i] = (uint8_t)('0' + v % 10); v /= 10; }
    str_add(s, tmp + i, (size_t)(20 - i));
}

/* ---------------- SSE2 scans ---------------- */
static inline unsigned mask16(__m128i v) { return (unsigned)_mm_movemask_epi8(v); }

static inline int first_bit(unsigned m, size_t base) {
    int off = 0;
    while ((m & 1) == 0) { m >>= 1; off++; }
    return (int)(base + (size_t)off);
}

int scan_any(const uint8_t *b, size_t n, size_t from, const uint8_t *needles, size_t nneedles) {
    /* Broadcast the needle vectors ONCE: gcc does not hoist the
     * broadcast out of the chunk loop (the needles may alias input). */
    __m128i vec[16];
    for (size_t k = 0; k < nneedles; k++) vec[k] = _mm_set1_epi8((char)needles[k]);
    size_t i = from;
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(b + i));
        unsigned m = 0;
        for (size_t k = 0; k < nneedles; k++) m |= mask16(_mm_cmpeq_epi8(chunk, vec[k]));
        if (m) return first_bit(m, i);
        i += 16;
    }
    while (i < n) {
        for (size_t k = 0; k < nneedles; k++) if (b[i] == needles[k]) return (int)i;
        i++;
    }
    return -1;
}

int scan_json_esc(const uint8_t *b, size_t n, size_t from) {
    size_t i = from;
    __m128i q = _mm_set1_epi8('"');
    __m128i bs = _mm_set1_epi8('\\');
    __m128i x80 = _mm_set1_epi8((char)0x80);
    __m128i lt = _mm_set1_epi8((char)0xa0); /* (0x20 ^ 0x80) as signed */
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(b + i));
        unsigned m = mask16(_mm_cmpeq_epi8(chunk, q)) | mask16(_mm_cmpeq_epi8(chunk, bs))
            | mask16(_mm_cmplt_epi8(_mm_xor_si128(chunk, x80), lt));
        if (m) return first_bit(m, i);
        i += 16;
    }
    while (i < n) {
        uint8_t c = b[i];
        if (c == '"' || c == '\\' || c < 0x20) return (int)i;
        i++;
    }
    return -1;
}

int scan_html_esc(const uint8_t *b, size_t n, size_t from) {
    static const uint8_t needles[5] = {'&', '<', '>', '"', '\''};
    return scan_any(b, n, from, needles, 5);
}

int scan_nl(const uint8_t *b, size_t n, size_t from) {
    size_t i = from;
    __m128i nl = _mm_set1_epi8(10);
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(b + i));
        unsigned m = mask16(_mm_cmpeq_epi8(chunk, nl));
        if (m) return first_bit(m, i);
        i += 16;
    }
    while (i < n) { if (b[i] == 10) return (int)i; i++; }
    return -1;
}

int scan_html_json_esc(const uint8_t *b, size_t n, size_t from) {
    size_t i = from;
    __m128i x80 = _mm_set1_epi8((char)0x80);
    __m128i lt = _mm_set1_epi8((char)0xa0);
    static const uint8_t needles[6] = {'&', '<', '>', '"', '\'', '\\'};
    __m128i vec[6];
    for (size_t k = 0; k < 6; k++) vec[k] = _mm_set1_epi8((char)needles[k]);
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(b + i));
        unsigned m = mask16(_mm_cmplt_epi8(_mm_xor_si128(chunk, x80), lt));
        for (size_t k = 0; k < 6; k++) m |= mask16(_mm_cmpeq_epi8(chunk, vec[k]));
        if (m) return first_bit(m, i);
        i += 16;
    }
    while (i < n) {
        uint8_t c = b[i];
        if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' || c == '\\' || c < 0x20) return (int)i;
        i++;
    }
    return -1;
}
