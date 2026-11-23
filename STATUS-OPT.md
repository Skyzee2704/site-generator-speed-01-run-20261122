# STATUS-OPT.md — Optimization Progress Log

## Baseline (Verified)
- Base binary backed up to `site-base-verified`: md5 `9576bfb071d449d7e03121072b56687f`, size 91,464 B.
- Verification:
  - `diff-expected.mjs` (practice + fresh1 + fresh2): 9/9 ALL GENERATED SETS IDENTICAL (differing=0, extra=0, orderSame=true).
  - `fuzz-e2e.mjs 3000`: FUZZ E2E CLEAN (3000 cases identical).
- Timing (practice input 2.93 MB / 3 cases, output 7.42 MB):
  - Binary alone (`timeit.py 9`): CPU median **53 ms** (52–72 ms), wall median 81 ms.
  - `wait4` breakdown: user ~42–58 ms, sys ~11–19 ms, **4,686 minor page faults**, maxrss 18.7 MB.
  - `main.ts` gross (`timeit.py 9`): CPU median **241 ms**; null control: **218 ms**; net ~23 ms.
- Hardware: AMD EPYC 9D25 (AVX2/BMI1/BMI2/POPCNT/AVX-512 available locally; binary must keep baseline x86-64 SSE2 fallback via runtime CPUID check).
- THP: `/sys/kernel/mm/transparent_hugepage/enabled` = `always [madvise] never`.

## Profiling Findings (RDTSC in `/tmp/prof`, ~80–86 Mcyc total)
- `md`: ~35.5 Mcyc (42–44%) — `inline_span` ~19.1 Mcyc, block parsing + slug + store ~13.7 Mcyc, `word_count` ~2.7 Mcyc, `resolve_href` ~2.4 Mcyc, `esc_html_into` ~1.8 Mcyc.
- `render`: ~22.6–27.1 Mcyc (28–32%) — `json_escape_into` ~9.2–13.4 Mcyc (mostly escaping `page.content`!), `html_json_escape_into` ~3.2 Mcyc, `tpl_eval` ~2.1 Mcyc, rest ~8 Mcyc (page faults on `out` + include cache + Val 56-byte sret copies).
- `json`: ~5.9 Mcyc (7%)
- `write`: ~4.9–5.7 Mcyc (6%)
- `front`: ~3.8–4.0 Mcyc (4.5%)
- `read`: ~3.0 Mcyc (3.5%)
- `tplparse`: ~1.6 Mcyc (2%)
- `tags` + `report` + `sort` + `col`: ~2.6 Mcyc (3%)

## Optimization Plan
1. **Milestone 1 — Memory/OS & Core Primitives**:
   - `madvise(MADV_HUGEPAGE)` on pools (aligned 2 MB or kernel-promoted), rewind working `pool.len` between cases (eliminates redundant page faults across cases), eliminate duplicate `excerpt`/`toc.html` copies in `md_build` (slices into `html_out`).
   - Inline hot primitives (`arr_at`, `pool_put`, `pool_copy`, `str_c`, `str_add`, `str_lit`, `copy_bytes`, `cmp_bytes`) in `zg.h`, replace `first_bit` while-loop with `__builtin_ctz`, compile with `-flto`.
   - Runtime CPUID detection (`has_avx2`, `has_ssse3`, `has_popcnt`) for fast AVX2 `memcpy`/`memcmp` and SIMD scans with safe SSE2 fallback.
2. **Milestone 2 — Direct JSON-Escaped Markdown Emission**:
   - When no template uses `page.content`/`excerpt`/`toc.html` in HTML-escaped `{{ }}` mode (checked during `tpl_analyze`), emit JSON-escaped HTML directly in `md_build` (straight into `pool`, eliminating `scratch->pool` copy + eliminating ALL `json_escape_into` scans on `page.html`/`excerpt`/`toc.html` during `render`).
