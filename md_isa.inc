/* md_isa.inc — ISA-specialized flat zero-recursion SIMD Markdown engine.
 * Included three times by md.c with:
 *   MD_SUF      : avx2 / ssse3 / sse2
 *   MD_ATTR     : __attribute__((target("avx2,bmi"))) / target("ssse3") / empty
 *   MD_IS_AVX2  : 1 or 0
 *   MD_IS_SSSE3 : 1 or 0
 * Features:
 * - Flat 1-pass speculative SIMD emphasis (** / *) with depth-2 stack:
 *   eliminates scalar bold/em loops and nested *...* inside **...** recursions.
 * - Masked 16B SIMD tail processing (eliminates 130 KB scalar tail loop).
 * - Fast SIMD link label & target scanning + zero-copy relative path resolution.
 * - Fast blank-line & paragraph continuation scanning + backtick-targeted fence close. */

#define MD_CAT2(a, b) a##b
#define MD_CAT(a, b) MD_CAT2(a, b)
#define MD_FN(name) MD_CAT(name##_, MD_SUF)

MD_ATTR
static inline int MD_FN(scan_byte_i)(const uint8_t *b, size_t end, size_t i, uint8_t ch) {
#if MD_IS_AVX2
    __m256i vc = _mm256_set1_epi8((char)ch);
    while (i + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vc));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 32;
    }
    if (i + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm256_castsi256_si128(vc)));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
#else
    __m128i vc = _mm_set1_epi8((char)ch);
    while (i + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
        if (m) return (int)(i + (size_t)__builtin_ctz(m));
        i += 16;
    }
#endif
    while (i < end) { if (b[i] == ch) return (int)i; i++; }
    return -1;
}

MD_ATTR
static inline int MD_FN(scan_nl_i)(const uint8_t *b, size_t end, size_t i) {
    return MD_FN(scan_byte_i)(b, end, i, '\n');
}

MD_ATTR
static inline int MD_FN(scan_two_i)(const uint8_t *b, size_t n, size_t i, uint8_t c1, uint8_t c2) {
#if MD_IS_AVX2
    if (i + 32 <= n) {
        __m256i v1 = _mm256_set1_epi8((char)c1);
        __m256i v2 = _mm256_set1_epi8((char)c2);
        do {
            __m256i v = _mm256_loadu_si256((const __m256i *)(b + i));
            uint32_t m = (uint32_t)_mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, v1), _mm256_cmpeq_epi8(v, v2)));
            if (m) return (int)(i + (size_t)__builtin_ctz(m));
            i += 32;
        } while (i + 32 <= n);
    }
#endif
    if (i + 16 <= n) {
        __m128i v1 = _mm_set1_epi8((char)c1);
        __m128i v2 = _mm_set1_epi8((char)c2);
        do {
            __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
            uint32_t m = (uint32_t)_mm_movemask_epi8(
                _mm_or_si128(_mm_cmpeq_epi8(v, v1), _mm_cmpeq_epi8(v, v2)));
            if (m) return (int)(i + (size_t)__builtin_ctz(m));
            i += 16;
        } while (i + 16 <= n);
    }
    while (i < n) { if (b[i] == c1 || b[i] == c2) return (int)i; i++; }
    return -1;
}

MD_ATTR
static inline int MD_FN(find_unesc_i)(const uint8_t *s, size_t n, uint8_t w0, uint8_t w1, size_t wlen, size_t from) {
    size_t pos = from;
    for (;;) {
        int j = MD_FN(scan_two_i)(s, n, pos, '\\', w0);
        if (j < 0) return -1;
        if (s[j] == '\\') {
            if ((size_t)j + 1 < n && is_escapable(s[j + 1])) pos = (size_t)j + 2;
            else pos = (size_t)j + 1;
            continue;
        }
        if (wlen == 1) return j;
        if ((size_t)j + 1 < n && s[j + 1] == w1) return j;
        pos = (size_t)j + 1;
    }
}

MD_ATTR
static inline int MD_FN(find_lone_star_i)(const uint8_t *s, size_t n, size_t from) {
    size_t pos = from;
    for (;;) {
        int j = MD_FN(scan_two_i)(s, n, pos, '\\', '*');
        if (j < 0) return -1;
        if (s[j] == '\\') {
            if ((size_t)j + 1 < n && is_escapable(s[j + 1])) pos = (size_t)j + 2;
            else pos = (size_t)j + 1;
            continue;
        }
        if ((size_t)j + 1 < n && s[j + 1] == '*') { pos = (size_t)j + 2; continue; }
        return j;
    }
}

/* Branchless HTML+JSON escape into dst (b has >=32B readable slack, dst has >=64B writable slack). */
MD_ATTR
static uint8_t *MD_FN(esc_html_json_fast)(const uint8_t *b, size_t n, uint8_t *dst) {
    size_t i = 0;
#if MD_IS_AVX2
    if (n >= 16) {
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
            uint32_t m = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), zero));
            _mm256_storeu_si256((__m256i *)dst, v);
            if (__builtin_expect(m == 0, 1)) { dst += 32; i += 32; continue; }
            size_t pos = (size_t)__builtin_ctz(m);
            dst += pos;
            for (;;) {
                uint8_t c = b[i + pos];
                __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                dst += ESC_HTML_LEN[c];
                pos++;
                m &= m - 1;
                if (!m) break;
                size_t nb = (size_t)__builtin_ctz(m);
                __m256i g = _mm256_loadu_si256((const __m256i *)(b + i + pos));
                _mm256_storeu_si256((__m256i *)dst, g);
                dst += (nb - pos);
                pos = nb;
            }
            __m256i rem = _mm256_loadu_si256((const __m256i *)(b + i + pos));
            _mm256_storeu_si256((__m256i *)dst, rem);
            dst += (32 - pos);
            i += 32;
        }
        if (i + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
            __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
            __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                          _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
            uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), _mm_setzero_si128()))) ^ 0xffffu;
            _mm_storeu_si128((__m128i *)dst, v);
            if (m == 0) { dst += 16; i += 16; }
            else {
                size_t pos = (size_t)__builtin_ctz(m);
                dst += pos;
                for (;;) {
                    uint8_t c = b[i + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m &= m - 1;
                    if (!m) break;
                    size_t nb = (size_t)__builtin_ctz(m);
                    __m128i g = _mm_loadu_si128((const __m128i *)(b + i + pos));
                    _mm_storeu_si128((__m128i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m128i rem = _mm_loadu_si128((const __m128i *)(b + i + pos));
                _mm_storeu_si128((__m128i *)dst, rem);
                dst += (16 - pos);
                i += 16;
            }
        }
    }
#elif MD_IS_SSSE3
    if (n >= 16) {
        __m128i lo_lut = _mm_setr_epi8(8, 8, 9, 8, 8, 8, 9, 9, 8, 8, 8, 8, 14, 8, 10, 8);
        __m128i hi_lut = _mm_setr_epi8(8, 8, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        __m128i mask0f = _mm_set1_epi8(0x0f);
        __m128i zero = _mm_setzero_si128();
        while (i + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
            __m128i lo = _mm_shuffle_epi8(lo_lut, v);
            __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
            uint32_t m = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(lo, hi), zero))) ^ 0xffffu;
            _mm_storeu_si128((__m128i *)dst, v);
            if (__builtin_expect(m == 0, 1)) { dst += 16; i += 16; continue; }
            size_t pos = (size_t)__builtin_ctz(m);
            dst += pos;
            for (;;) {
                uint8_t c = b[i + pos];
                __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                dst += ESC_HTML_LEN[c];
                pos++;
                m &= m - 1;
                if (!m) break;
                size_t nb = (size_t)__builtin_ctz(m);
                __m128i g = _mm_loadu_si128((const __m128i *)(b + i + pos));
                _mm_storeu_si128((__m128i *)dst, g);
                dst += (nb - pos);
                pos = nb;
            }
            __m128i rem = _mm_loadu_si128((const __m128i *)(b + i + pos));
            _mm_storeu_si128((__m128i *)dst, rem);
            dst += (16 - pos);
            i += 16;
        }
    }
