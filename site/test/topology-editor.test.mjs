// site/test/topology-editor.test.mjs
// The Topology editor in the real app.html, in Chromium, against a mock API:
// /api/layout is an in-memory layout that records every POST. The rest of the
// API comes from the screenshot fixtures. Barcodes use the fixtures' fake
// 04C05B0000 prefix.
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

const P = ['04C05B0000300001', '04C05B0000300002', '04C05B0000300003'];

// Inverter 1 / MPPT 1 / A (P1, P2) and B (empty); P3 in no string.
function startLayout() {
  return {
    manual: false,
    inverters: [{
      name: 'Inverter 1', display_name: 'South Roof',
      mppts: [{ label: 'MPPT 1', strings: [
        { label: 'A', display_label: '', rating_w: 400, panels: [
          { barcode: P[0], addr: '0002', name: 'A1' }, { barcode: P[1], addr: '0003', name: 'A2' }] },
        { label: 'B', display_label: '', rating_w: 0, panels: [] },
      ] }],
    }],
    unplaced_mppts: [],
    unassigned: [{ barcode: P[2], addr: '0004', name: '' }],
    pending: null,
  };
}

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

// A page on the Topology view. api.posts collects POST bodies; api.saveStatus
// and api.saveError make the next layout save fail; api.power sets P1's watts.
async function open(opts = {}) {
  const touch = !!opts.touch;
  const api = { layout: startLayout(), posts: [], saveStatus: 200, saveError: '', power: 300,
    pending: null, actions: [] };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 }, hasTouch: touch });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/layout' && req.method() === 'POST') {
      api.posts.push(JSON.parse(req.postData()));
      return api.saveStatus === 200 ? json(200, { success: true })
        : json(api.saveStatus, { success: false, error: api.saveError });
    }
    if (path === '/api/layout/pending' && req.method() === 'POST') {
      api.actions.push(JSON.parse(req.postData()));
      api.pending = null;
      return json(200, { success: true });
    }
    if (path === '/api/layout/pending') return json(200, api.pending || { pending: null });
    if (path === '/api/layout') return json(200, { ...api.layout,
      pending: api.pending ? { source: api.pending.source, changes: api.pending.changes.length } : null });
    if (path === '/api/devices') {
      return json(200, { devices: [{ addr: '0002', barcode: P[0], name: 'A1', power_in: api.power, power: api.power,
        voltage_in: 38, current: 8, temperature: 40, data_age_ms: 1000, stale: false }], count: 1 });
    }
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  if (opts.pending) api.pending = opts.pending;
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/#topology`, { waitUntil: 'networkidle' });
  await page.waitForSelector('#topo-edit-btn');
  return { page, ctx, api, errors };
}

async function edit(page) {
  await page.click('#topo-edit-btn');
  await page.waitForSelector('#topo-unassigned');
}

const names = (page, label) => page.$$eval(
  `[data-string-label="${label}"] .topo-ed-panel`, (els) => els.map((e) => e.querySelector('.panel-tile-id').textContent.trim()));
const barcodes = (page, label) => page.$$eval(
  `[data-string-label="${label}"] .topo-ed-panel`, (els) => els.map((e) => e.dataset.barcode));

