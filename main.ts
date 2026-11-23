// main.ts — the house entry point. The harness runs `node /bot/main.ts`
// (node22). All of the work happens in the native `site` binary: this
// driver only hands it the cases file and the answers path, so Node pays
// one spawn (~2ms) and never parses the multi-megabyte JSON itself.
// Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in
// for /input and /output.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const r = spawnSync(join(here, 'site'),
  [join(INPUT_DIR, 'cases.json'), join(OUTPUT_DIR, 'answers.json')],
  { stdio: 'inherit' });
if (r.status !== 0) process.exit(r.status ?? 1);