#else
    if (n >= 16) {
        __m128i c1f = _mm_set1_epi8(0x1f);
        __m128i mfe = _mm_set1_epi8((char)0xfe);
        __m128i v26 = _mm_set1_epi8(0x26); /* & ' */
        __m128i mfd = _mm_set1_epi8((char)0xfd);
        __m128i v3c = _mm_set1_epi8(0x3c); /* < > */
        __m128i vq = _mm_set1_epi8('"');
        __m128i vbs = _mm_set1_epi8('\\');
        while (i + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(b + i));
            __m128i h1 = _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v),
                                      _mm_cmpeq_epi8(_mm_and_si128(v, mfe), v26));
            __m128i h2 = _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(v, mfd), v3c),
                                      _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)));
            uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h1, h2));
            _mm_storeu_si128((__m128i *)dst, v);
            if (__builtin_expect(m == 0, 1)) { dst += 16; i += 16; continue; }
            size_t pos = (size_t)__builtin_ctz(m);
            dst += pos;
            for (;;) {
                uint8_t c = b[i + pos];
                __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                dst += ESC_HTML_LEN[c];
                pos++;
                m &= m - 1;
                if (!m) break;
                size_t nb = (size_t)__builtin_ctz(m);
                __m128i g = _mm_loadu_si128((const __m128i *)(b + i + pos));
                _mm_storeu_si128((__m128i *)dst, g);
                dst += (nb - pos);
                pos = nb;
            }
            __m128i rem = _mm_loadu_si128((const __m128i *)(b + i + pos));
            _mm_storeu_si128((__m128i *)dst, rem);
            dst += (16 - pos);
            i += 16;
        }
    }
#endif
    while (i < n) {
        uint8_t c = b[i++];
        uint8_t el = ESC_HTML_LEN[c];
        if (__builtin_expect(el == 0, 1)) *dst++ = c;
        else { __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8); dst += el; }
    }
    return dst;
}

MD_ATTR
static uint8_t *MD_FN(inline_span_json)(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, uint8_t *dst, Str *tmp);

/* Fast SIMD-assisted link handler for s[*ip] == '['.
 * Sets *em_conflict = true if em_depth > 0 and this valid link contains '*'
 * (so caller can rewind to exact reference find_unesc/find_lone_star). */
MD_ATTR
static uint8_t *MD_FN(handle_link_json)(Pool *p, LinkCtx *env, const uint8_t *s, size_t n,
                                        size_t *ip, uint8_t *dst, Str *tmp,
                                        uint8_t em_depth, bool *em_conflict) {
    size_t j = *ip;
    size_t lstart = j + 1;
    uint8_t label_cat = 0; /* bit0=simple esc, bit1=md structural */
    bool has_star = false;
    int k = -1;

    /* Fast 16B SIMD label scan (covers >80% of links since avg label ~10B). */
    size_t lrem = n - lstart;
    if (__builtin_expect(lrem > 0, 1)) {
        __m128i lv = _mm_loadu_si128((const __m128i *)(s + lstart));
        uint32_t lvalid = lrem >= 16 ? 0xffffu : ((1u << lrem) - 1u);
        uint32_t m_rb = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(lv, _mm_set1_epi8(']'))) & lvalid;
        if (__builtin_expect(m_rb != 0, 1)) {
            size_t llen0 = (size_t)__builtin_ctz(m_rb);
            uint32_t pre_mask = (1u << llen0) - 1u;
#if MD_IS_AVX2 || MD_IS_SSSE3
            __m128i lo_lut = _mm_setr_epi8(
                0x21, 0x01, 0x03, 0x01, 0x01, 0x01, 0x03, 0x03,
                0x01, 0x01, 0x09, 0x11, 0x15, 0x01, 0x05, 0x01);
            __m128i hi_lut = _mm_setr_epi8(
                0x01, 0x01, 0x0a, 0x04, 0x00, 0x10, 0x20, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
            __m128i llo = _mm_shuffle_epi8(lo_lut, lv);
            __m128i lhi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(lv, 4), _mm_set1_epi8(0x0f)));
            __m128i lhit = _mm_and_si128(llo, lhi);
            uint32_t lm_all = (((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(lhit, _mm_setzero_si128()))) ^ 0xffffu) & pre_mask;
#else
            __m128i h_sim = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(lv, _mm_set1_epi8(0x1f)), lv),
                             _mm_cmpeq_epi8(_mm_and_si128(lv, _mm_set1_epi8((char)0xfe)), _mm_set1_epi8(0x26))),
                _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(lv, _mm_set1_epi8((char)0xfd)), _mm_set1_epi8(0x3c)),
                             _mm_cmpeq_epi8(lv, _mm_set1_epi8('"'))));
            __m128i h_st = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(lv, _mm_set1_epi8('*')), _mm_cmpeq_epi8(lv, _mm_set1_epi8('['))),
                _mm_or_si128(_mm_cmpeq_epi8(lv, _mm_set1_epi8('\\')), _mm_cmpeq_epi8(lv, _mm_set1_epi8('`'))));
            uint32_t lm_all = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h_sim, h_st)) & pre_mask;
#endif
            if (__builtin_expect(lm_all == 0, 1)) {
                k = (int)(lstart + llen0);
                goto label_done;
            }
        }
    }
    {
        size_t q = lstart;
        while (q < n) {
            uint8_t ch = s[q];
            if (ch == ']') { k = (int)q; break; }
            uint8_t t = TBL_INLINE_CAT[ch];
            if (__builtin_expect(t == 0, 1)) { q++; continue; }
            if (ch == '\\') {
                label_cat |= 2;
                if (q + 1 < n && is_escapable(s[q + 1])) q += 2;
                else q += 1;
                continue;
            }
            if (ch == '*') has_star = true;
            label_cat |= t;
            q++;
        }
    }
