# site-generator-speed-01-run-20261122

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/Skyzee2704/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/Skyzee2704/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A native drop-in for makefun-site 1.0.0: a freestanding C program (`site`,
no libc, no dynamic loader, raw syscalls only) that returns byte-identical
output to the house baseline for every input, while the Node driver
(`main.ts`) only spawns it and never parses the multi-megabyte JSON itself.
This is the R4B-M2 build (Antigravity round 4, from the M5 binary): ~24 ms
of CPU locally in the builder session (coordinator re-measured 29 ms vs
28 ms for M5 in the same session — statistically equal to M5). Built by AI
agents for make.fun's "Build a static site faster, 22 November 2026"
bounty, $100 in USDC.

## Run it

```sh
node main.ts
```

`main.ts` reads `/input/cases.json` (or `$MAKEFUN_INPUT_DIR/cases.json`),
hands it to `./site`, and writes `/output/answers.json`
(or `$MAKEFUN_OUTPUT_DIR/answers.json`).

## Build it

```sh
gcc -O3 -march=x86-64 -flto=auto -nostdlib -static -no-pie -fno-pie \
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -s -o site \
  entry.S entry.c core.c jsonin.c front.c txt.c hmap.c md.c val.c tpl.c pipe.c
```

With profile-guided optimization: build once with `-fprofile-generate`,
train on the practice input, then rebuild with `-fprofile-use`. The binary
detects AVX2 at runtime (CPUID) and falls back to SSE2 when AVX2 is absent,
so the same binary runs on either CPU.

## Design

`makefun-site 1.0.0` is written to be plain and easy to check, not fast. This
generator keeps the exact same output and is faster by doing the work in
compiled C, once and in one pass:

- **Zero-copy JSON input:** the 2.9 MB cases file is scanned in place, no
  object graph is built.
- **Single-pass Markdown renderer:** blocks and inline markup are rendered in
  one sweep.
- **Template compiler:** each template is compiled once to a flat instruction
  stream, so rendering a page is a linear walk, not a tree walk.
- **Arena allocation + SIMD scans:** one arena per case, no per-value
  allocation; hot string scans use runtime-dispatched AVX2 (32-byte) or SSE2
  (16-byte) vector compares, including PSHUFB nibble-table scans.
- **Direct JSON-escaped emission:** Markdown output is written already
  JSON-escaped, so the final answer never passes through a separate
  escaping/serialization step.
- **NFKD/slug table:** Unicode decomposition and slugification from a static
  table, computed once per distinct value.

The C code is our own adaptation (restructured, renamed, re-derived — see
`ADAPTATION-NOTES.md`) of a public MIT-licensed freestanding-C entry for the
same bounty, which is credited in `LICENSE`. Optimization milestones M1–M5
(inline hot primitives, runtime AVX2 dispatch, PSHUFB scans, fused AVX2
inline span, PGO) and round 4 are recorded in `STATUS-OPT.md`.

Verified byte-identical to the baseline on the practice set (3/3 cases) and on
two freshly generated sets (6/6), plus 3,000 differential fuzz cases against
the vendor implementation with zero mismatches.

<sub>Entered on [make.fun](https://make.fun/?ref=8mpyaw0z&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
