// site/test/new-optimizer.test.mjs
// A new optimizer (a replacement, or one added) and replacing a panel with it,
// in the real app.html, in Chromium, against the screenshot fixtures plus one
// unassigned panel first heard an hour ago. A2's optimizer is the dead one: it
// has no reading.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { routes } from '../screenshots/fixtures.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(resolve(HERE, '../../components/tigo_server/web/app.html'), 'utf8')
  .replaceAll('__TIGO_API_TOKEN__', '');
const NEW_BC = '04C05B0000200001';
const A2 = '04C05B0000101337';

let server, port, browser;
before(async () => {
  server = createServer((_q, res) => { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); res.end(html); })
    .listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => { await browser.close(); server.close(); });

// heard: the new panel's first_heard_unix (0: one the monitor started with).
// mutate(nodes): change /api/nodes' nodes before the page sees them.
async function open(hash, heard = Math.floor(Date.now() / 1000) - 3600, mutate = null) {
  const clone = (o) => JSON.parse(JSON.stringify(o));
  const layout = clone(routes['/api/layout']);
  layout.unassigned = [{ barcode: NEW_BC, addr: '0080', name: '', first_heard_unix: heard }];
  const nodes = clone(routes['/api/nodes']);
  nodes.nodes.push({ addr: '0080', first_heard_unix: heard, long_address: NEW_BC, sensor_index: 24, checksum: '',
    cca_validated: false, cca_label: '', cca_string: '', cca_mppt: '', cca_channel: '' });
  if (mutate) mutate(nodes.nodes);
  const devices = clone(routes['/api/devices']);
  devices.devices = devices.devices.filter((d) => d.barcode !== A2);
  const api = { posts: [] };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 1400 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/layout' && req.method() === 'POST') { api.posts.push(JSON.parse(req.postData())); return json(200, { saved: true }); }
    if (path === '/api/layout') return json(200, layout);
    if (path === '/api/nodes') return json(200, nodes);
    if (path === '/api/devices') return json(200, devices);
    if (path === '/api/layout/pending') return json(200, null);
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/${hash}`, { waitUntil: 'networkidle' });
  return { page, ctx, api, errors };
}

test('the Node Table tags a new optimizer, and only it', async () => {
  const { page, ctx, errors } = await open('#nodes');
  await page.waitForFunction(() => /0080/.test(document.getElementById('nodes-tbody').textContent));
  assert.equal(await page.locator('#nodes-tbody .node-new').count(), 1);
  assert.match(await page.locator('#nodes-tbody tr', { has: page.locator('.node-new') }).textContent(), /0080/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the Topology page says a new optimizer was heard and where to place it', async () => {
  const { page, ctx } = await open('#topology');
  await page.waitForSelector('#topo-new', { state: 'visible' });
  const text = await page.textContent('#topo-new');
  assert.match(text, /New optimizer heard/);
  assert.match(text, /…200001/);
  assert.match(text, /Edit layout/);
  await ctx.close();
});

test('one the monitor started with is not news', async () => {
  const { page, ctx } = await open('#topology', 0);
  await page.waitForTimeout(800);
  assert.equal(await page.isVisible('#topo-new'), false);
  await ctx.close();
});

test('a week on it is not news either', async () => {
  const { page, ctx } = await open('#topology', Math.floor(Date.now() / 1000) - 8 * 86400);
  await page.waitForTimeout(800);
  assert.equal(await page.isVisible('#topo-new'), false);
  await ctx.close();
});

test('Replace puts the new panel in the dead one\'s place, and the dead one in Unassigned', async () => {
  const { page, ctx, api, errors } = await open('#topology');
  await page.click('#topo-edit-btn');
  const tile = page.locator(`#topo-unassigned .topo-ed-panel[data-barcode="${NEW_BC}"]`);
  await tile.waitFor();
  assert.equal(await tile.locator('.node-new').count(), 1);
  await tile.locator('.topo-ed-replace').click();
  const pick = tile.locator('select.topo-ed-replace-pick');
  await pick.waitFor();
  // The ones with no reading come first.
  const first = await pick.locator('option').nth(1).textContent();
  assert.match(first, /^String A2 · no reading$/);
  await pick.selectOption({ value: A2 });
  const a = page.locator('.topo-string[data-string-label="String A"] .topo-ed-panel');
  assert.equal(await a.nth(1).getAttribute('data-barcode'), NEW_BC);
  assert.equal(await page.locator(`#topo-unassigned .topo-ed-panel[data-barcode="${A2}"]`).count(), 1);
  await page.click('#topo-save-btn');
  await page.waitForFunction(() => true);
  await page.waitForTimeout(400);
  const s = api.posts[0].inverters[0].mppts[0].strings[0];
  assert.equal(s.panels[1], NEW_BC);
  assert.equal(s.panels.includes(A2), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the Node Table tags a missing optimizer, one off the TAP\'s roster, and one not set up', async () => {
  const now = Math.floor(Date.now() / 1000);
  const { page, ctx, errors } = await open('#nodes', 0, (nodes) => {
    for (const n of nodes) { n.last_heard_unix = now - 60; n.last_roster_unix = now - 600; }
    const by = (name) => nodes.find((n) => n.cca_label === name);
    by('A3').last_heard_unix = now - 4 * 86400;                  // no reading for 4 days
    by('B4').last_roster_unix = now - 2 * 86400;                 // the roster stopped listing it
    by('C5').setup = { info: '!Info 0027 12 0000 0DC1 00 0000 0000 00 00 0000 0FFF 1F9 B',
                       mppt: '!Mppt 1 0 N M[100 0000 - 0/3] 0 12 40 08 12' };
    by('C6').setup = { info: '!Info 0027 12 0000 0DC1 00 0000 0000 FF 00 0000 0FFF 1F9 B',
                       mppt: '!Mppt 1 0 S M[100 0000 - 3/3] 0 12 40 08 12' };
  });
  await page.waitForFunction(() => /A3/.test(document.getElementById('nodes-tbody').textContent));
  const row = (name) => page.locator('#nodes-tbody tr', { has: page.locator('strong', { hasText: new RegExp('^' + name + '$') }) });
  assert.equal(await page.locator('#nodes-tbody .node-missing').count(), 2);
  assert.match(await row('A3').locator('.node-missing').textContent(), /^Missing$/);
  assert.match(await row('A3').locator('.node-missing').getAttribute('title'), /no reading since/);
  assert.match(await row('B4').locator('.node-missing').textContent(), /^Not on the TAP$/);
  assert.equal(await page.locator('#nodes-tbody .node-unset').count(), 1);
  assert.match(await row('C5').locator('.node-unset').textContent(), /^Not set up$/);
  assert.match(await row('C5').locator('.node-unset').getAttribute('title'), /!Mppt 1 0 N/);
  assert.equal(await row('C6').locator('.node-unset').count(), 0);
  assert.match(await row('C6').locator('strong').first().getAttribute('title'), /!Mppt 1 0 S/);
  assert.deepEqual(errors, []);
  await ctx.close();
});