3. **Milestone 3 — AVX2 PSHUFB Nibble-Table Scans & Markdown Fast Paths**:
   - Runtime-dispatched AVX2 (32 B/iter) + SSE2 (16 B/iter) specialized scanners for: inline_span (9 needles), html_esc (5 needles), html_json_esc (6 needles + `<0x20`), json_esc (2 needles + `<0x20`), scan_nl, scan_quote_bs, word_count (with popcnt).
   - Fast markdown block loop (direct byte dispatch on first char instead of calling 4 functions per line; zero-copy list items / paragraphs when single-line; hash table for `page_lookup` / `static_lookup` instead of binary search with `u16_cmp` on every link!).
   - Fast JSON string unescape (AVX2 32-byte copy loop).
4. **Milestone 4 — Template Engine & Value Model Overhaul**:
   - Shrink `Val` from 56 bytes to 16 bytes (fits in 2 registers, eliminates sret + zero-init overhead).
   - Fix include cache: skip hashing/probing when `!t->storable`; add O(1) direct cache for global templates (0 dynamic deps) and single-card templates (indexed by page/tag idx).
   - Pre-escape / cache `html_json_escape` for repeatedly rendered fields (`site.title`, `site.base_url`, `page.title`, `page.url`, `page.summary`, `tag.name`, `tag.url`) and fast-path known-safe ASCII fields (`date`, `date_long`, `slug`).
   - Faster hash function (`wyhash`/FxHash-style 64-bit word hashing instead of byte-by-byte FNV-1a).
   - Specialize/inline `tpl_eval` and `tpl_emit` hot loops.
5. **Milestone 5 — PGO (`-fprofile-generate` / `-fprofile-use`) & Micro-tuning**.

## Progress Log

### Milestone 1 (Completed)
- Added runtime CPUID detection (`g_has_avx2`, `g_has_popcnt`), `os_map_huge` (`MADV_HUGEPAGE`), pool rewind between cases, reused `Str` scratch buffers across cases, zero-copy `excerpt`/`toc.html` slices into `html_out`, inlined hot primitives in `zg.h`, AVX2 `copy_bytes`, AVX2 PSHUFB scanners (`scan_byte`, `scan_two`, `scan_json_esc`, `scan_html_esc`, `scan_inline_md`, `scan_html_json_esc`), and AVX2+POPCNT `word_count`.
- Verified: 9/9 identical, fuzz-e2e 3000 clean.
- Timing (`timeit.py 9`, binary alone): CPU median **40–41 ms** (down from 53–56 ms).

### Milestone 2 (Completed)
- Added direct JSON-escaped Markdown emission (`md_build(..., bool json_esc)` with `scan_inline_md_json` AVX2 PSHUFB + SSE2 fallback, `inline_span_json`, `esc_html_json_into_str`).
- Checked template ASTs in `tpl_can_pre_json_md` so `md_pre_json` only activates when no template HTML-escapes `content`/`excerpt`/`toc.html` (`{{ }}`).
- Added `K_STR_JSON` value kind for pre-escaped Markdown outputs and known-safe ASCII fields (`date`, `date_long`, `slug`, `toc.id`).
- Fixed `tpl_emit` include cache to skip hashing/probing when `!t->storable`.
- Verified: `diff-expected.mjs` 9/9 ALL GENERATED SETS IDENTICAL; `fuzz-e2e.mjs 3000` FUZZ E2E CLEAN.
- Timing (`timeit.py 9`, binary alone): CPU median **38 ms** (runs 36–48 ms); `wait4` user median **25.5 ms**, sys median **9.9 ms**, `minflt=104`.
- RDTSC after M2 (~44 Mcyc total): `md` 20.3 Mcyc (46%, of which `inline_md` 12.1M, `md_wc` 2.5M, block+slug+resolve 5.7M), `render` 8.0 Mcyc (18%, down from 21.0 Mcyc!), `json` 5.5–6.9 Mcyc (13%), `write` 4.2–4.5 Mcyc (10%), `front` 2.4 Mcyc (5.5%), `read` 1.1 Mcyc (2.5%), `tpl` 0.9 Mcyc, `col_tags` 0.8 Mcyc, `report` 0.5 Mcyc.

