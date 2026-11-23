/* core.c — freestanding runtime: raw syscalls, mmap/THP allocator, the
 * bump pool, generic arrays, byte strings, runtime-dispatched AVX2 / SSE2 scans.
 * No libc: every byte we need comes from a raw syscall. */
#include "zg.h"
#include <immintrin.h>

uint8_t g_has_avx2 = 0;
uint8_t g_has_ssse3 = 0;
uint8_t g_has_popcnt = 0;

void cpu_detect(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    uint32_t max_leaf = eax;
    if (max_leaf < 1) return;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (ecx & (1u << 9)) g_has_ssse3 = 1;
    if (ecx & (1u << 23)) g_has_popcnt = 1;
    bool osxsave = (ecx & (1u << 27)) != 0;
    bool avx = (ecx & (1u << 28)) != 0;
    if (osxsave && avx && max_leaf >= 7) {
        uint32_t xlo, xhi;
        __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
        if ((xlo & 6u) == 6u) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
            if ((ebx & (1u << 5)) && (ebx & (1u << 3))) {
                g_has_avx2 = 1;
            }
        }
    }
}

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

int os_fstat_reg_size(int fd, size_t *sz_out) {
    uint8_t st[144];
    long r = raw_call(5, fd, (long)st, 0, 0, 0, 0);
    if (r < 0) return -1;
    uint32_t mode;
    int64_t sz;
    __builtin_memcpy(&mode, st + 24, 4);
    __builtin_memcpy(&sz, st + 48, 8);
    if ((mode & 0170000u) != 0100000u || sz < 0) return -1;
    *sz_out = (size_t)sz;
    return 0;
}

void *os_mmap_ro(int fd, size_t len) {
    if (len == 0) return 0;
    /* PROT_READ=1, MAP_PRIVATE|MAP_POPULATE=0x8002 */
    long r = raw_call(9, 0, (long)len, 1, 0x8002, fd, 0);
    if (r < 0 && r > -4096) {
        r = raw_call(9, 0, (long)len, 1, 0x02, fd, 0);
        if (r < 0 && r > -4096) return 0;
    }
    return (void *)r;
}

long os_munmap(void *p, size_t len) {
    return raw_call(11, (long)p, (long)len, 0, 0, 0, 0);
}

void *os_map(long n) {
    long r = raw_call(9, 0, n, 3, 0x22, -1, 0);
    if (r < 0 && r > -4096) return 0;
    return (void *)r;
}

/* Map with 2MB alignment + MADV_HUGEPAGE hint for big arena pools. */
void *os_map_huge(size_t n) {
    size_t align = 1u << 21; /* 2 MB */
    size_t total = n + align;
    void *raw = os_map((long)total);
    if (!raw) return 0;
    uintptr_t addr = ((uintptr_t)raw + align - 1) & ~(align - 1);
    /* SYS_madvise = 28, MADV_HUGEPAGE = 14 (harmless if unsupported) */
    raw_call(28, (long)addr, (long)n, 14, 0, 0, 0);
    return (void *)addr;
}

void *os_alloc(size_t n) {
    size_t pages = (n + 4095) / 4096;
    return os_map((long)(pages * 4096));
}

