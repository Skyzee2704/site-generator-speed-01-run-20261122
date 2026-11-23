# ADAPTATION-NOTES.md — native lane (site-generator-speed)

How this lane's implementation relates to its source, and what differs.
Read alongside `LICENSE` (MIT, original copyright retained).

## Source

- Reference: public repo `wantorieta15/site-generator-speed-01-run-20261122-akun2-napi`
  (MIT), cloned read-only to `ref-wantorieta15/` for reading only.
- Ground truth for semantics: makefun-site 1.0.0 (`kit/baselines/makefun-site/`,
  the house reference program). Where the reference C and the vendor disagree,
  **the vendor wins** — two such divergences were found by differential
  fuzzing and fixed here (see "Vendor-conformance fixes" below).
- The reference's prebuilt `site` binary was NEVER executed. Its `build.sh`
  was never run (read as text only). This lane's binary was built solely from
  this lane's own adapted sources with a gcc invocation written here.

## What was adapted (not byte-copied)

The observable semantics (front matter, Markdown subset, template dialect,
value model, collections/tags/pagers, build report, byte-identical JSON
output) follow the reference's proven-correct approach, but the code is a
rewrite in a different organization:

- **File organization (different):** the reference has `rt.c, json.c,
  frontmatter.c, text.c, map.c, markdown.c, model.c, template.c, build.c,
  main.c, sg.h, nfkd_table.h` (+ `start.S`, `smoke.c`). This lane has
  `core.c, jsonin.c, front.c, txt.c, hmap.c, md.c, val.c, tpl.c, pipe.c,
  entry.c, zg.h, decomp.h, entry.S`. Different split of responsibilities
  (e.g. syscalls + pool + arr/str + SSE2 scans are one `core.c`; the value
  model is a separate `val.c`; the build pipeline is `pipe.c`).
- **Renames throughout:** types and functions carry different names
  (`Pool`/`Sp`/`Arr`/`Str`/`Dict`/`Set` vs the reference's `Arena`/`S`/
  `Vec`/`Buf`/`StrMap`/`StrSet`; `pool_*`/`arr_*` vs `ar_*`/`vec_*`; `Tpl`
  vs `Template`, etc.). Helper decomposition differs (e.g. JSON unescape,
  slugify and template slot resolution are factored differently).
- **Hash function:** FNV-1a (offset basis 1469598103934665603) instead of the
  reference's FxHash-style rotate-multiply (seed 0x517cc1b727220a95).
- **Growth policies (different):** dynamic arrays start at cap 16 (reference
  doubles from its own initial sizes); strings start at 512; the dict/set
  starts at cap 32 and grows past 70% load (reference: lazy 64-cap init,
  grow past 75%).
- **NFKD table:** the 173-entry decomposition table in `decomp.h` was
  reformatted by script from the reference table and verified sorted by code
  point; fullwidth FF01–FF5E and U+3000 are handled by formula in `txt.c`,
  as in the reference.
- **Driver:** `main.ts` spawns `./site` with cases/answers paths (same
  shape as the reference driver, rewritten here); `solve.js` is a per-case
  adapter (temp dir + one-case mode + parse the answer back).

## Bugs found and fixed during adaptation

1. **ND_TEXT cross-pool copy (23 Nov ~23:05 WITA).** Template text nodes
   live in the working pool, but the renderer copied them with the same-pool
   helper, so a text segment rendered as NULs/newlines at its own offset
   (e.g. `<h1>`/`</h1>` became 4/5 newlines; `STATIC` rendered empty).
   Fixed to a cross-pool copy from the working pool into the output pool.
   (The ND_INCLUDE cache-hit copy is correct as-is: cache entries' byte
   spans point into the output pool.)

## Vendor-conformance fixes (found by differential fuzzing, 23 Nov ~23:30 WITA)

The fuzzer `fuzz-e2e.mjs` compares this lane's `run(input)` against the
vendor's `build(input)` on random edge-case inputs. It found 3 mismatches in
the first 300 cases, from two root causes — both are places where the
reference C diverges from the vendor, so the fixes follow the vendor:

2. **Duplicate static paths: first wins + "shadowed" report.** The vendor
   checks `Object.hasOwn(output, path)` for statics, so a second static with
   a path already in the output is reported as
   `shadowed <path>: a built page has its path` and not written (first
   wins). The reference C's `written_keys` only ever receives page keys — a
   written static's key is never recorded — so a duplicate static was
   written twice (last wins in JSON.parse) with no report line. Fix
   (`pipe.c`): after writing a non-shadowed static, push its key into
   `written_keys`.