label_done:
    if (__builtin_expect(k != -1 && (size_t)k + 1 < n && s[k + 1] == '(', 1)) {
        size_t tstart = (size_t)k + 2;
        bool has_ws = false;
        bool target_clean = true;
        size_t hash_off = (size_t)-1;
        int m = -1;

        /* Fast SIMD target scan (signed v < 0x2b catches 0x00..0x2a and >=0x80!). */
        size_t trem = n - tstart;
        if (__builtin_expect(trem > 0, 1)) {
#if MD_IS_AVX2
            __m256i tv = _mm256_loadu_si256((const __m256i *)(s + tstart));
            uint32_t tvalid = trem >= 32 ? 0xffffffffu : ((1u << trem) - 1u);
            uint32_t m_rp = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(tv, _mm256_set1_epi8(')'))) & tvalid;
            if (__builtin_expect(m_rp != 0, 1)) {
                size_t tl0 = (size_t)__builtin_ctz(m_rp);
                if (tl0 > 0) {
                    __m256i tbad = _mm256_or_si256(
                        _mm256_cmpgt_epi8(_mm256_set1_epi8(0x2b), tv),
                        _mm256_or_si256(
                            _mm256_cmpeq_epi8(_mm256_and_si256(tv, _mm256_set1_epi8((char)0xfd)), _mm256_set1_epi8(0x3c)),
                            _mm256_cmpeq_epi8(tv, _mm256_set1_epi8('\\'))));
                    uint32_t m_bad = (uint32_t)_mm256_movemask_epi8(tbad) & ((1u << tl0) - 1u);
                    if (__builtin_expect(m_bad == 0, 1)) {
                        m = (int)(tstart + tl0);
                        goto target_done;
                    }
                }
            }
#else
            __m128i tv0 = _mm_loadu_si128((const __m128i *)(s + tstart));
            uint32_t tvalid0 = trem >= 16 ? 0xffffu : ((1u << trem) - 1u);
            uint32_t m_rp = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(tv0, _mm_set1_epi8(')'))) & tvalid0;
            __m128i tbad0 = _mm_or_si128(
                _mm_cmpgt_epi8(_mm_set1_epi8(0x2b), tv0),
                _mm_or_si128(
                    _mm_cmpeq_epi8(_mm_and_si128(tv0, _mm_set1_epi8((char)0xfd)), _mm_set1_epi8(0x3c)),
                    _mm_cmpeq_epi8(tv0, _mm_set1_epi8('\\'))));
            uint32_t m_bad0 = (uint32_t)_mm_movemask_epi8(tbad0);
            if (m_rp != 0) {
                size_t tl0 = (size_t)__builtin_ctz(m_rp);
                if (tl0 > 0 && (m_bad0 & ((1u << tl0) - 1u)) == 0) {
                    m = (int)(tstart + tl0);
                    goto target_done;
                }
            } else if (trem > 16 && m_bad0 == 0) {
                __m128i tv1 = _mm_loadu_si128((const __m128i *)(s + tstart + 16));
                size_t trem1 = trem - 16;
                uint32_t tvalid1 = trem1 >= 16 ? 0xffffu : ((1u << trem1) - 1u);
                uint32_t m_rp1 = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(tv1, _mm_set1_epi8(')'))) & tvalid1;
                if (m_rp1 != 0) {
                    size_t tl1 = (size_t)__builtin_ctz(m_rp1);
                    __m128i tbad1 = _mm_or_si128(
                        _mm_cmpgt_epi8(_mm_set1_epi8(0x2b), tv1),
                        _mm_or_si128(
                            _mm_cmpeq_epi8(_mm_and_si128(tv1, _mm_set1_epi8((char)0xfd)), _mm_set1_epi8(0x3c)),
                            _mm_cmpeq_epi8(tv1, _mm_set1_epi8('\\'))));
                    uint32_t m_bad1 = (uint32_t)_mm_movemask_epi8(tbad1) & ((1u << tl1) - 1u);
                    if (m_bad1 == 0) {
                        m = (int)(tstart + 16 + tl1);
                        goto target_done;
                    }
                }
            }
#endif
        }
        {
            size_t tq = tstart;
            while (tq < n) {
                uint8_t ch = s[tq];
                if (ch == ')') { m = (int)tq; break; }
                if (ch == '*') has_star = true;
                if (ESC_HTML_LEN[ch]) target_clean = false;
                if (ch == '#' && hash_off == (size_t)-1) hash_off = tq - tstart;
                else if (ch <= ' ') {
                    if (ch == ' ' || (ch >= 9 && ch <= 13)) has_ws = true;
                } else if (__builtin_expect(ch >= 0x80, 0)) {
                    uint32_t cp; size_t ulen = (size_t)u8_decode(s, n, tq, &cp);
                    if (u_is_space(cp)) has_ws = true;
                    tq += ulen;
                    continue;
                }
                tq++;
            }
        }
target_done:
        if (m != -1) {
            size_t tl = (size_t)m - tstart;
            if (tl > 0 && !has_ws) {
                if (__builtin_expect(em_depth > 0 && has_star, 0)) {
                    *em_conflict = true;
                    return dst;
                }
                const uint8_t *target = s + tstart;
                size_t hash = (hash_off == (size_t)-1) ? tl : hash_off;
                size_t href_len; bool hclean;
                const uint8_t *href = resolve_href_ex(p, env, target, tl, hash, target_clean, tmp, &href_len, &hclean);
                size_t llen = (size_t)k - lstart;
                dst = ensure_room(env, dst, href_len * 6 + llen * 8 + (n - (size_t)m) * 8 + 1024);
                __builtin_memcpy(dst, "<a href=\\\"", 10); dst += 10;
                if (__builtin_expect(hclean, 1)) {
                    copy_fast(dst, href, href_len);
                    dst += href_len;
                } else {
                    dst = MD_FN(esc_html_json_fast)(href, href_len, dst);
                }
                __builtin_memcpy(dst, "\\\">", 3); dst += 3;
                if (__builtin_expect(label_cat == 0, 1)) {
                    copy_fast(dst, s + lstart, llen);
                    dst += llen;
                } else if (__builtin_expect((label_cat & 2) == 0, 1)) {
                    dst = MD_FN(esc_html_json_fast)(s + lstart, llen, dst);
                } else {
                    dst = MD_FN(inline_span_json)(p, env, s + lstart, llen, dst, tmp);
                }
                __builtin_memcpy(dst, "</a>", 4); dst += 4;
                *ip = (size_t)m + 1;
                return dst;
            }
        }
    }
    *dst++ = '[';
    *ip = j + 1;
    return dst;
}

typedef struct {
    uint8_t kind;        /* 1 = em (*), 2 = strong (**) */
    size_t open_dst_off; /* offset into env->out_str->data */
    size_t open_j;       /* opening index in s */
    size_t open_broken;  /* env->broken->len before opening */
} MD_FN(EmFrame);

/* In-vector structural+simple bitmask drain macro: drains simple escapes, \, *, and **
 * without exiting or reloading the vector! Only jumps to do_struct on `, [, rewind, or
 * a multi-byte token straddling the vector boundary. */