/* ---------------- byte primitives (also satisfy gcc codegen) ---------------- */
__attribute__((target("avx2")))
static void copy_bytes_avx2(uint8_t *dd, const uint8_t *ss, size_t n) {
    while (n >= 64) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)ss);
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(ss + 32));
        _mm256_storeu_si256((__m256i *)dd, v0);
        _mm256_storeu_si256((__m256i *)(dd + 32), v1);
        dd += 64; ss += 64; n -= 64;
    }
    if (n >= 32) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)ss);
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(ss + n - 32));
        _mm256_storeu_si256((__m256i *)dd, v0);
        _mm256_storeu_si256((__m256i *)(dd + n - 32), v1);
        return;
    }
    if (n >= 16) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)ss);
        __m128i v1 = _mm_loadu_si128((const __m128i *)(ss + n - 16));
        _mm_storeu_si128((__m128i *)dd, v0);
        _mm_storeu_si128((__m128i *)(dd + n - 16), v1);
        return;
    }
    if (n >= 8) {
        uint64_t a, b;
        __builtin_memcpy(&a, ss, 8); __builtin_memcpy(&b, ss + n - 8, 8);
        __builtin_memcpy(dd, &a, 8); __builtin_memcpy(dd + n - 8, &b, 8);
        return;
    }
    if (n >= 4) {
        uint32_t a, b;
        __builtin_memcpy(&a, ss, 4); __builtin_memcpy(&b, ss + n - 4, 4);
        __builtin_memcpy(dd, &a, 4); __builtin_memcpy(dd + n - 4, &b, 4);
        return;
    }
    if (n >= 2) {
        uint16_t a, b;
        __builtin_memcpy(&a, ss, 2); __builtin_memcpy(&b, ss + n - 2, 2);
        __builtin_memcpy(dd, &a, 2); __builtin_memcpy(dd + n - 2, &b, 2);
        return;
    }
    if (n == 1) *dd = *ss;
}

void *copy_bytes(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (n >= 64 && g_has_avx2) {
        copy_bytes_avx2(dd, ss, n);
        return d;
    }
    if (n >= 32) {
        while (n >= 32) {
            __m128i v0 = _mm_loadu_si128((const __m128i *)ss);
            __m128i v1 = _mm_loadu_si128((const __m128i *)(ss + 16));
            _mm_storeu_si128((__m128i *)dd, v0);
            _mm_storeu_si128((__m128i *)(dd + 16), v1);
            dd += 32; ss += 32; n -= 32;
        }
    }
    if (n >= 16) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)ss);
        __m128i v1 = _mm_loadu_si128((const __m128i *)(ss + n - 16));
        _mm_storeu_si128((__m128i *)dd, v0);
        _mm_storeu_si128((__m128i *)(dd + n - 16), v1);
        return d;
    }
    if (n >= 8) {
        uint64_t a, b;
        __builtin_memcpy(&a, ss, 8); __builtin_memcpy(&b, ss + n - 8, 8);
        __builtin_memcpy(dd, &a, 8); __builtin_memcpy(dd + n - 8, &b, 8);
        return d;
    }
    if (n >= 4) {
        uint32_t a, b;
        __builtin_memcpy(&a, ss, 4); __builtin_memcpy(&b, ss + n - 4, 4);
        __builtin_memcpy(dd, &a, 4); __builtin_memcpy(dd + n - 4, &b, 4);
        return d;
    }
    if (n >= 2) {
        uint16_t a, b;
        __builtin_memcpy(&a, ss, 2); __builtin_memcpy(&b, ss + n - 2, 2);
        __builtin_memcpy(dd, &a, 2); __builtin_memcpy(dd + n - 2, &b, 2);
        return d;
    }
    if (n == 1) *dd = *ss;
    return d;
}

void *fill_bytes(void *d, int c, size_t n) {
    uint8_t *dd = d; uint8_t v = (uint8_t)c;
    if (n >= 16) {
        __m128i vv = _mm_set1_epi8((char)v);
        while (n >= 32) {
            _mm_storeu_si128((__m128i *)dd, vv);
            _mm_storeu_si128((__m128i *)(dd + 16), vv);
            dd += 32; n -= 32;
        }
        if (n >= 16) {
            _mm_storeu_si128((__m128i *)dd, vv);
            _mm_storeu_si128((__m128i *)(dd + n - 16), vv);
            return d;
        }
    }
    uint64_t w = (uint64_t)v * 0x0101010101010101ull;
    while (n >= 8) { __builtin_memcpy(dd, &w, 8); dd += 8; n -= 8; }
    while (n--) *dd++ = v;
    return d;
}

