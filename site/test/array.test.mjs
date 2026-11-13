// site/test/array.test.mjs
// Topology > Array on the Raspberry Pi build, in the real app.html, in
// Chromium, against a mock API: an empty array says what to do, a saved one
// draws each panel in place by its power and each TAP, and Edit array places
// panels from a tray by dragging (mouse) or pressing, moves, turns and
// returns them with the keyboard, swaps two panels on a drop, and saves.
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

let server, port, browser;
before(async () => {
  server = createServer((_q, res) => {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
  }).listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => {
  await browser.close();
  server.close();
});

// Three panels and a TAP: A1 at full power, A2 at a quarter, B1 not heard lately.
const DEVICES = [
  { addr: '0002', name: 'A1', string_label: 'String A', power: 400, peak_power: 400, stale: false, tap: '1201' },
  { addr: '0003', name: 'A2', string_label: 'String A', power: 100, peak_power: 380, stale: false, tap: '1201' },
  { addr: '0004', name: 'B1', string_label: 'String B', power: 0, peak_power: 300, stale: true, tap: '1201' },
];

async function open(saved = { panels: [], taps: [] }, platform = 'linux') {
  const api = { layout: JSON.parse(JSON.stringify(saved)), posts: [] };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json(200, { ...routes['/api/status'], ...(platform === 'linux' ? { platform: 'linux' } : {}) });
    if (path === '/api/devices') return json(200, { devices: DEVICES, count: DEVICES.length });
    if (path === '/api/array') {
      if (req.method() === 'POST') { const b = JSON.parse(req.postData()); api.posts.push(b); api.layout = b; return json(200, { saved: true }); }
      return json(200, api.layout);
    }
    if (path === '/api/mesh') return json(200, { taps: [{ id: '1201', name: 'Garage' }], nodes: [], loss_days: [] });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/#topology`, { waitUntil: 'networkidle' });
  if (platform === 'linux') {
    await page.click('#topology-tab-array');
    await page.waitForSelector('#topology-array:not([hidden])');
  }
  return { page, ctx, api, errors };
}
const tile = (page, addr) => page.locator(`#array-canvas [data-kind="panel"][data-id="${addr}"]`);
const chip = (page, addr) => page.locator(`#array-tray [data-kind="panel"][data-id="${addr}"]`);

test('the ESP32 has no Array tab', async () => {
  const { page, ctx } = await open(undefined, 'esp32');
  await page.waitForTimeout(400);
  assert.equal(await page.isVisible('#topology-tab-array'), false);
  await ctx.close();
});

test('no array yet: it says how to start', async () => {
  const { page, ctx, errors } = await open();
  await page.waitForSelector('#array-canvas .array-empty');
  assert.match(await page.textContent('#array-canvas'), /No array drawn yet/);
  assert.equal(await page.isVisible('#array-tray'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('a saved array: each panel in place, lit by its power, and the TAP', async () => {
  const { page, ctx, errors } = await open({
    panels: [{ addr: '0002', x: 5, y: 3, portrait: true }, { addr: '0003', x: 6, y: 3, portrait: false }, { addr: '0004', x: 5, y: 4, portrait: true }],
    taps: [{ id: '1201', x: 8, y: 4 }],
  });
  await page.waitForSelector('#array-canvas .array-tile');
  const box = async (addr) => (await tile(page, addr).boundingBox());
  const a1 = await box('0002'), a2 = await box('0003'), b1 = await box('0004');
  assert.ok(a2.x > a1.x && Math.abs(a2.y + a2.height / 2 - (a1.y + a1.height / 2)) < 2, 'A2 to the right of A1, same row');
  assert.ok(b1.y > a1.y && Math.abs(b1.x - a1.x) < 2, 'B1 below A1');
  assert.ok(a1.height > a1.width && a2.width > a2.height, 'portrait and landscape');
  assert.match(await tile(page, '0002').getAttribute('class'), /\blit\b/);
  assert.doesNotMatch(await tile(page, '0003').getAttribute('class'), /\blit\b/);
  assert.match(await tile(page, '0004').getAttribute('class'), /\bstale\b/);
  assert.match(await tile(page, '0002').textContent(), /400 W/);
  assert.match(await page.getAttribute('#array-canvas .array-tap', 'aria-label'), /Garage/);
  assert.match(await page.textContent('#array-summary'), /3 of 3 panels placed · 1 TAP/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('editing: drag from the tray, press to place, keys to move, turn and return, swap on a drop, save', async () => {
  const { page, ctx, api, errors } = await open();
  await page.click('#array-edit-btn');
  await page.waitForSelector('#array-tray .array-chip');
  assert.equal(await page.locator('#array-tray [data-kind="tap"]').count(), 1);
  // Drag A1 to the cell at column 4, row 2.
  const c = await page.locator('#array-canvas').boundingBox();
  const cell = c.width / 16;
  const from = await chip(page, '0002').boundingBox();
  await page.mouse.move(from.x + from.width / 2, from.y + from.height / 2);
  await page.mouse.down();
  await page.mouse.move(c.x + 4.5 * cell, c.y + 2.5 * cell, { steps: 8 });
  await page.mouse.up();
  await tile(page, '0002').waitFor();
  // A press on A2's chip places it in the first free cell, top left.
  await chip(page, '0003').click();
  await tile(page, '0003').waitFor();
  // Keys: A2 moves right, turns, then A1 goes back to the tray.
  await tile(page, '0003').focus();
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('r');
  await tile(page, '0002').focus();
  await page.keyboard.press('Delete');
  assert.equal(await tile(page, '0002').count(), 0);
  assert.equal(await chip(page, '0002').count(), 1);
  // B1 pressed in lands in the first free cell, (0,0); dragged onto A2 at (1,0), the two swap.
  await chip(page, '0004').click();
  const b1 = await tile(page, '0004').boundingBox(), a2 = await tile(page, '0003').boundingBox();
  await page.mouse.move(b1.x + b1.width / 2, b1.y + b1.height / 2);
  await page.mouse.down();
  await page.mouse.move(a2.x + a2.width / 2, a2.y + a2.height / 2, { steps: 8 });
  await page.mouse.up();
  await page.click('#array-save-btn');
  await page.waitForFunction(() => document.getElementById('array-edit-btn').style.display !== 'none');
  const saved = api.posts.at(-1);
  const at = Object.fromEntries(saved.panels.map(p => [p.addr, [p.x, p.y, p.portrait]]));
  // The drawing is kept anchored at its top left: A1 dropped alone sat at
  // 0,0; with A1 gone, A2 shifted back to 0,0; B1 came in at 1,0 and swapped.
  assert.deepEqual(at, { '0003': [1, 0, false], '0004': [0, 0, true] }, 'A2 and B1 swapped; A1 back off the array');
  assert.deepEqual(saved.taps, []);
  // Saved: the array is drawn, the tray gone.
  assert.equal(await page.isVisible('#array-tray'), false);
  assert.equal(await page.locator('#array-canvas .array-tile').count(), 2);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('a TAP can share a panel\'s square, drawn small in its corner', async () => {
  const { page, ctx, api, errors } = await open();
  await page.click('#array-edit-btn');
  await page.waitForSelector('#array-tray .array-chip');
  // A1 pressed in lands at 0,0; the TAP pressed in lands at the first square
  // without a TAP: the same one, under A1.
  await page.locator('#array-tray [data-kind="panel"][data-id="0002"]').click();
  await page.locator('#array-tray [data-kind="tap"]').click();
  const tap = page.locator('#array-canvas [data-kind="tap"]');
  await tap.waitFor();
  assert.match(await tap.getAttribute('class'), /\bunder\b/);
  const t = await tap.boundingBox(), p = await page.locator('#array-canvas [data-kind="panel"][data-id="0002"]').boundingBox();
  assert.ok(t.width < p.width && t.height < p.height, 'smaller than the panel');
  await page.click('#array-save-btn');
  await page.waitForFunction(() => document.getElementById('array-edit-btn').style.display !== 'none');
  assert.deepEqual(api.posts.at(-1).taps, [{ id: '1201', x: 0, y: 0 }]);
  assert.deepEqual(api.posts.at(-1).panels, [{ addr: '0002', x: 0, y: 0, portrait: true }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the grid grows in every direction: a drop above or left of everything shifts the drawing', async () => {
  const { page, ctx, api, errors } = await open({ panels: [{ addr: '0002', x: 0, y: 0, portrait: true }], taps: [] });
  await page.click('#array-edit-btn');
  await page.waitForSelector('#array-tray .array-chip');
  // Three empty squares above and to the left of A1 while editing.
  const a1 = await page.locator('#array-canvas [data-kind="panel"][data-id="0002"]').boundingBox();
  const c = await page.locator('#array-canvas').boundingBox();
  const cell = (a1.x + a1.width / 2 - c.x) / 3.5;
  assert.ok(Math.abs((a1.y + a1.height / 2 - c.y) / cell - 3.5) < 0.2, 'three squares above it');
  // Drop A2 two squares left of A1 and one above.
  const from = await page.locator('#array-tray [data-kind="panel"][data-id="0003"]').boundingBox();
  await page.mouse.move(from.x + from.width / 2, from.y + from.height / 2);
  await page.mouse.down();
  await page.mouse.move(a1.x + a1.width / 2 - 2 * cell, a1.y + a1.height / 2 - cell, { steps: 8 });
  await page.mouse.up();
  // Keys reach past the edge too: A1 one square further down.
  await page.locator('#array-canvas [data-kind="panel"][data-id="0002"]').focus();
  await page.keyboard.press('ArrowDown');
  await page.click('#array-save-btn');
  await page.waitForFunction(() => document.getElementById('array-edit-btn').style.display !== 'none');
  const at = Object.fromEntries(api.posts.at(-1).panels.map(p => [p.addr, [p.x, p.y]]));
  assert.deepEqual(at, { '0002': [2, 2], '0003': [0, 0] });
  assert.deepEqual(errors, []);
  await ctx.close();
});