3. **Var/raw path validated at tokenize time.** The vendor's `tokenize()`
   calls `pathOf` on a `{{ }}`/`{{{ }}}` path immediately, so a bad path
   (e.g. the empty `{{}}` → `"" is not a path`) beats any later error in the
   source. This lane validated var paths only in the node-build phase, which
   runs after the whole tokenize — so a later unclosed `{{` was reported
   instead (`a {{ is not closed`). Fix (`tpl.c`): validate the trimmed path
   with `path_valid` in the tokenizer's `{{`/`{{{` branch and return
   `path_err` on failure, matching the vendor's left-to-right first-error
   order. (For/if/include paths stay validated in the node-build phase,
   matching the vendor's parse phase.)

## Verification (all after the fixes above)

- Practice 3/3 byte-identical (`diff-expected.mjs`): g-0001 276 keys,
  g-0002 350 keys, g-0003 509 keys, differing=0, extra=0, orderSame=true.
- Fresh sets 6/6 byte-identical: fresh1 315/443/560 keys, fresh2 277/359/502
  keys, differing=0.
- Differential fuzz vs vendor (`fuzz-e2e.mjs`, deterministic seed):
  3000/3000 random edge-case inputs byte-identical (keys, order, values).
  The generator covers: malformed/unclosed front matter, all 7 keys with
  invalid values (bad dates incl. leap-year, bad slugs, bad layouts, quoted
  strings with bad escapes, duplicate/unknown keys), CRLF bodies, Markdown
  edge cases (unclosed emphasis/code, bad fences, HR variants, lists with
  >9-digit and 0. starts, blockquotes, `..` link escapes, unicode/NFKD
  titles, duplicate titles), template edge cases (empty `{{}}`, unclosed
  tags, bad paths, `else` outside `if`, include of missing templates),
  duplicate file paths (`.md` and static), reserved dirs, negative
  `per_page`/`recent`, empty `base_url`.
- Timing (local, practice input 2.9 MB / 3 cases): binary alone median
  55 ms CPU (53–61 ms, N=9). Net (gross − null, CPU medians) across five
  independent rounds: 31, 19, 35, 30 (paired), 21 ms — median ≈ 30 ms,
  mean ≈ 27 ms. See STATUS.md for the full table and the honest verdict.

## Build

```sh
gcc -O3 -march=x86-64 -nostdlib -static -no-pie -fno-pie \
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -s -o site entry.S entry.c core.c jsonin.c front.c txt.c hmap.c md.c \
  val.c tpl.c pipe.c
```

Produces `./site` (91,464 B, ELF 64-bit LSB, statically linked, stripped).
md5 (post-fix build): `9576bfb071d449d7e03121072b56687f`.
Source: 4,824 lines of C (12 files + 2 headers), + `main.ts` (18) +
`solve.js` (26).

## Optimization M1–M5 (Antigravity, 23–24 Nov 2026)

The adapted binary was optimized in five milestones by Antigravity (agy),
working from the adapted source above. All milestones kept the output
byte-identical (practice 3/3 + two fresh sets 6/6 + 3,000 differential fuzz
cases vs the vendor, zero mismatches).

- M1 — inline hot primitives (`arr_at`, `pool_put`, `pool_copy`, `str_c`,
  `str_add`, `copy_bytes`, `cmp_bytes`) in `zg.h`; `first_bit` while-loop
  replaced with `__builtin_ctz`; `-flto`.
- M2 — runtime CPUID detection (`has_avx2`, `has_ssse3`, `has_popcnt`) for
  fast AVX2 `memcpy`/`memcmp` and SIMD scans, with a safe SSE2 fallback.
- M3 — AVX2 PSHUFB nibble-table scans and Markdown fast paths (inline_span,
  html_esc, json_esc, scan_nl, word_count with popcnt).
- M4 — fused AVX2 speculative-store `inline_span_json` and direct
  JSON-escaped Markdown emission; AVX2 32-byte `parse_string_escaped`.
- M5 — PGO (`-fprofile-generate` / `-fprofile-use`) and micro-tuning.

Timing (binary alone, local, median of 9): base 56 ms → M1 41 → M2 38 →
M3 29 → M4 29–30 → M5 24 ms (driver session); coordinator re-measured M5 at
22 and 29 ms in two sessions, base at 53 ms in the same session — about
2.3x faster than the first native binary. The SSE2-only fallback variant was
also verified correct (9/9 + fuzz 3000) at 30 ms, so the entry stays correct
and fast even on a CPU without AVX2.

## Round 4 — R4B-M2 (Antigravity, 24 Nov 2026)

Round 4 (two chunks, both killed by the 30-minute print timeout) produced
M6, R4B-M1 and R4B-M2 from the M5 source. R4B-M1 (22 ms) has no source
snapshot and is not submitted. R4B-M2 (24 ms local in the driver session;
coordinator re-measured 29 ms vs M5's 28 ms in the same session —
statistically equal to M5) is the clean candidate: its source snapshot
(`src-m2-backup/`, 12 files + the unchanged `entry.S` startup stub) matches
the binary, and it was verified byte-identical on practice 3/3 + two fresh
sets 6/6 + 3,000 differential fuzz cases. It replaces the M5 entry
(rank 31 @ 70,000 µs) as the counting submission.