int cmp_bytes(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n >= 16) {
        __m128i vx = _mm_loadu_si128((const __m128i *)x);
        __m128i vy = _mm_loadu_si128((const __m128i *)y);
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(vx, vy)) ^ 0xffffu;
        if (m) {
            int idx = __builtin_ctz(m);
            return (int)x[idx] - (int)y[idx];
        }
        x += 16; y += 16; n -= 16;
    }
    while (n >= 8) {
        uint64_t u, v;
        __builtin_memcpy(&u, x, 8); __builtin_memcpy(&v, y, 8);
        if (u != v) {
            int idx = __builtin_ctzll(u ^ v) >> 3;
            return (int)x[idx] - (int)y[idx];
        }
        x += 8; y += 8; n -= 8;
    }
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

__attribute__((used)) void *memcpy(void *d, const void *s, size_t n) { return copy_bytes(d, s, n); }
__attribute__((used)) void *memset(void *d, int c, size_t n) { return fill_bytes(d, c, n); }
__attribute__((used)) void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd < ss) return copy_bytes(d, s, n);
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}
__attribute__((used)) int memcmp(const void *a, const void *b, size_t n) { return cmp_bytes(a, b, n); }
__attribute__((used)) size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------------- bump pool ---------------- */
static void write_all_fd(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        long w = os_write(fd, p, (long)n);
        if (w <= 0) die(1);
        p += w;
        n -= (size_t)w;
    }
}

void pool_open(Pool *p, size_t cap) {
    p->base = (cap >= (1u << 21)) ? os_map_huge(cap) : os_map((long)cap);
    if (!p->base) p->base = os_map((long)cap);
    p->len = 0;
    p->cap = cap;
    p->fd = -1;
}

void pool_open_stream(Pool *p, size_t cap, int fd) {
    p->base = os_map((long)cap);
    p->len = 0;
    p->cap = cap;
    p->fd = fd;
}

void out_flush(Pool *p) {
    if (p->fd < 0) die(1);
    if (p->len > 0) {
        write_all_fd(p->fd, p->base, p->len);
        p->len = 0;
    }
}

void pool_put_slow(Pool *p, const uint8_t *src, size_t n) {
    if (p->fd < 0) die(1);
    if (p->len > 0) {
        write_all_fd(p->fd, p->base, p->len);
        p->len = 0;
    }
    if (n <= p->cap) {
        copy_bytes(p->base, src, n);
        p->len = n;
    } else {
        write_all_fd(p->fd, src, n);
    }
}

void pool_u64(Pool *p, uint64_t v) {
    if (__builtin_expect(p->len + 24 > p->cap, 0)) out_flush(p);
    uint8_t tmp[20]; int i = 20;
    if (v == 0) { p->base[p->len++] = '0'; return; }
    while (v > 0) { tmp[--i] = (uint8_t)('0' + v % 10); v /= 10; }
    copy_fast(p->base + p->len, tmp + i, (size_t)(20 - i));
    p->len += (size_t)(20 - i);
}

void pool_i64(Pool *p, int64_t n) {
    if (__builtin_expect(p->len + 24 > p->cap, 0)) out_flush(p);
    if (n == 0) { p->base[p->len++] = '0'; return; }
    if (n < 0) {
        p->base[p->len++] = '-';
        pool_u64(p, (uint64_t)(-(n + 1)) + 1);
        return;
    }
    pool_u64(p, (uint64_t)n);
}

/* ---------------- generic array ---------------- */
void arr_grow(Arr *a, Pool *p, size_t n) {
    if (n <= a->cap) return;
    size_t ncap = a->cap ? a->cap * 2 : 16;
    while (ncap < n) ncap *= 2;
    void *np = pool_alloc(p, ncap * a->esz);
    if (a->items && a->len) copy_bytes(np, a->items, a->len * a->esz);
    a->items = np;
    a->cap = ncap;
}

