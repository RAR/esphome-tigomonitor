// site/test/no-pi-in-copy.test.mjs
// TigoMonitor runs on any Linux machine, not only a Raspberry Pi, so the
// page names itself, never "the Pi". "Raspberry Pi" stays where the
// hardware is meant (its UART). Comments are left out of the check.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(resolve(HERE, '../../components/tigo_server/web/app.html'), 'utf8');

test('the page never calls itself "the Pi"', () => {
  // Comments: HTML ones, lines that are a // comment, and a trailing
  // " // " one. (Block comments are only used at line starts here.)
  const copy = html
    .replace(/<!--[\s\S]*?-->/g, (c) => c.replace(/[^\n]/g, ''))
    .split('\n').map((l) => (/^\s*(\/\/|\*|\/\*)/.test(l) ? '' : l.replace(/\s\/\/\s.*$/, ''))).join('\n');
  const hits = copy.split('\n').map((l, i) => [i + 1, l]).filter(([, l]) => /(?<!Raspberry )\bPi\b/.test(l));
  assert.equal(hits.length, 0, hits.map(([n, l]) => `\n  ${n}: ${l.trim().slice(0, 100)}`).join(''));
});

test('the TAP-changes switch is named as Settings names it', () => {
  assert.doesNotMatch(html, /Let me change the TAP/);
});