#define MD_DRAIN_MIXED(W, LOAD_V, STORE_V) do {                                             \
    size_t pos = (size_t)__builtin_ctz(m_all);                                              \
    dst += pos;                                                                             \
    for (;;) {                                                                              \
        uint8_t c = s[j + pos];                                                             \
        if (!(m_st & (1u << pos))) {                                                        \
            __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);                                    \
            dst += ESC_HTML_LEN[c];                                                         \
            pos++;                                                                          \
            m_all &= m_all - 1;                                                             \
        } else if (c == '*') {                                                              \
            if (pos + 1 >= (W)) { j += pos; goto do_struct; }                               \
            if (s[j + pos + 1] == '*') {                                                    \
                if (in_strong) {                                                            \
                    if (__builtin_expect(em_st[em_depth - 1].kind == 2, 1)) {               \
                        __builtin_memcpy(dst, "</strong>", 9); dst += 9;                    \
                        em_depth--; in_strong = 0;                                          \
                        m_all &= ~(3u << pos);                                              \
                        pos += 2;                                                           \
                    } else { j += pos; goto do_struct; }                                    \
                } else {                                                                    \
                    if (pos + 3 >= (W)) { j += pos; goto do_struct; }                       \
                    if (__builtin_expect(!(s[j + pos + 2] == '*' && s[j + pos + 3] == '*'), 1)) { \
                        em_st[em_depth].kind = 2;                                           \
                        em_st[em_depth].open_dst_off = (size_t)(dst - env->out_str->data);  \
                        em_st[em_depth].open_j = j + pos;                                   \
                        em_st[em_depth].open_broken = env->broken->len;                     \
                        em_depth++; in_strong = 1;                                          \
                        __builtin_memcpy(dst, "<strong>", 8); dst += 8;                     \
                    } else {                                                                \
                        dst[0] = '*'; dst[1] = '*'; dst += 2;                               \
                    }                                                                       \
                    m_all &= ~(3u << pos);                                                  \
                    pos += 2;                                                               \
                }                                                                           \
            } else {                                                                        \
                if (in_em) {                                                                \
                    if (__builtin_expect(em_st[em_depth - 1].kind == 1, 1)) {               \
                        __builtin_memcpy(dst, "</em>", 5); dst += 5;                        \
                        em_depth--; in_em = 0;                                              \
                    } else { j += pos; goto do_struct; }                                    \
                } else {                                                                    \
                    if (pos + 2 >= (W) && j + pos + 2 >= n) {                               \
                        *dst++ = '*';                                                       \
                    } else {                                                                \
                        em_st[em_depth].kind = 1;                                           \
                        em_st[em_depth].open_dst_off = (size_t)(dst - env->out_str->data);  \
                        em_st[em_depth].open_j = j + pos;                                   \
                        em_st[em_depth].open_broken = env->broken->len;                     \
                        em_depth++; in_em = 1;                                              \
                        __builtin_memcpy(dst, "<em>", 4); dst += 4;                         \
                    }                                                                       \
                }                                                                           \
                pos++;                                                                      \
                m_all &= m_all - 1;                                                         \
            }                                                                               \
        } else if (c == '\\') {                                                             \
            if (pos + 1 >= (W)) { j += pos; goto do_struct; }                               \
            if (__builtin_expect(is_escapable(s[j + pos + 1]), 1)) {                        \
                uint8_t e = s[j + pos + 1];                                                 \
                if (e == '>') { __builtin_memcpy(dst, "&gt;", 4); dst += 4; }               \
                else if (e == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }             \
                else { *dst++ = e; }                                                        \
                m_all &= ~(3u << pos);                                                      \
                pos += 2;                                                                   \
            } else {                                                                        \
                dst[0] = '\\'; dst[1] = '\\'; dst += 2;                                     \
                pos++;                                                                      \
                m_all &= m_all - 1;                                                         \
            }                                                                               \
        } else {                                                                            \
            j += pos; goto do_struct;                                                       \
        }                                                                                   \
        if (!m_all) break;                                                                  \
        size_t nb = (size_t)__builtin_ctz(m_all);                                           \
        STORE_V(dst, LOAD_V(s + j + pos));                                                  \
        dst += (nb - pos);                                                                  \
        pos = nb;                                                                           \
    }                                                                                       \
    STORE_V(dst, LOAD_V(s + j + pos));                                                      \
    dst += ((W) - pos);                                                                     \
    j += (W);                                                                               \
} while (0)

#define MD_LD256(p) _mm256_loadu_si256((const __m256i *)(p))
#define MD_ST256(p, v) _mm256_storeu_si256((__m256i *)(p), (v))
#define MD_LD128(p) _mm_loadu_si128((const __m128i *)(p))
#define MD_ST128(p, v) _mm_storeu_si128((__m128i *)(p), (v))