/* Stable mergesort over an array's elements. */
static void sort_merge(uint8_t *a, uint8_t *tmp, size_t esz, size_t lo, size_t mid, size_t hi,
                       void *ctx, int (*cmp)(void *, const void *, const void *)) {
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (cmp(ctx, a + i * esz, a + j * esz) <= 0) { copy_fast(tmp + k * esz, a + i * esz, esz); i++; }
        else { copy_fast(tmp + k * esz, a + j * esz, esz); j++; }
        k++;
    }
    if (i < mid) copy_bytes(tmp + k * esz, a + i * esz, (mid - i) * esz);
    if (j < hi) copy_bytes(tmp + k * esz, a + j * esz, (hi - j) * esz);
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
void str_grow(Str *s, size_t n) {
    if (n <= s->cap) return;
    size_t ncap = s->cap ? s->cap * 2 : 4096;
    while (ncap < n) ncap *= 2;
    uint8_t *np = os_alloc(ncap);
    if (s->data && s->len) copy_bytes(np, s->data, s->len);
    s->data = np;
    s->cap = ncap;
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

/* ---------------- SIMD scans (AVX2 + SSE2 fallback) ---------------- */

static const uint8_t TBL_INLINE[256] = {
    ['\\'] = 1, ['`'] = 1, ['['] = 1, ['*'] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};
static const uint8_t TBL_INLINE_JSON[256] = {
    [0 ... 0x1f] = 1,
    ['\\'] = 1, ['`'] = 1, ['['] = 1, ['*'] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};
static const uint8_t TBL_HTML[256] = {
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1
};
static const uint8_t TBL_HTML_JSON[256] = {
    [0 ... 0x1f] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1, ['\\'] = 1
};
static const uint8_t TBL_JSON[256] = {
    [0 ... 0x1f] = 1, ['"'] = 1, ['\\'] = 1
};

__attribute__((target("avx2,bmi")))
static int scan_byte_avx2(const uint8_t *b, size_t n, size_t i, uint8_t c) {
    __m256i vc = _mm256_set1_epi8((char)c);
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vc));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm256_castsi256_si128(vc)));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c) return (int)i; i++; }
    return -1;
}

int scan_byte(const uint8_t *b, size_t n, size_t from, uint8_t c) {
    if (g_has_avx2 && n - from >= 32) return scan_byte_avx2(b, n, from, c);
    size_t i = from;
    __m128i vc = _mm_set1_epi8((char)c);
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c) return (int)i; i++; }
    return -1;
}

int scan_nl(const uint8_t *b, size_t n, size_t from) {
    return scan_byte(b, n, from, 10);
}

__attribute__((target("avx2,bmi")))
static int scan_two_avx2(const uint8_t *b, size_t n, size_t i, uint8_t c1, uint8_t c2) {
    __m256i v1 = _mm256_set1_epi8((char)c1);
    __m256i v2 = _mm256_set1_epi8((char)c2);
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i hit = _mm256_or_si256(_mm256_cmpeq_epi8(v, v1), _mm256_cmpeq_epi8(v, v2));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_or_si128(_mm_cmpeq_epi8(v, _mm256_castsi256_si128(v1)),
                                   _mm_cmpeq_epi8(v, _mm256_castsi256_si128(v2)));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c1 || b[i] == c2) return (int)i; i++; }
    return -1;
}

int scan_two(const uint8_t *b, size_t n, size_t from, uint8_t c1, uint8_t c2) {
    if (g_has_avx2 && n - from >= 32) return scan_two_avx2(b, n, from, c1, c2);
    size_t i = from;
    __m128i v1 = _mm_set1_epi8((char)c1);
    __m128i v2 = _mm_set1_epi8((char)c2);
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_or_si128(_mm_cmpeq_epi8(v, v1), _mm_cmpeq_epi8(v, v2));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (b[i] == c1 || b[i] == c2) return (int)i; i++; }
    return -1;
}

int scan_any(const uint8_t *b, size_t n, size_t from, const uint8_t *needles, size_t nneedles) {
    if (nneedles == 1) return scan_byte(b, n, from, needles[0]);
    if (nneedles == 2) return scan_two(b, n, from, needles[0], needles[1]);
    __m128i vec[16];
    for (size_t k = 0; k < nneedles; k++) vec[k] = _mm_set1_epi8((char)needles[k]);
    size_t i = from;
    while (i + 16 <= n) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_setzero_si128();
        for (size_t k = 0; k < nneedles; k++) hit = _mm_or_si128(hit, _mm_cmpeq_epi8(chunk, vec[k]));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) {
        for (size_t k = 0; k < nneedles; k++) if (b[i] == needles[k]) return (int)i;
        i++;
    }
    return -1;
}

