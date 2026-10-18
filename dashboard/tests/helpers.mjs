// Shared by the core and UI tests.
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';

const require = createRequire(import.meta.url);
export const C = require('../src/core.js');
export const sha = (b) => createHash('sha256').update(b).digest('hex');

// Rebuilds a consistent chain from edited rows: what a full-file rewrite does.
export function rechain(rows) {
  let prev = C.GENESIS;
  const out = [];
  for (const row of rows) {
    const cols = row.split('\t');
    const rest = cols.slice(2).join('\t');
    const hash = sha(Buffer.from(prev + '\t' + rest));
    out.push([prev, hash, rest].join('\t'));
    prev = hash;
  }
  return out.join('\n') + '\n';
}