MD_ATTR
static uint8_t *MD_FN(inline_span_json)(Pool *p, LinkCtx *env, const uint8_t *s, size_t n, uint8_t *dst, Str *tmp) {
    size_t j = 0;
    MD_FN(EmFrame) em_st[2];
    uint8_t em_depth = 0;
    uint8_t in_strong = 0;
    uint8_t in_em = 0;

#if MD_IS_AVX2
    /* Simple bits (1,2,4 -> <=7): 0x00..0x1f (1), " & ' (2), < > (4).
     * Structural bits (8,16,32 -> >7): * (8), [ \ (16), ` (32). */
    __m256i lo_lut = _mm256_setr_epi8(
        0x21, 0x01, 0x03, 0x01, 0x01, 0x01, 0x03, 0x03,
        0x01, 0x01, 0x09, 0x11, 0x15, 0x01, 0x05, 0x01,
        0x21, 0x01, 0x03, 0x01, 0x01, 0x01, 0x03, 0x03,
        0x01, 0x01, 0x09, 0x11, 0x15, 0x01, 0x05, 0x01);
    __m256i hi_lut = _mm256_setr_epi8(
        0x01, 0x01, 0x0a, 0x04, 0x00, 0x10, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x0a, 0x04, 0x00, 0x10, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    __m256i mask0f = _mm256_set1_epi8(0x0f);
    __m256i zero = _mm256_setzero_si256();
    __m256i seven = _mm256_set1_epi8(7);
#elif MD_IS_SSSE3
    __m128i lo_lut = _mm_setr_epi8(
        0x21, 0x01, 0x03, 0x01, 0x01, 0x01, 0x03, 0x03,
        0x01, 0x01, 0x09, 0x11, 0x15, 0x01, 0x05, 0x01);
    __m128i hi_lut = _mm_setr_epi8(
        0x01, 0x01, 0x0a, 0x04, 0x00, 0x10, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    __m128i mask0f = _mm_set1_epi8(0x0f);
    __m128i zero = _mm_setzero_si128();
    __m128i seven = _mm_set1_epi8(7);
#else
    __m128i c1f = _mm_set1_epi8(0x1f);
    __m128i mfe = _mm_set1_epi8((char)0xfe);
    __m128i v26 = _mm_set1_epi8(0x26); /* & ' */
    __m128i mfd = _mm_set1_epi8((char)0xfd);
    __m128i v3c = _mm_set1_epi8(0x3c); /* < > */
    __m128i vq  = _mm_set1_epi8('"');
    __m128i vst = _mm_set1_epi8('*');
    __m128i vlb = _mm_set1_epi8('[');
    __m128i vbs = _mm_set1_epi8('\\');
    __m128i vbt = _mm_set1_epi8('`');
#endif

    for (;;) {
#if MD_IS_AVX2
        while (j + 32 <= n) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(s + j));
            __m256i lo = _mm256_shuffle_epi8(lo_lut, v);
            __m256i hi = _mm256_shuffle_epi8(hi_lut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask0f));
            __m256i hit = _mm256_and_si256(lo, hi);
            uint32_t m_all = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, zero));
            _mm256_storeu_si256((__m256i *)dst, v);
            if (__builtin_expect(m_all == 0, 1)) {
                dst += 32; j += 32;
                continue;
            }
            uint32_t m_st = (uint32_t)_mm256_movemask_epi8(_mm256_cmpgt_epi8(hit, seven));
            if (m_st == 0) {
                size_t pos = (size_t)__builtin_ctz(m_all);
                dst += pos;
                for (;;) {
                    uint8_t c = s[j + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m_all &= m_all - 1;
                    if (!m_all) break;
                    size_t nb = (size_t)__builtin_ctz(m_all);
                    __m256i g = _mm256_loadu_si256((const __m256i *)(s + j + pos));
                    _mm256_storeu_si256((__m256i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m256i rem = _mm256_loadu_si256((const __m256i *)(s + j + pos));
                _mm256_storeu_si256((__m256i *)dst, rem);
                dst += (32 - pos);
                j += 32;
                continue;
            }
            MD_DRAIN_MIXED(32, MD_LD256, MD_ST256);
        }
        while (j + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
            __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
            __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                          _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
            __m128i hit = _mm_and_si128(lo, hi);
            uint32_t m_all = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu;
            _mm_storeu_si128((__m128i *)dst, v);
            if (m_all == 0) { dst += 16; j += 16; continue; }
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(_mm_cmpgt_epi8(hit, _mm256_castsi256_si128(seven)));
            if (m_st == 0) {
                size_t pos = (size_t)__builtin_ctz(m_all);
                dst += pos;
                for (;;) {
                    uint8_t c = s[j + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m_all &= m_all - 1;
                    if (!m_all) break;
                    size_t nb = (size_t)__builtin_ctz(m_all);
                    __m128i g = _mm_loadu_si128((const __m128i *)(s + j + pos));
                    _mm_storeu_si128((__m128i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m128i rem = _mm_loadu_si128((const __m128i *)(s + j + pos));
                _mm_storeu_si128((__m128i *)dst, rem);
                dst += (16 - pos);
                j += 16;
                continue;
            }
            MD_DRAIN_MIXED(16, MD_LD128, MD_ST128);
        }
#elif MD_IS_SSSE3
        while (j + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
            __m128i lo = _mm_shuffle_epi8(lo_lut, v);
            __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
            __m128i hit = _mm_and_si128(lo, hi);
            uint32_t m_all = ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, zero))) ^ 0xffffu;
            _mm_storeu_si128((__m128i *)dst, v);
            if (__builtin_expect(m_all == 0, 1)) { dst += 16; j += 16; continue; }
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(_mm_cmpgt_epi8(hit, seven));
            if (m_st == 0) {
                size_t pos = (size_t)__builtin_ctz(m_all);
                dst += pos;
                for (;;) {
                    uint8_t c = s[j + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m_all &= m_all - 1;
                    if (!m_all) break;
                    size_t nb = (size_t)__builtin_ctz(m_all);
                    __m128i g = _mm_loadu_si128((const __m128i *)(s + j + pos));
                    _mm_storeu_si128((__m128i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m128i rem = _mm_loadu_si128((const __m128i *)(s + j + pos));
                _mm_storeu_si128((__m128i *)dst, rem);
                dst += (16 - pos);
                j += 16;
                continue;
            }
            MD_DRAIN_MIXED(16, MD_LD128, MD_ST128);
        }
#else
        while (j + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
            __m128i h_sim = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v),
                             _mm_cmpeq_epi8(_mm_and_si128(v, mfe), v26)),
                _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(v, mfd), v3c),
                             _mm_cmpeq_epi8(v, vq)));
            __m128i h_st = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(v, vst), _mm_cmpeq_epi8(v, vlb)),
                _mm_or_si128(_mm_cmpeq_epi8(v, vbs), _mm_cmpeq_epi8(v, vbt)));
            uint32_t m_all = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h_sim, h_st));
            _mm_storeu_si128((__m128i *)dst, v);
            if (__builtin_expect(m_all == 0, 1)) { dst += 16; j += 16; continue; }
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(h_st);
            if (m_st == 0) {
                size_t pos = (size_t)__builtin_ctz(m_all);
                dst += pos;
                for (;;) {
                    uint8_t c = s[j + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m_all &= m_all - 1;
                    if (!m_all) break;
                    size_t nb = (size_t)__builtin_ctz(m_all);
                    __m128i g = _mm_loadu_si128((const __m128i *)(s + j + pos));
                    _mm_storeu_si128((__m128i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m128i rem = _mm_loadu_si128((const __m128i *)(s + j + pos));
                _mm_storeu_si128((__m128i *)dst, rem);
                dst += (16 - pos);
                j += 16;
                continue;
            }
            MD_DRAIN_MIXED(16, MD_LD128, MD_ST128);
        }
#endif

        /* Masked 16B SIMD tail (1 <= rem <= 15; safe: >=64B readable/writable slack). */
        if (j < n) {
            size_t rem = n - j;
            uint32_t valid = (1u << rem) - 1u;
            __m128i v = _mm_loadu_si128((const __m128i *)(s + j));
#if MD_IS_AVX2
            __m128i lo = _mm_shuffle_epi8(_mm256_castsi256_si128(lo_lut), v);
            __m128i hi = _mm_shuffle_epi8(_mm256_castsi256_si128(hi_lut),
                                          _mm_and_si128(_mm_srli_epi16(v, 4), _mm256_castsi256_si128(mask0f)));
            __m128i hit = _mm_and_si128(lo, hi);
            uint32_t m_all = (((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()))) ^ 0xffffu) & valid;
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(_mm_cmpgt_epi8(hit, _mm256_castsi256_si128(seven))) & valid;
#elif MD_IS_SSSE3
            __m128i lo = _mm_shuffle_epi8(lo_lut, v);
            __m128i hi = _mm_shuffle_epi8(hi_lut, _mm_and_si128(_mm_srli_epi16(v, 4), mask0f));
            __m128i hit = _mm_and_si128(lo, hi);
            uint32_t m_all = (((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, zero))) ^ 0xffffu) & valid;
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(_mm_cmpgt_epi8(hit, seven)) & valid;
#else
            __m128i h_sim = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, c1f), v),
                             _mm_cmpeq_epi8(_mm_and_si128(v, mfe), v26)),
                _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(v, mfd), v3c),
                             _mm_cmpeq_epi8(v, vq)));
            __m128i h_st = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(v, vst), _mm_cmpeq_epi8(v, vlb)),
                _mm_or_si128(_mm_cmpeq_epi8(v, vbs), _mm_cmpeq_epi8(v, vbt)));
            uint32_t m_all = (uint32_t)_mm_movemask_epi8(_mm_or_si128(h_sim, h_st)) & valid;
            uint32_t m_st = (uint32_t)_mm_movemask_epi8(h_st) & valid;
