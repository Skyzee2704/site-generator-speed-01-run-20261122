// solve.js — per-case adapter for the local grade harness. run(input)
// serializes one case to a temp dir, runs the native `site` binary in
// one-case mode, and parses the single answer object back. The binary
// does all parsing, rendering and emitting; Node only shuttles bytes.
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const site = join(here, 'site');

export function run(input) {
  const dir = mkdtempSync(join(tmpdir(), 'zg-'));
  try {
    const inPath = join(dir, 'in.json');
    const outPath = join(dir, 'out.json');
    writeFileSync(inPath, JSON.stringify(input));
    const r = spawnSync(site, [inPath, outPath], { encoding: 'utf8' });
    if (r.status !== 0) throw new Error('site failed: ' + (r.stderr || r.status));
    return JSON.parse(readFileSync(outPath, 'utf8'));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}