__attribute__((target("avx2,bmi")))
static int scan_json_esc_avx2(const uint8_t *b, size_t n, size_t i) {
    __m256i q = _mm256_set1_epi8('"');
    __m256i bs = _mm256_set1_epi8('\\');
    __m256i c1f = _mm256_set1_epi8(0x1f);
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i hit = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs)),
            _mm256_cmpeq_epi8(_mm256_min_epu8(v, c1f), v));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, _mm256_castsi256_si128(q)),
                         _mm_cmpeq_epi8(v, _mm256_castsi256_si128(bs))),
            _mm_cmpeq_epi8(_mm_min_epu8(v, _mm256_castsi256_si128(c1f)), v));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_JSON[b[i]]) return (int)i; i++; }
    return -1;
}

int scan_json_esc(const uint8_t *b, size_t n, size_t from) {
    if (g_has_avx2 && n - from >= 32) return scan_json_esc_avx2(b, n, from);
    size_t i = from;
    __m128i q = _mm_set1_epi8('"');
    __m128i bs = _mm_set1_epi8('\\');
    __m128i c1f = _mm_set1_epi8(0x1f);
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
            _mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_JSON[b[i]]) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static int scan_html_esc_avx2(const uint8_t *b, size_t n, size_t i) {
    /* Needles: " (22), & (26), ' (27), < (3C), > (3E) */
    __m256i lo_lut = _mm256_setr_epi8(
        0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 2, 0, 2, 0,
        0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 2, 0, 2, 0);
    __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        __m256i hit = _mm256_and_si256(lo, hi);
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    while (i < n) { if (TBL_HTML[b[i]]) return (int)i; i++; }
    return -1;
}

int scan_html_esc(const uint8_t *b, size_t n, size_t from) {
    if (g_has_avx2 && n - from >= 32) return scan_html_esc_avx2(b, n, from);
    size_t i = from;
    __m128i v0 = _mm_set1_epi8('&'), v1 = _mm_set1_epi8('<'), v2 = _mm_set1_epi8('>'),
            v3 = _mm_set1_epi8('"'), v4 = _mm_set1_epi8('\'');
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, v0), _mm_cmpeq_epi8(v, v1)),
            _mm_or_si128(_mm_cmpeq_epi8(v, v2),
                         _mm_or_si128(_mm_cmpeq_epi8(v, v3), _mm_cmpeq_epi8(v, v4))));
        uint32_t m = (uint32_t)_mm_movemask_epi8(hit);
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_HTML[b[i]]) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static int scan_inline_md_avx2(const uint8_t *b, size_t n, size_t i) {
    /* Needles: " (22), & (26), ' (27), * (2A), < (3C), > (3E), [ (5B), \ (5C), ` (60) */
    __m256i lo_lut = _mm256_setr_epi8(
        8, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 4, 6, 0, 2, 0,
        8, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 4, 6, 0, 2, 0);
    __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        __m256i hit = _mm256_and_si256(lo, hi);
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
        __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                      _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
        __m128i hit = _mm_and_si128(lo, hi);
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu;
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_INLINE[b[i]]) return (int)i; i++; }
    return -1;
}

