// site/test/builder-bridge.test.mjs
// The Config Builder's "What you're building" choice, in Chromium: the form
// markup comes from ConfigWizard.astro and the script is the real wizard.js.
// Choosing a network bridge swaps the board list, hides what a bridge has no
// use for, and writes a stream_server config with no transmit pin.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve, dirname, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { BOARDS } from '../boards.js';

const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const astro = readFileSync(resolve(SITE, 'src/components/ConfigWizard.astro'), 'utf8');
// The markup between the frontmatter and the component's own <script>.
const markup = astro.split(/^---$/m)[2].split('<script>')[0];
const page = `<!doctype html><meta charset="utf-8"><body>${markup}
<script type="module" src="/wizard.js"></script></body>`;

let server, port, browser;
before(async () => {
  server = createServer((req, res) => {
    const path = new URL(req.url, 'http://x').pathname;
    if (path === '/' || path === '/config-builder/') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      return res.end(page);
    }
    const file = normalize(resolve(SITE, '.' + path));
    if (!file.startsWith(SITE) || !['.js', '.mjs'].includes(extname(file))) { res.writeHead(404); return res.end(); }
    try {
      const body = readFileSync(file);
      res.writeHead(200, { 'Content-Type': 'text/javascript' });
      res.end(body);
    } catch { res.writeHead(404); res.end(); }
  }).listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => {
  await browser.close();
  server.close();
});

async function open(query = '') {
  const ctx = await browser.newContext();
  const p = await ctx.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(e.message));
  await p.goto(`http://127.0.0.1:${port}/config-builder/${query}`, { waitUntil: 'networkidle' });
  await p.waitForFunction(() => document.getElementById('out').textContent.length > 0);
  return { p, ctx, errors };
}
const options = (p) => p.$$eval('#board option', (os) => os.map((o) => o.value));
const out = (p) => p.textContent('#out');

test('the builder opens on a monitor, with every monitor board', async () => {
  const { p, ctx, errors } = await open();
  assert.equal(await p.isChecked('#build-monitor'), true);
  assert.deepEqual(await options(p), BOARDS.map((b) => b.id));
  assert.match(await out(p), /^tigo_monitor:/m);
  assert.equal(await p.isVisible('#cca-card'), true);
  assert.equal(await p.isVisible('#bridge-intro'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('choosing a network bridge writes a bridge, and choosing a monitor again undoes it', async () => {
  const { p, ctx, errors } = await open();
  await p.check('#build-bridge');
  assert.deepEqual(await options(p), ['atoms3-bridge', 't-can485-bridge']);
  for (const id of ['#cca-card', '#monitor-fields']) assert.equal(await p.isVisible(id), false, id);
  assert.equal(await p.isVisible('#bridge-intro'), true);
  assert.equal(await p.inputValue('#name'), 'tigo-bridge');
  assert.equal(await p.textContent('#out-name'), 'atoms3-bridge.yaml');
  assert.equal(new URL(p.url()).search, '?build=bridge');
  let y = await out(p);
  assert.match(y, /^stream_server:/m);
  assert.match(y, /pin: GPIO6\b/);
  assert.doesNotMatch(y, /^\s*tx_pin:|tigo_monitor/m);

  await p.selectOption('#board', 't-can485-bridge');
  y = await out(p);
  assert.match(y, /pin: GPIO22\b/);
  assert.match(y, /minimum_chip_revision: '3.0'/);

  await p.check('#build-monitor');
  assert.equal(await p.inputValue('#name'), 'tigo-server');
  assert.equal(await p.isVisible('#cca-card'), true);
  assert.equal(new URL(p.url()).search, '');
  assert.match(await out(p), /^tigo_monitor:/m);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('a link with ?build=bridge opens on the bridge', async () => {
  const { p, ctx, errors } = await open('?build=bridge');
  assert.equal(await p.isChecked('#build-bridge'), true);
  assert.match(await out(p), /^stream_server:/m);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('a wired monitor board, then a bridge, brings the Wi-Fi fields back', async () => {
  const { p, ctx } = await open();
  await p.selectOption('#board', 'esp32s3-lilygo-t-connect-pro-lite');
  assert.equal(await p.isVisible('#wifi-ssid-row'), false);
  await p.check('#build-bridge');
  assert.equal(await p.isVisible('#wifi-ssid-row'), true);
  assert.equal(await p.isVisible('#ethernet-note'), false);
  await p.fill('#wifi-ssid', 'cabinet-net');
  await p.uncheck('#use-secrets');
  assert.match(await out(p), /^  ssid: cabinet-net$/m);
  await ctx.close();
});
