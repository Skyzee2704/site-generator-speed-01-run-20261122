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

## Ronde 4, chunk 1 (driver record — agy did NOT checkpoint this)

- Agy ran ~21 min (print timeout 30m fired; exec session then SIGTERMed).
  Run log essentially lost: agy-opt-run4.log is 2 lines only ("Waiting for
  the R4-1 vs M5 benchmark to complete..." + "error: interrupted").
- Agy's work as delivered in the exec tail (UNVERIFIED claims): M6 = SSE2
  scalar-escape fast path for HTML/JSON escaping (`scan_inline_span_sse2`,
  `esc_html_sse2`), claimed -16%.
- DRIVER verification of the left-behind binary `site` (md5
  e5377ebb06bb7e94639fa3422558d8fe, 177,904 B): diff-expected 9/9 identical;
  fuzz-e2e 3000/3000 clean; timeit median 9: M6 27 ms vs M5 27 ms SAME
  SESSION — the -16% claim did NOT reproduce. M6 is statistically identical
  to M5. Implied house ~70k, rank ~31 zone — NO zone change, top 10 NOT
  reached. Backed up as site-m6-verified + src-m6-backup/.

## Ronde 4, chunk 2

### R4B-M1 (Verified — backed up to `site-r4b-m1`)
- Binary: `site-r4b-m1`, md5 `322ce8f9c91137bf75eaf9bdcd67c6d4`, size 178,064 B (non-PGO `-O3 -flto=auto`).
- Changes:
  - Restored `os_map_huge` in `pool_open` (minflt dropped 554 -> 102).
  - Streaming 1-case-at-a-time pipeline (`json_all_begin`/`json_all_next`) rewinding `pool.len = 0` between cases (L2-resident working set).
  - Fused 1-pass `.md` JSON unescape + CRLF->LF + closing `---` detection + SIMD `word_count` (`parse_string_md`, eliminating separate `skip_json_string` + `json_unescape_fm` + `json_unescape_body` + `word_count` passes).
  - Inline `meta_nl` + `meta_parse_hint` in `front.c`, cached `post`/`page` template indices & `taken_posts` set, hash-based `tag_by_slug`, and reused `static Str` in `tpl.c:lex_tokens` (eliminated 70 `mmap` syscalls).
- Verification: `diff-expected.mjs` 9/9 ALL GENERATED SETS IDENTICAL; `fuzz-e2e.mjs 3000` FUZZ E2E CLEAN; byte-identical to M6.
- Timing (same session, non-PGO R4B-M1 vs PGO M6):
  - `timeit.py 9`: R4B-M1 median **32 ms** (wall 42 ms) vs M6 **35 ms** (wall 46 ms).
  - `inproc` AVX2 (30 iters): **16.61 ms** mean (16.32 ms min) vs M6 **19.67 ms** mean (-15.6% pure compute before PGO).

### R4B-M2 + driver verification chunk 2 (driver record)
- Chunk 2 ran the full 30 min print timeout (AGY_EXIT:3, DEADLINE_EXCEEDED,
  partial output). Agy built R4B-M1 (saved as site-r4b-m1) and R4B-M2
  (current `site`, md5 1bb376e8c811fd1709d32eb965234d26, 218,768 B, backed up
  as site-r4b-m2). Agy checkpointed R4B-M1 in STATUS-OPT.md but left NO
  written record of what R4B-M2 changed (no M2 section; only the exec tail
  says it passed practice 3/3).
- DRIVER verification (same session, median 9, binary alone):
  - R4B-M2: diff-expected 9/9 identical; fuzz-e2e 3000/3000 clean; **24 ms**.
  - R4B-M1: diff-expected 9/9 identical; fuzz-e2e 3000/3000 clean; **22 ms**.
  - M6 control: **24 ms**. M5 was 22-29 ms in earlier sessions.
  - Verdict: M1 is ~8% faster than M6/M2 — BELOW the +-15 spread, NOT a
    proven gain. All three (M1/M2/M6) are statistically equivalent.
  - Agy's in-process AVX2 claim (-15.6%, 16.61 vs 19.67 ms) is a pure-compute
    metric, not the end-to-end binary time the house measures; end-to-end
    shows ~8%.
- Implied house (M5 ratio 2.4-2.9x): M1 ~57-64k (rank ~23-24, INSIDE final
  heat), M2/M6 ~62-70k (rank ~25-31, borderline). Top 10 (20k) NOT reached;
  top 1 (10k) NOT reached. Gap to top 10 is still ~3x.
- SOURCE NOTE: current source files on disk build R4B-M2 (M2 built 18:01,
  md.c edited 18:04). R4B-M1 has NO source snapshot (agy saved only the
  binary), so M1 cannot be rebuilt from a matching source. `site` restored
  to R4B-M2 for source consistency.
- Round 4 total: chunk 1 M6 (0 gain vs M5) + chunk 2 M1/M2 (~8%, noise).
  Target top 10 NOT reached in either chunk.

## Chunk 3 — C3-1 (driver-written, agy run 1 timed out)

- Run 1 (02:08–02:33 WITA): agy built C3-1 (zero-copy raw-JSON `.md` body
  skip + fused single-pass raw-JSON Markdown engine + fixed SSE2 loop),
  binary `site` md5 83589f044b60a79a31bc183096d59367, 214.032 B. Run mati
  kena timeout 25 menit di tengah benchmark/diagnostik; log hanya 5 baris,
  agy TIDAK sempat menulis ringkasan. Source yang diubah: md.c, pipe.c,
  jsonin.c, zg.h. Artefak diagnostik di `.work/`: diag_c3/diag_c3b
  (json-vs-case split benchmark), md_prof_c3.c, .work/inproc/b_c3_1.
- DRIVER verification (bukan klaim agy): diff-expected 9/9 identik,
  fuzz-e2e 3000/3000 bersih, timeit median 9 C3-1 = 26 ms vs M2 = 24 ms
  (sesi yang sama) — C3-1 BENAR tapi TIDAK LEBIH CEPAT (0%, dalam noise).
  Fused engine tidak menghasilkan gain end-to-end.
- Run 2 (lanjutan) akan memakai diagnostik C3-1 untuk mencari tuas berikutnya.

## Chunk 3 run 2

### Langkah 1 — Diagnostik C3-1 vs M2 vs R4B-M1 (selesai)
Angka diukur langsung di sesi yang sama pada practice `cases.json` (2,93 MB input / 3 kasus / 1.070 file `.md` = 2.706 KB Markdown; 7,45 MB output; TSC = 1,498 GHz → 1,50 Mcyc ≈ 1,00 ms):

1. **In-process compute (`cases_build`, N=30, tanpa I/O file)**:
   - `b_r4b_m1` (AVX2): mean **16,808 ms** (min 16,448 ms)
   - `b_r4b_m2` (AVX2): mean **19,148 ms** (min 17,730 ms) — regresi **+2,34 ms** dari R4B-M1 (rewrite `md.c` di M2)
   - `b_c3_1`   (AVX2): mean **20,194 ms** (min 19,101 ms) — regresi **+1,05 ms** dari M2 (**+3,39 ms** dari R4B-M1!)
   - `b_c3_1`   (SSE2): mean **23,105 ms** vs `b_r4b_m2` (SSE2): mean **20,543 ms**

2. **Split `json` vs `case` (`diag_c3` vs `diag_m2`, N=20)**:
   - `diag_m2` AVX2: `json=2,872 ms`, `case=17,165 ms`, `total=20,037 ms`
   - `diag_c3` AVX2: `json=0,766 ms`, `case=20,475 ms`, `total=21,241 ms`
   - **Penyebab C3-1 tidak lebih cepat (bahkan lebih lambat):**
     1. `json` memang hemat **2,11 ms** (skip unescape body `.md`), TAPI `case` membengkak **+3,31 ms**.
     2. `diag_c3b` (`tot=32,54 Mcyc`): `scan_raw_md_meta_avx2` saja menghabiskan **3,93 Mcyc (2,62 ms)** — lebih mahal dari seluruh hematan `json` (2,11 ms) — karena tetap memindai ulang 2,7 MB raw JSON body untuk `word_count` + `can_direct`, dan terus terputus oleh tiap `\` (`\n`, `\"`, `\\`, `\t`).
     3. **43% paragraf gagal fast-path (`pfast=3571`, `pslow=2677`)** di `emit_para_raw_*`: tiap kali `handle_raw_special` menemui `**`/`*`/`` ` ``/`[` yang berisi karakter khusus (mis. apostrof, kutip, newline, nested markup, image), ia rollback `out->len` lalu mem-parse ulang seluruh paragraf lewat `emit_para_raw_slow` (double work!).
     4. Ukuran `md.c` membesar dari 66 KB → 127 KB (tekanan I-cache).

3. **End-to-end `wait4` (`user` vs `sys`, median 9, sesi sama)**:
   - Output file **fresh** (unlink tiap run, mirip container grading):
     - `C3-1`:   **30,7 ms** (`u=22,9 s=8,8`)
     - `M2`:     **26,1 ms** (`u=19,1 s=7,1`)
     - `R4B-M1`: **24,9 ms** (`u=18,6 s=7,0`)
   - Output file **trunc** (overwrite berulang seperti `timeit.py`):
     - `C3-1`:   **32,0 ms** (`u=21,7 s=10,6`)
     - `M2`:     **31,2 ms** (`u=20,5 s=10,9`)
     - `R4B-M1`: **31,0 ms** (`u=19,3 s=12,1`)
   - **Floor kernel I/O lokal (`wbench` & `io_bench`)**:
     - `write(7,45 MB)` fresh = **5,7 ms** (`trunc` = **11,0 ms**); `mmap_ro` 2,93 MB = **0,5 ms**; `execve`+`mmap`+101 minflt ≈ **0,8–1,0 ms**. Jadi floor kernel `sys` lokal ≈ **7,0 ms** (fresh) / **11 ms** (trunc).

4. **Breakdown user compute (`prof_r4b/prof`, M6 baseline, 26,68 Mcyc ≈ 17,8 ms)**:
   - `md_build`: **12,10 Mcyc (45,3%)** — di dalamnya `inline_sum=8,63 Mcyc` (`para=6,44M`, `list=1,60M`, `bq=0,52M`), `block_overhead=3,91 Mcyc`, `md_href=1,01 Mcyc`, `md_code=0,41 Mcyc`.
   - `tpl` render (`pages+index+tag+rest`): **3,74 Mcyc (14,0%)**
   - `json_read` + `unesc_body` + `word_count`: **5,87 Mcyc (22,0%)** (sudah sebagian difusikan di R4B-M1 → total turun ke 16,8 ms)
   - `front`: **2,38 Mcyc (8,9%)**
   - `tpl_parse` + `tags_col` + `sort`: **2,16 Mcyc (8,1%)**

5. **Langkah berikutnya**: Selidiki struktur aktual Markdown/template di dataset + mengapa `md_build` masih 12,1 Mcyc (4,5 cycle/byte) & mengapa M2 regresi +2,3 ms dari R4B-M1, lalu pilih SATU tuas terbesar yang belum dicoba.

## Chunk 3 — SELESAI (driver-written, 02:50 WITA)

- Run 1 (02:08–02:33): C3-1 (fused single-pass raw-JSON Markdown engine),
  md5 83589f04, 214.032 B. Mati timeout 25 menit. DRIVER-verified: 9/9
  identik + fuzz 3000 bersih, timeit median 9 = 26 ms vs M2 24 ms (sesi
  yang sama) — BENAR tapi 0% gain.
- Run 2 (02:37–02:53): mati kena QUOTA 429 RESOURCE_EXHAUSTED
  ("Resets in 3h18m30s" → ~06:11 WITA Rab 25 Nov, SETELAH entri tutup
  04:00 WITA — agy TIDAK bisa jalan lagi untuk kompetisi ini). Agy sempat
  menulis analisis diagnostik (lihat atas) + membangun C3-2 dan C3-3 di
  `.work/` sebelum mati.
- Diagnostik agy (run 2): C3-1 json hemat 2,11 ms tapi case +3,31 ms;
  scan_raw_md_meta_avx2 sendiri 3,93 Mcyc (2,62 ms); 43% paragraf gagal
  fast-path (pfast=3571, pslow=2677) → double work; md.c 66 KB → 127 KB
  (tekanan I-cache). End-to-end fresh-output: M1 24,9 ms, M2 26,1 ms,
  C3-1 30,7 ms. Floor kernel I/O lokal ≈ 7,0 ms (write 7,45 MB 5,7 ms +
  mmap 0,5 ms + execve ~1 ms). Breakdown M6: md_build 12,10 Mcyc (45,3%),
  json+unesc+word_count 5,87 (22%), tpl render 3,74 (14%).
- DRIVER-verified C3-2 (md5 defa9326, .work/site-c3-2): 9/9 identik,
  timeit median 9 = 25 ms. C3-3 (md5 d164a47d, .work/site-c3-3): 9/9
  identik, timeit median 9 = 24 ms. Fuzz TIDAK dijalankan untuk keduanya
  (tidak ada kasus kecepatan — setara M2). Backup: site-c3-2-verified,
  site-c3-3-verified (lane root); site-c3-1 (= lane `site` saat ini).
- CAVEAT SOURCE: tidak ada snapshot source per-kandidat untuk C3-2/C3-3
  (source di lane = state run-2 terakhir, kemungkinan C3-3 tapi tidak
  terbukti). M2/M5/M6/C3-1 punya backup source sendiri.
- KESIMPULAN CHUNK 3: semua kandidat 22–26 ms, setara statistik dengan M5.
  Target top 10 (butuh ~7-9 ms lokal, 3x) TIDAK tercapai. Gap tetap ~3x.