#endif
            _mm_storeu_si128((__m128i *)dst, v);
            if (__builtin_expect(m_all == 0, 1)) {
                dst += rem; j = n;
            } else if (m_st == 0) {
                size_t pos = (size_t)__builtin_ctz(m_all);
                dst += pos;
                for (;;) {
                    uint8_t c = s[j + pos];
                    __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                    dst += ESC_HTML_LEN[c];
                    pos++;
                    m_all &= m_all - 1;
                    if (!m_all) break;
                    size_t nb = (size_t)__builtin_ctz(m_all);
                    __m128i g = _mm_loadu_si128((const __m128i *)(s + j + pos));
                    _mm_storeu_si128((__m128i *)dst, g);
                    dst += (nb - pos);
                    pos = nb;
                }
                __m128i rvec = _mm_loadu_si128((const __m128i *)(s + j + pos));
                _mm_storeu_si128((__m128i *)dst, rvec);
                dst += (rem - pos);
                j = n;
            } else {
                size_t spos = (size_t)__builtin_ctz(m_st);
                uint32_t m_pre = m_all & ((1u << spos) - 1u);
                if (m_pre == 0) {
                    dst += spos; j += spos;
                } else {
                    size_t pos = (size_t)__builtin_ctz(m_pre);
                    dst += pos;
                    for (;;) {
                        uint8_t c = s[j + pos];
                        __builtin_memcpy(dst, ESC_HTML_BYTES[c], 8);
                        dst += ESC_HTML_LEN[c];
                        pos++;
                        m_pre &= m_pre - 1;
                        if (!m_pre) break;
                        size_t nb = (size_t)__builtin_ctz(m_pre);
                        __m128i g = _mm_loadu_si128((const __m128i *)(s + j + pos));
                        _mm_storeu_si128((__m128i *)dst, g);
                        dst += (nb - pos);
                        pos = nb;
                    }
                    __m128i rvec = _mm_loadu_si128((const __m128i *)(s + j + pos));
                    _mm_storeu_si128((__m128i *)dst, rvec);
                    dst += (spos - pos);
                    j += spos;
                }
                goto do_struct;
            }
        }

        /* Reached end of span: check for unclosed speculative emphasis. */
        if (__builtin_expect(em_depth == 0, 1)) return dst;
        {
            uint8_t k0 = em_st[0].kind;
            dst = env->out_str->data + em_st[0].open_dst_off;
            j = em_st[0].open_j;
            env->broken->len = em_st[0].open_broken;
            em_depth = 0; in_strong = 0; in_em = 0;
            *dst++ = '*'; j++;
            if (k0 == 2) { *dst++ = '*'; j++; }
            continue;
        }

do_struct: {
            uint8_t c = s[j];
            if (__builtin_expect(c == '*', 1)) {
                if (j + 1 < n && s[j + 1] == '*') {
                    if (in_strong) {
                        if (__builtin_expect(em_st[em_depth - 1].kind == 2, 1)) {
                            __builtin_memcpy(dst, "</strong>", 9); dst += 9;
                            em_depth--; in_strong = 0; j += 2;
                        } else {
                            /* Strong below unclosed em: rewind em and emit literal '*'. */
                            dst = env->out_str->data + em_st[1].open_dst_off;
                            j = em_st[1].open_j;
                            env->broken->len = em_st[1].open_broken;
                            em_depth = 1; in_em = 0;
                            *dst++ = '*'; j++;
                        }
                    } else if (__builtin_expect(j + 3 < n && !(s[j + 2] == '*' && s[j + 3] == '*'), 1)) {
                        em_st[em_depth].kind = 2;
                        em_st[em_depth].open_dst_off = (size_t)(dst - env->out_str->data);
                        em_st[em_depth].open_j = j;
                        em_st[em_depth].open_broken = env->broken->len;
                        em_depth++; in_strong = 1;
                        __builtin_memcpy(dst, "<strong>", 8); dst += 8;
                        j += 2;
                    } else {
                        dst[0] = '*'; dst[1] = '*'; dst += 2;
                        j += 2;
                    }
                    continue;
                } else {
                    if (in_em) {
                        if (__builtin_expect(em_st[em_depth - 1].kind == 1, 1)) {
                            __builtin_memcpy(dst, "</em>", 5); dst += 5;
                            em_depth--; in_em = 0; j++;
                        } else {
                            /* Em below unclosed strong: rewind strong and emit literal "**". */
                            dst = env->out_str->data + em_st[1].open_dst_off;
                            j = em_st[1].open_j;
                            env->broken->len = em_st[1].open_broken;
                            em_depth = 1; in_strong = 0;
                            dst[0] = '*'; dst[1] = '*'; dst += 2; j += 2;
                        }
                    } else if (__builtin_expect(j + 2 < n, 1)) {
                        em_st[em_depth].kind = 1;
                        em_st[em_depth].open_dst_off = (size_t)(dst - env->out_str->data);
                        em_st[em_depth].open_j = j;
                        em_st[em_depth].open_broken = env->broken->len;
                        em_depth++; in_em = 1;
                        __builtin_memcpy(dst, "<em>", 4); dst += 4;
                        j++;
                    } else {
                        *dst++ = '*'; j++;
                    }
                    continue;
                }
            }
            if (c == '\\') {
                if (__builtin_expect(j + 1 < n && is_escapable(s[j + 1]), 1)) {
                    uint8_t e = s[j + 1];
                    if (e == '>') { __builtin_memcpy(dst, "&gt;", 4); dst += 4; }
                    else if (e == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
                    else { *dst++ = e; }
                    j += 2;
                } else {
                    dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                    j++;
                }
                continue;
            }
            if (c == '`') {
                int kb = MD_FN(scan_byte_i)(s, n, j + 1, '`');
                if (__builtin_expect(kb > (int)(j + 1), 1)) {
                    if (__builtin_expect(em_depth > 0, 0) && MD_FN(scan_byte_i)(s, (size_t)kb, j + 1, '*') >= 0) {
                        goto em_fallback;
                    }
                    __builtin_memcpy(dst, "<code>", 6); dst += 6;
                    dst = MD_FN(esc_html_json_fast)(s + j + 1, (size_t)kb - (j + 1), dst);
                    __builtin_memcpy(dst, "</code>", 7); dst += 7;
                    j = (size_t)kb + 1;
                } else {
                    *dst++ = '`'; j++;
                }
                continue;
            }
            /* c == '[' */
            bool em_conflict = false;
            dst = MD_FN(handle_link_json)(p, env, s, n, &j, dst, tmp, em_depth, &em_conflict);
            if (__builtin_expect(!em_conflict, 1)) continue;
        }

em_fallback: {
            uint8_t k0 = em_st[0].kind;
            size_t j0 = em_st[0].open_j;
            dst = env->out_str->data + em_st[0].open_dst_off;
            env->broken->len = em_st[0].open_broken;
            em_depth = 0; in_strong = 0; in_em = 0;
            if (k0 == 2) {
                int kf = MD_FN(find_unesc_i)(s, n, '*', '*', 2, j0 + 2);
                if (kf > (int)(j0 + 2)) {
                    __builtin_memcpy(dst, "<strong>", 8); dst += 8;
                    dst = MD_FN(inline_span_json)(p, env, s + j0 + 2, (size_t)kf - (j0 + 2), dst, tmp);
                    __builtin_memcpy(dst, "</strong>", 9); dst += 9;
                    j = (size_t)kf + 2;
                } else {
                    dst[0] = '*'; dst[1] = '*'; dst += 2;
                    j = j0 + 2;
                }
            } else {
                int kf = MD_FN(find_lone_star_i)(s, n, j0 + 1);
                if (kf > (int)(j0 + 1)) {
                    __builtin_memcpy(dst, "<em>", 4); dst += 4;
                    dst = MD_FN(inline_span_json)(p, env, s + j0 + 1, (size_t)kf - (j0 + 1), dst, tmp);
                    __builtin_memcpy(dst, "</em>", 5); dst += 5;
                    j = (size_t)kf + 1;
                } else {
                    *dst++ = '*';
                    j = j0 + 1;
                }
            }
        }
    }
}