test('Edit layout shows the tray and add buttons; normal view has neither', async () => {
  const { page, ctx, errors } = await open();
  assert.equal(await page.$('#topo-unassigned'), null);
  assert.equal(await page.$('.topo-add-string'), null);
  await edit(page);
  assert.ok(await page.$('#topo-unassigned [data-barcode="' + P[2] + '"]'));
  assert.ok(await page.$('.topo-add-string'));
  assert.ok(await page.$('.topo-add-mppt'));
  assert.ok(await page.$('.topo-add-inverter'));
  assert.ok(await page.isVisible('#topo-save-btn'));
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('outside edit mode no card is draggable', async () => {
  const { page, ctx } = await open();
  assert.equal(await page.$('.topo-ed-panel'), null);
  const before = await page.locator('#topo-tree').innerHTML();
  const tile = page.locator('#topo-tree .panel-tile').first();
  if (await tile.count()) {
    await tile.hover();
    await page.mouse.down();
    await page.mouse.move(600, 700, { steps: 5 });
    await page.mouse.up();
  }
  assert.equal(await page.$('.topo-ed-panel'), null);
  assert.equal(await page.locator('#topo-tree').innerHTML(), before);
  await ctx.close();
});

test('dragging a tray panel into string A appends it and names it A3', async () => {
  const { page, ctx } = await open();
  await edit(page);
  // The empty end of the row, past A's two tiles: dropping on a tile puts it before that one.
  const grid = await page.locator('[data-string-label="A"] .panel-grid').boundingBox();
  await page.locator(`#topo-unassigned [data-barcode="${P[2]}"]`)
    .dragTo(page.locator('[data-string-label="A"] .panel-grid'), { targetPosition: { x: grid.width - 10, y: 10 } });
  assert.deepEqual(await barcodes(page, 'A'), [P[0], P[1], P[2]]);
  assert.deepEqual(await names(page, 'A'), ['A1', 'A2', 'A3']);
  assert.equal(await page.$(`#topo-unassigned [data-barcode="${P[2]}"]`), null);
  await ctx.close();
});

test('dropping on a panel puts the dragged one before it and renumbers', async () => {
  const { page, ctx } = await open();
  await edit(page);
  await page.locator(`[data-barcode="${P[1]}"]`).dragTo(page.locator(`[data-barcode="${P[0]}"]`));
  assert.deepEqual(await barcodes(page, 'A'), [P[1], P[0]]);
  assert.deepEqual(await names(page, 'A'), ['A1', 'A2']);
  await ctx.close();
});

test('picking a panel up and dropping it on itself leaves the string as it was', async () => {
  const { page, ctx } = await open();
  await edit(page);
  const box = await page.locator(`[data-barcode="${P[0]}"]`).boundingBox();
  await page.mouse.move(box.x + 20, box.y + 20);
  await page.mouse.down();
  await page.mouse.move(box.x + 30, box.y + 24, { steps: 3 });
  await page.mouse.up();
  assert.deepEqual(await barcodes(page, 'A'), [P[0], P[1]]);
  assert.deepEqual(await names(page, 'A'), ['A1', 'A2']);
  await ctx.close();
});

test('Save posts the tree with panels as barcodes in order', async () => {
  const { page, ctx, api } = await open();
  await edit(page);
  await page.locator(`[data-barcode="${P[0]}"]`).dragTo(page.locator('[data-string-label="B"] .panel-grid'));
  await page.click('#topo-save-btn');
  await page.waitForFunction(() => !document.getElementById('topo-unassigned'));
  assert.deepEqual(api.posts, [{ inverters: [{
    name: 'Inverter 1', display_name: 'South Roof',
    mppts: [{ label: 'MPPT 1', strings: [
      { label: 'A', display_label: '', rating_w: 400, panels: [P[1]] },
      { label: 'B', display_label: '', rating_w: 0, panels: [P[0]] },
    ] }],
  }] }]);
  await ctx.close();
});

test('Cancel discards a drag', async () => {
  const { page, ctx, api } = await open();
  await edit(page);
  await page.locator(`[data-barcode="${P[0]}"]`).dragTo(page.locator('[data-string-label="B"] .panel-grid'));
  await page.click('#topo-cancel-btn');
  await page.waitForFunction(() => !document.getElementById('topo-unassigned'));
  assert.deepEqual(api.posts, []);
  await edit(page);
  assert.deepEqual(await barcodes(page, 'A'), [P[0], P[1]]);
  assert.deepEqual(await barcodes(page, 'B'), []);
  await ctx.close();
});

test('+ String adds the next free letter; + MPPT the next MPPT n; + Inverter "Inverter n"', async () => {
  const { page, ctx, api } = await open();
  await edit(page);
  await page.click('.topo-add-string');
  assert.ok(await page.$('[data-string-label="C"]'));
  await page.click('.topo-add-mppt');
  assert.ok(await page.$('[data-mppt-label="MPPT 2"]'));
  await page.click('.topo-add-inverter');
  assert.ok(await page.$('[data-inv-name="Inverter 2"]'));
  await page.click('#topo-save-btn');
  await page.waitForFunction(() => !document.getElementById('topo-unassigned'));
  const sent = api.posts[0].inverters;
  assert.deepEqual(sent.map((i) => i.name), ['Inverter 1', 'Inverter 2']);
  assert.deepEqual(sent[0].mppts.map((m) => m.label), ['MPPT 1', 'MPPT 2']);
  assert.deepEqual(sent[0].mppts[0].strings.map((s) => s.label), ['A', 'B', 'C']);
  await ctx.close();
});

test('renaming a string renames its panels', async () => {
  const { page, ctx } = await open();
  await edit(page);
  await page.fill('[data-string-label="A"] .topo-ed-string-name', 'East');
  assert.deepEqual(await names(page, 'A'), ['East1', 'East2']);
  await ctx.close();
});

test('an empty string, MPPT or inverter can be deleted; one with panels has no delete button', async () => {
  const { page, ctx } = await open();
  await edit(page);
  assert.equal(await page.$('[data-string-label="A"] > .topo-string-head .topo-del'), null);
  assert.equal(await page.$('[data-mppt-label="MPPT 1"] > .topo-mppt-head .topo-del'), null);
  await page.click('[data-string-label="B"] > .topo-string-head .topo-del');
  assert.equal(await page.$('[data-string-label="B"]'), null);
  await page.click('.topo-add-inverter');
  await page.click('[data-inv-name="Inverter 2"] .topo-inv-head .topo-del');
  assert.equal(await page.$('[data-inv-name="Inverter 2"]'), null);
  await ctx.close();
});

test('a 400 from Save shows its error and stays in edit mode', async () => {
  const { page, ctx, api } = await open();
  api.saveStatus = 400;
  api.saveError = 'Panel 04C05B0000300001 is in the layout twice.';
  await edit(page);
  await page.click('#topo-save-btn');
  await page.waitForSelector('#topo-ed-error:not(:empty)');
  assert.match(await page.textContent('#topo-ed-error'), /in the layout twice/);
  assert.ok(await page.$('#topo-unassigned'));
  await ctx.close();
});

test('live readings keep updating on cards in edit mode without undoing a drag', async () => {
  const { page, ctx, api } = await open();
  await edit(page);
  await page.locator(`[data-barcode="${P[2]}"]`).dragTo(page.locator('[data-string-label="B"] .panel-grid'));
  api.power = 512;
  await page.evaluate(() => refreshTopology());
  await page.waitForFunction((bc) => document.querySelector(`[data-barcode="${bc}"] .panel-tile-power`)
    .textContent.includes('512'), P[0]);
  assert.deepEqual(await barcodes(page, 'B'), [P[2]]);
  await ctx.close();
});

test('leaving the view with unsaved edits asks first', async () => {
  const { page, ctx } = await open();
  await edit(page);
  await page.locator(`[data-barcode="${P[0]}"]`).dragTo(page.locator('[data-string-label="B"] .panel-grid'));
  let asked = 0;
  page.once('dialog', (d) => { asked++; d.dismiss(); });
  await page.click('button[data-view="dashboard"]');
  assert.equal(asked, 1);
  assert.ok(await page.isVisible('#view-topology'));
  assert.ok(await page.$('#topo-unassigned'));
  await ctx.close();
});

// Touch: synthetic TouchEvents, as a finger would send them.
async function touchDrag(page, fromSel, toSel, holdMs) {
  const from = await page.locator(fromSel).boundingBox();
  const to = await page.locator(toSel).boundingBox();
  await page.evaluate(async ({ from, to, holdMs, fromSel }) => {
    const el = document.querySelector(fromSel);
    const t = (x, y) => new Touch({ identifier: 1, target: el, clientX: x, clientY: y });
    const fire = (type, x, y) => el.dispatchEvent(new TouchEvent(type, {
      bubbles: true, cancelable: true, touches: type === 'touchend' ? [] : [t(x, y)],
      targetTouches: type === 'touchend' ? [] : [t(x, y)], changedTouches: [t(x, y)] }));
    const fx = from.x + from.width / 2, fy = from.y + from.height / 2;
    const tx = to.x + 5, ty = to.y + 5;
    fire('touchstart', fx, fy);
    await new Promise((r) => setTimeout(r, holdMs));
    for (let i = 1; i <= 5; i++) fire('touchmove', fx + (tx - fx) * i / 5, fy + (ty - fy) * i / 5);
    fire('touchend', tx, ty);
  }, { from, to, holdMs, fromSel });
}

test('touch: long-press then move drops the panel; a quick swipe on a card does not', async () => {
  const { page, ctx } = await open({ touch: true });
  await edit(page);
  await touchDrag(page, `[data-barcode="${P[2]}"]`, '[data-string-label="B"] .panel-grid', 50);
  assert.deepEqual(await barcodes(page, 'B'), []);
  await touchDrag(page, `[data-barcode="${P[2]}"]`, '[data-string-label="B"] .panel-grid', 400);
  assert.deepEqual(await barcodes(page, 'B'), [P[2]]);
  await ctx.close();
});

// ---- The banner: a layout that arrived after the user's own and waits.
function held(source) {
  return {
    source,
    changes: [{ barcode: P[0], now: { name: 'A1', string: 'A', mppt: 'MPPT 1' },
                then: { name: 'P0', string: 'East', mppt: 'MPPT 2' } }],
    inverters_now: null, inverters_then: null,
  };
}

test('no banner when nothing waits', async () => {
  const { page, ctx } = await open();
  await page.waitForTimeout(300);
  assert.equal(await page.isVisible('#topo-pending'), false);
  await ctx.close();
});

test('the banner names where the layout came from', async () => {
  for (const [source, words] of [['cca', 'The CCA'], ['cloud', 'Tigo cloud'], ['import', 'imported file'], ['yaml', 'YAML']]) {
    const { page, ctx } = await open({ pending: held(source) });
    await page.waitForSelector('#topo-pending', { state: 'visible' });
    assert.match(await page.textContent('#topo-pending'), new RegExp(words), source);
    await ctx.close();
  }
});

test('Review lists what would change', async () => {
  const { page, ctx } = await open({ pending: held('cca') });
  await page.waitForSelector('#topo-pending', { state: 'visible' });
  await page.click('#topo-pending-review');
  const text = await page.textContent('#topo-pending-list');
  assert.match(text, /A1/);
  assert.match(text, /East \/ MPPT 2/);
  assert.match(text, /P0/);
  await ctx.close();
});

test('Keep mine and Use theirs send the answer and clear the banner', async () => {
  for (const [btn, action] of [['#topo-pending-keep', 'keep'], ['#topo-pending-use', 'use']]) {
    const { page, ctx, api } = await open({ pending: held('cca') });
    await page.waitForSelector('#topo-pending', { state: 'visible' });
    await page.click(btn);
    await page.waitForSelector('#topo-pending', { state: 'hidden' });
    assert.deepEqual(api.actions, [{ action }]);
    await ctx.close();
  }
});

test('Use theirs waits while the editor has unsaved changes', async () => {
  const { page, ctx } = await open({ pending: held('cca') });
  await edit(page);
  await page.waitForSelector('#topo-pending', { state: 'visible' });
  assert.equal(await page.isDisabled('#topo-pending-use'), false);
  await page.locator(`[data-barcode="${P[0]}"]`).dragTo(page.locator('[data-string-label="B"] .panel-grid'));
  assert.equal(await page.isDisabled('#topo-pending-use'), true);
  await ctx.close();
});
