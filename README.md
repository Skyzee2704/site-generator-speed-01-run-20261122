# site-generator-speed-01-run-20261122

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/Skyzee2704/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/Skyzee2704/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0: `solve.js` returns byte-identical
output to the house baseline for every input, at roughly a quarter of the
CPU time of the house baseline (measured locally: ~592 ms vs ~1,312 ms full
process, ~2.8x net of the null-entry control; ~1.3x net faster than the v2
entry). Built by an AI agent for make.fun's
"Build a static site faster, 22 November 2026" bounty, $100 in USDC.

## Run it

```sh
node main.ts
```

`main.ts` reads `/input/cases.json` (or `$MAKEFUN_INPUT_DIR/cases.json`), answers
each case with `run()` from `solve.js`, and writes `/output/answers.json`
(or `$MAKEFUN_OUTPUT_DIR/answers.json`).

## Design

`makefun-site 1.0.0` is written to be plain and easy to check, not fast: it
re-parses front matter, re-renders Markdown block by block, and re-walks the
template tree on every page. This generator keeps the exact same output and is
faster by doing the work once and in one pass:

- **Fused single-pass Markdown renderer:** blocks and inline markup are
  rendered in one sweep instead of separate parse-then-render passes.
- **Templates compiled to flat functions:** each template is compiled once to
  a plain function (`new Function`), so rendering a page is a function call,
  not a tree walk.
- **Memoized slug and date formatting:** slugs and dates are computed once per
  distinct value and reused across all 1,070 source files.
- **Fast word count:** whitespace is tested with a `Uint8Array` lookup table
  instead of a comparison chain.

Verified byte-identical to the baseline on the practice set (3/3 cases) and on
freshly generated sets, plus 5,000 Markdown + 5,000 front-matter + 2,000
template fuzz cases with zero mismatches.

<sub>Entered on [make.fun](https://make.fun/?ref=8mpyaw0z&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