int scan_inline_md(const uint8_t *b, size_t n, size_t from) {
    if (g_has_avx2 && n - from >= 16) return scan_inline_md_avx2(b, n, from);
    size_t i = from;
    __m128i v0 = _mm_set1_epi8('\\'), v1 = _mm_set1_epi8('`'), v2 = _mm_set1_epi8('['),
            v3 = _mm_set1_epi8('*'), v4 = _mm_set1_epi8('&'), v5 = _mm_set1_epi8('<'),
            v6 = _mm_set1_epi8('>'), v7 = _mm_set1_epi8('"'), v8 = _mm_set1_epi8('\'');
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i h1 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, v0), _mm_cmpeq_epi8(v, v1)),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v2), _mm_cmpeq_epi8(v, v3)));
        __m128i h2 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, v4), _mm_cmpeq_epi8(v, v5)),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v6),
                                               _mm_or_si128(_mm_cmpeq_epi8(v, v7), _mm_cmpeq_epi8(v, v8))));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h1, h2));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_INLINE[b[i]]) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static int scan_inline_md_json_avx2(const uint8_t *b, size_t n, size_t i) {
    /* Needles: 0x00..0x1f (hi=0,1 -> 1), " (22), & (26), ' (27), * (2A) (hi=2 -> 2),
     * < (3C), > (3E) (hi=3 -> 4), [ (5B), \ (5C) (hi=5 -> 8), ` (60) (hi=6 -> 16) */
    __m256i lo_lut = _mm256_setr_epi8(
        17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1,
        17, 1, 3, 1, 1, 1, 3, 3, 1, 1, 3, 9, 13, 1, 5, 1);
    __m256i hi_lut = _mm256_setr_epi8(
        1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 4, 0, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        __m256i hit = _mm256_and_si256(lo, hi);
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
        __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                      _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
        __m128i hit = _mm_and_si128(lo, hi);
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu;
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_INLINE_JSON[b[i]]) return (int)i; i++; }
    return -1;
}

int scan_inline_md_json(const uint8_t *b, size_t n, size_t from) {
    if (g_has_avx2 && n - from >= 16) return scan_inline_md_json_avx2(b, n, from);
    size_t i = from;
    __m128i c1f = _mm_set1_epi8(0x1f);
    __m128i v0 = _mm_set1_epi8('\\'), v1 = _mm_set1_epi8('`'), v2 = _mm_set1_epi8('['),
            v3 = _mm_set1_epi8('*'), v4 = _mm_set1_epi8('&'), v5 = _mm_set1_epi8('<'),
            v6 = _mm_set1_epi8('>'), v7 = _mm_set1_epi8('"'), v8 = _mm_set1_epi8('\'');
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i h0 = _mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v);
        __m128i h1 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, v0), _mm_cmpeq_epi8(v, v1)),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v2), _mm_cmpeq_epi8(v, v3)));
        __m128i h2 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, v4), _mm_cmpeq_epi8(v, v5)),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v6),
                                               _mm_or_si128(_mm_cmpeq_epi8(v, v7), _mm_cmpeq_epi8(v, v8))));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h0, _mm_or_si128(h1, h2)));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_INLINE_JSON[b[i]]) return (int)i; i++; }
    return -1;
}

__attribute__((target("avx2,bmi")))
static int scan_html_json_esc_avx2(const uint8_t *b, size_t n, size_t i) {
    /* Needles: 0x00..0x1f (hi=0,1 -> 8), " (22), & (26), ' (27) (hi=2 -> 1), < (3C), > (3E) (hi=3 -> 2), \ (5C) (hi=5 -> 4) */
    __m256i lo_lut = _mm256_setr_epi8(
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8,
        8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
    __m256i hi_lut = _mm256_setr_epi8(
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
        __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
        __m256i hit = _mm256_and_si256(lo, hi);
        uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
        __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                      _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
        __m128i hit = _mm_and_si128(lo, hi);
        uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu;
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_HTML_JSON[b[i]]) return (int)i; i++; }
    return -1;
}

int scan_html_json_esc(const uint8_t *b, size_t n, size_t from) {
    if (g_has_avx2 && n - from >= 16) return scan_html_json_esc_avx2(b, n, from);
    size_t i = from;
    __m128i c1f = _mm_set1_epi8(0x1f);
    __m128i v0 = _mm_set1_epi8('&'), v1 = _mm_set1_epi8('<'), v2 = _mm_set1_epi8('>'),
            v3 = _mm_set1_epi8('"'), v4 = _mm_set1_epi8('\''), v5 = _mm_set1_epi8('\\');
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i h1 = _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v0), _mm_cmpeq_epi8(v, v1)));
        __m128i h2 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, v2), _mm_cmpeq_epi8(v, v3)),
                                  _mm_or_si128(_mm_cmpeq_epi8(v, v4), _mm_cmpeq_epi8(v, v5)));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h1, h2));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
    while (i < n) { if (TBL_HTML_JSON[b[i]]) return (int)i; i++; }
    return -1;
}