MD_ATTR
static bool MD_FN(md_build_json)(Pool *p, Sp src, LinkCtx *env, Dict *slug_cache,
                                 Str *scratch, Str *tmp, Str *text_tmp,
                                 Sp *html_out, Arr *toc_out, Sp *excerpt_out,
                                 bool store_html_in_pool) {
    size_t init_need = (size_t)src.len * 8 + 16384;
    if (scratch->cap < init_need) str_grow(scratch, init_need);
    scratch->len = 0;
    env->out_str = scratch;
    env->out_end = scratch->data + scratch->cap;
    uint8_t *dst = scratch->data;

    const uint8_t *base = p->base;
    size_t off = src.off, end = off + src.len;
    size_t toc_base = toc_out->len;
    Range toc_stack[64];
    size_t toc_cnt = 0;
    Arr toc_html; arr_new(&toc_html, sizeof(Range));
    bool has_excerpt = false;
    Range excerpt_range = {0, 0};
    SmallIds ids; ids.nsmall = 0; ids.spilled = false;
    size_t i = off;
    Str *item_scratch = env->item_scratch;

    while (i < end) {
        uint8_t c0 = base[i];
        if (c0 == '\n') { i++; continue; }
        int nl = MD_FN(scan_nl_i)(base, end, i);
        size_t le = nl >= 0 ? (size_t)nl : end;
        if ((c0 == ' ' || c0 == '\t') && line_blank(base, i, le)) { i = le + 1; continue; }

        /* Fenced code (backtick-targeted closing scan + whole-block escaping). */
        size_t loff, llen;
        if (c0 == '`' && fence_open_at(base, i, le, &loff, &llen)) {
            dst = ensure_room(env, dst, (end - i) * 6 + 1024);
            __builtin_memcpy(dst, "<pre><code", 10); dst += 10;
            if (llen > 0) {
                __builtin_memcpy(dst, " class=\\\"language-", 18); dst += 18;
                copy_fast(dst, base + loff, llen); dst += llen;
                dst[0] = '\\'; dst[1] = '"'; dst += 2;
            }
            *dst++ = '>';
            i = le + 1;
            size_t code_start = i;
            size_t code_end = end;
            bool closed = false;
            size_t sp = i;
            while (sp < end) {
                int bt = MD_FN(scan_byte_i)(base, end, sp, '`');
                if (bt < 0) break;
                size_t bp = (size_t)bt;
                if (bp == code_start || base[bp - 1] == '\n') {
                    int nl2 = MD_FN(scan_nl_i)(base, end, bp);
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    if (fence_close_at(base, bp, le2)) {
                        code_end = bp;
                        i = le2 + 1;
                        closed = true;
                        break;
                    }
                    sp = le2 + 1;
                } else {
                    sp = bp + 1;
                }
            }
            if (!closed) { code_end = end; i = end; }
            if (code_end > code_start) {
                dst = MD_FN(esc_html_json_fast)(base + code_start, code_end - code_start, dst);
                if (!closed && base[code_end - 1] != '\n') {
                    dst[0] = '\\'; dst[1] = 'n'; dst += 2;
                }
            }
            __builtin_memcpy(dst, "</code></pre>\\n", 15); dst += 15;
            continue;
        }

        /* ATX heading. */
        size_t depth, t_off, t_len;
        if (c0 == '#' && heading_at(base, i, le, &depth, &t_off, &t_len)) {
            Sp text_s = {(uint32_t)t_off, (uint32_t)t_len};
            Sp slug = slug_make_fast(p, text_s, slug_cache);
            Sp id = slug_unique_small(p, slug, &ids);
            dst = ensure_room(env, dst, t_len * 8 + id.len + 128);
            dst[0] = '<'; dst[1] = 'h'; dst[2] = (uint8_t)('0' + depth); dst += 3;
            __builtin_memcpy(dst, " id=\\\"", 6); dst += 6;
            copy_fast(dst, base + id.off, id.len); dst += id.len;
            dst[0] = '\\'; dst[1] = '"'; dst[2] = '>'; dst += 3;
            uint32_t html_start = (uint32_t)(dst - scratch->data);
            dst = MD_FN(inline_span_json)(p, env, base + t_off, t_len, dst, tmp);
            uint32_t html_end = (uint32_t)(dst - scratch->data);
            dst[0] = '<'; dst[1] = '/'; dst[2] = 'h'; dst[3] = (uint8_t)('0' + depth);
            dst[4] = '>'; dst[5] = '\\'; dst[6] = 'n'; dst += 7;
            if (depth == 2 || depth == 3) {
                Toc *t = arr_slot(toc_out, p);
                t->level = (int64_t)depth; t->id = id; t->html = SP_NONE;
                Range hr = {html_start, html_end - html_start};
                if (toc_cnt < 64 && toc_html.len == 0) {
                    toc_stack[toc_cnt++] = hr;
                } else {
                    if (toc_html.len == 0) {
                        for (size_t k = 0; k < toc_cnt; k++) *(Range *)arr_slot(&toc_html, p) = toc_stack[k];
                    }
                    *(Range *)arr_slot(&toc_html, p) = hr;
                    toc_cnt++;
                }
            }
            i = le + 1;
            continue;
        }

        /* Thematic break. */
        if ((c0 == '-' || c0 == '*') && hr_at(base, i, le)) {
            dst = ensure_room(env, dst, 64);
            __builtin_memcpy(dst, "<hr>\\n", 6); dst += 6;
            i = le + 1;
            continue;
        }

        /* Blockquote. */
        if (c0 == '>') {
            str_clear(text_tmp);
            bool first = true;
            while (i < end && base[i] == '>') {
                int nl2 = MD_FN(scan_nl_i)(base, end, i);
                size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                size_t s = i + 1;
                if (s < le2 && base[s] == ' ') s++;
                size_t ts, te;
                line_trim(base, s, le2, &ts, &te);
                if (ts < te) {
                    if (!first) str_c(text_tmp, '\n');
                    first = false;
                    str_add(text_tmp, base + ts, te - ts);
                }
                i = le2 + 1;
            }
            dst = ensure_room(env, dst, text_tmp->len * 8 + 128);
            if (text_tmp->len == 0) {
                __builtin_memcpy(dst, "<blockquote>\\n</blockquote>\\n", 29); dst += 29;
            } else {
                __builtin_memcpy(dst, "<blockquote>\\n<p>", 17); dst += 17;
                dst = MD_FN(inline_span_json)(p, env, text_tmp->data, text_tmp->len, dst, tmp);
                __builtin_memcpy(dst, "</p>\\n</blockquote>\\n", 21); dst += 21;
            }
            continue;
        }

        /* Lists. */
        bool ordered; int64_t start_num; size_t cur_ts, cur_te;
        if ((c0 == '-' || c0 == '*' || (c0 >= '0' && c0 <= '9'))
            && item_start_span(base, i, le, &ordered, &start_num, &cur_ts, &cur_te)) {
            bool is_ordered = ordered;
            dst = ensure_room(env, dst, 64);
            if (is_ordered && start_num != 1) {
                __builtin_memcpy(dst, "<ol start=\\\"", 12); dst += 12;
                uint64_t v = (uint64_t)start_num;
                uint8_t nbuf[20]; int ni = 20;
                if (v == 0) { *dst++ = '0'; }
                else {
                    while (v > 0) { nbuf[--ni] = (uint8_t)('0' + v % 10); v /= 10; }
                    copy_fast(dst, nbuf + ni, (size_t)(20 - ni));
                    dst += (size_t)(20 - ni);
                }
                __builtin_memcpy(dst, "\\\">\\n", 5); dst += 5;
            } else {
                if (is_ordered) { __builtin_memcpy(dst, "<ol>\\n", 6); dst += 6; }
                else { __builtin_memcpy(dst, "<ul>\\n", 6); dst += 6; }
            }
            i = le + 1;
            for (;;) {
                bool multi = false;
                bool has_next = false;
                size_t next_ts = 0, next_te = 0, next_le = 0;
                while (i < end) {
                    uint8_t lc = base[i];
                    if (lc == '\n') { i++; break; }
                    if (lc != '-' && lc != '*' && lc != ' ' && (lc < '0' || lc > '9')) break;
                    int nl2 = MD_FN(scan_nl_i)(base, end, i);
                    size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
                    bool o2; int64_t s2;
                    size_t nts, nte;
                    if (lc != ' ' && item_start_span(base, i, le2, &o2, &s2, &nts, &nte)) {
                        if (o2 == is_ordered) {
                            has_next = true;
                            next_ts = nts; next_te = nte; next_le = le2;
                        }
                        break;
                    }
                    if (lc == ' ' && continued_at(base, i, le2)) {
                        if (!multi) {
                            str_clear(item_scratch);
                            str_add(item_scratch, base + cur_ts, cur_te - cur_ts);
                            multi = true;
                        }
                        size_t cts, cte;
                        line_trim(base, i, le2, &cts, &cte);
                        str_c(item_scratch, '\n');
                        str_add(item_scratch, base + cts, cte - cts);
                        i = le2 + 1;
                        continue;
                    }
                    break;
                }
                const uint8_t *its = multi ? item_scratch->data : base + cur_ts;
                size_t itn = multi ? item_scratch->len : (cur_te - cur_ts);
                dst = ensure_room(env, dst, itn * 8 + 64);
                __builtin_memcpy(dst, "<li>", 4); dst += 4;
                dst = MD_FN(inline_span_json)(p, env, its, itn, dst, tmp);
                __builtin_memcpy(dst, "</li>\\n", 7); dst += 7;
                if (!has_next) break;
                cur_ts = next_ts; cur_te = next_te;
                i = next_le + 1;
            }
            dst = ensure_room(env, dst, 16);
            if (is_ordered) { __builtin_memcpy(dst, "</ol>\\n", 7); dst += 7; }
            else { __builtin_memcpy(dst, "</ul>\\n", 7); dst += 7; }
            continue;
        }

        /* Paragraph (zero-copy when lines have no leading/trailing ws). */
        size_t ts, te;
        line_trim(base, i, le, &ts, &te);
        size_t span_start = ts, span_end = te;
        bool contiguous = (te == le);
        i = le + 1;
        bool multi = false;
        while (i < end) {
            uint8_t nc = base[i];
            if (nc == '\n') { i++; break; }
            int nl2 = MD_FN(scan_nl_i)(base, end, i);
            size_t le2 = nl2 >= 0 ? (size_t)nl2 : end;
            if (__builtin_expect(!PARA_END_FIRST[nc], 1)) {
                size_t cte = le2;
                if (__builtin_expect(cte > i && (base[cte - 1] == ' ' || base[cte - 1] == '\t'), 0)) {
                    while (cte > i && (base[cte - 1] == ' ' || base[cte - 1] == '\t')) cte--;
                }
                if (__builtin_expect(!multi && contiguous, 1)) {
                    span_end = cte;
                    contiguous = (cte == le2);
                } else {
                    if (!multi) {
                        str_clear(text_tmp);
                        str_add(text_tmp, base + span_start, span_end - span_start);
                        multi = true;
                    }
                    str_c(text_tmp, '\n');
                    str_add(text_tmp, base + i, cte - i);
                }
                i = le2 + 1;
                continue;
            }
            if (para_ends(base, i, le2)) break;
            size_t cts, cte;
            line_trim(base, i, le2, &cts, &cte);
            if (!multi && contiguous && cts == i) {
                span_end = cte;
                contiguous = (cte == le2);
            } else {
                if (!multi) {
                    str_clear(text_tmp);
                    str_add(text_tmp, base + span_start, span_end - span_start);
                    multi = true;
                }
                str_c(text_tmp, '\n');
                str_add(text_tmp, base + cts, cte - cts);
            }
            i = le2 + 1;
        }
        const uint8_t *ps = multi ? text_tmp->data : base + span_start;
        size_t pn = multi ? text_tmp->len : (span_end - span_start);
        dst = ensure_room(env, dst, pn * 8 + 64);
        dst[0] = '<'; dst[1] = 'p'; dst[2] = '>'; dst += 3;
        uint32_t html_start = (uint32_t)(dst - scratch->data);
        dst = MD_FN(inline_span_json)(p, env, ps, pn, dst, tmp);
        uint32_t html_end = (uint32_t)(dst - scratch->data);
        if (!has_excerpt) {
            has_excerpt = true;
            excerpt_range.start = html_start;
            excerpt_range.len = html_end - html_start;
        }
        __builtin_memcpy(dst, "</p>\\n", 6); dst += 6;
    }

    scratch->len = (size_t)(dst - scratch->data);
    if (store_html_in_pool) {
        *html_out = pool_store(p, scratch->data, scratch->len);
        if (has_excerpt) {
            *excerpt_out = (Sp){html_out->off + excerpt_range.start, excerpt_range.len};
        } else {
            *excerpt_out = SP_NONE;
        }
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){html_out->off + r.start, r.len};
        }
    } else {
        *html_out = (Sp){0, (uint32_t)scratch->len};
        if (has_excerpt) {
            *excerpt_out = pool_store(p, scratch->data + excerpt_range.start, excerpt_range.len);
        } else {
            *excerpt_out = SP_NONE;
        }
        for (size_t k = 0; k < toc_cnt; k++) {
            Range r = toc_html.len ? *(Range *)arr_at(&toc_html, k) : toc_stack[k];
            Toc *t = arr_at(toc_out, toc_base + k);
            t->html = (Sp){r.start, r.len};
        }
    }
    return true;
}

#undef MD_DRAIN_MIXED
#undef MD_LD256
#undef MD_ST256
#undef MD_LD128
#undef MD_ST128
#undef MD_FN
#undef MD_CAT
#undef MD_CAT2