### Milestone 3 (Completed)
- Added fused AVX2 speculative-store `inline_span_json_avx2`, `esc_html_json_into_str_avx2`, and `html_json_escape_into_avx2` (keeping PSHUFB LUTs in YMM registers across loops, eliminating double-read scan+copy).
- Added AVX2 32B/iter `parse_string_escaped_avx2` with direct `__builtin_ctz` jump and fast inline escapes (`\n`, `\"`, `\\`, `\t`, `\r`).
- Replaced `fnv1a` in `hmap.c` with 64-bit word hash `hash_bytes` + `eq_bytes`; added O(1) `page_map` / `static_set` in `LinkCtx`, pre-split `env.dir` once per page, zero-copy `resolve_href` for `/`/`#`/scheme/no-fragment page links, and O(1) `written_set` for static shadowing.
- Added first-char dispatch in `md_build` and `para_ends`, zero-copy single-line paragraphs, and zero-copy streaming lists.
- Improved `word_count_avx2` to only drop to scalar UTF-8 decode on potential non-ASCII space lead bytes (`0xC2`, `0xE0..0xEF`).
- Verified: `diff-expected.mjs` 9/9 ALL GENERATED SETS IDENTICAL; `fuzz-e2e.mjs 3000` FUZZ E2E CLEAN.
- Timing (`timeit.py 9`, binary alone): CPU median **29 ms** (runs 27–34 ms, wall median 32 ms) — ~1.8–1.9x faster than base 53–56 ms.

### Milestone 4 (Completed)
- Shrunk `Val` from 56 bytes to 16 bytes (register-passed, eliminating sret + zero-init overhead).
- Added `clean_mask` bypass for clean titles, URLs, summaries, tags, and pagers (skipping redundant HTML/JSON escaping when inputs contain no special bytes).
- Enabled link-time optimization (`-flto=auto`).
- Backed up verified M4 binary to `site-m4-verified` (md5 `d756b8df4e5368636de94f8ed18817ee`, 140,408 B) and M4 sources to `src-m4-backup/`.
- Verified: `diff-expected.mjs` 9/9 ALL GENERATED SETS IDENTICAL; `fuzz-e2e.mjs 3000` FUZZ E2E CLEAN.
- Timing (`timeit.py 9`): binary alone CPU median **29 ms** (runs 27–31 ms); `main.ts` gross **217 ms**, null **209 ms**, net **8 ms**. (No measurable binary-alone median improvement over M3's 29 ms.)

### Milestone 5 (Completed — driver-verified; agy session cut short by quota 429)
- PGO builds (site-pgo, site-pgo2, site-pgo3, site-m5-pgo) + micro-tuning; THP vs non-THP and SSE2-fallback variants compared in .work/ (15-round interleaved A/B + 30-iteration in-process benchmark were running when agy hit the Antigravity individual quota, 429 RESOURCE_EXHAUSTED, ~16:19 UTC; quota reset ~49 min later).
- Current `site` (md5 f53320388843082652fc9c227700f38b, 153,080 B) and `.work/site-m5-pgo` (md5 83ed725b181913022512d1cfa427d3c8) are the M5 builds.
- DRIVER verification (not agy claims): diff-expected.mjs 9/9 ALL GENERATED SETS IDENTICAL; fuzz-e2e.mjs 3000 FUZZ E2E CLEAN.
- Timing (`timeit.py 9`, driver-measured): binary alone CPU median **24 ms** (current site runs 22-25 ms; site-m5-pgo also 24 ms); main.ts gross **203 ms**, null **203 ms**, net **~0 ms**. M4 re-measured same session: 30 ms. So M5 ≈ 1.2x over M4, **2.3x over base 53-56 ms**.
- SSE2 fallback variant (`.work/site-sse2`, md5 7bbfad5db8527a6e7ddf721fc39bf718): DRIVER-verified 9/9 identical + fuzz 3000 clean; binary alone median **30 ms**. Fallback is CORRECT — safe if the grading CPU lacks AVX2.
- Implied house (ratio ~1.8x): ~43k (AVX2 path, rank ~18-22, final heat) / ~54k (SSE2 path, rank ~25-28, still final heat). Top 10 (20k bar) NOT reached; top 5 (0-10k) NOT reached.
