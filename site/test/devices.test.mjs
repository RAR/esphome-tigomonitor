// site/test/devices.test.mjs
// The Devices page (once the Node Table): on the Raspberry Pi build, the TAPs
// and each optimizer's firmware from /api/versions; on the ESP32, neither.
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
  server = createServer((_q, res) => { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); res.end(html); })
    .listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => { await browser.close(); server.close(); });

async function open(linux, versions = routes['/api/versions']) {
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 1200 } });
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json(200, linux ? { ...routes['/api/status'], platform: 'linux' } : routes['/api/status']);
    if (path === '/api/versions') return linux ? json(200, versions) : route.fulfill({ status: 404, body: 'no' });
    if (path === '/api/master') return json(200, { role: 'listen', active: false });
    if (path === '/api/bus') return json(200, { alarm: { active: false }, stats: {}, exchanges: [] });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/#nodes`, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => document.querySelectorAll('#nodes-tbody tr').length > 5);
  return { page, ctx, errors };
}

test('Devices, on the Pi: the TAP, and each optimizer with its firmware', async () => {
  const { page, ctx, errors } = await open(true);
  assert.equal((await page.textContent('#nav button[data-view="nodes"]')).trim().replace(/^\S+\s*/, ''), 'Devices');
  assert.equal(await page.textContent('#view-nodes h1'), 'Devices');
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  const tap = await page.textContent('#devices-taps tbody');
  assert.match(tap, /1201.*Mgate.*G8\.59 Jan {2}1 2026/s);
  await page.waitForFunction(() => /K8\.0120/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const row = (addr) => page.$eval(`#nodes-tbody tr[data-addr="${addr}"]`, (tr) => tr.textContent);
  assert.match(await row(dev[0].addr), /K8\.0120 \(2D\)/);
  // An update shows what it replaced.
  assert.match(await row(dev[4].addr), /K8\.0130 \(2D\)/);
  assert.match(await page.$eval(`#nodes-tbody tr[data-addr="${dev[4].addr}"] .fw`, (td) => td.title), /was K8\.0120 \(2D\)/);
  assert.equal(await page.isVisible('table.nodes thead th.fw'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test("Devices, on the Pi: each optimizer's settings, stored or not, and the odd ones out", async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForFunction(() => /Not stored/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const cell = (addr) => page.$eval(`#nodes-tbody tr[data-addr="${addr}"] td.smart`, (td) => [td.textContent.trim(), td.title]);
  assert.deepEqual((await cell(dev[0].addr))[0], 'Stored');
  // Compared only with the same firmware family, and not on the stored flag,
  // which the cell already shows: the two H9s agree with each other.
  const [n5, t5] = await cell(dev[5].addr);
  assert.equal(n5, 'Not stored');
  assert.match(t5, /The same as 2 of the 2 on H9 firmware that answered/);
  assert.match(t5, /0000 0000 0000 0000 0000 0000 0000 0000 0000 N 00 0000 0000 0000 00/);
  assert.equal((await cell(dev[6].addr))[0], 'Stored');
  const [n7, t7] = await cell(dev[7].addr);
  assert.equal(n7, 'Stored · differs');
  assert.match(t7, /Differs from 5 of the 6 on K8 firmware that answered/);
  // Each setting by the CCA's name and unit; the one that differs, against
  // what most report. The K8's answer is cut short: its last "01" is the
  // start of UclLimit, so it's neither named nor shown.
  // 0FFF is the most a voltage field holds: no limit, not 204.75 V.
  assert.match(t7, /HwVout 179\.20 V, where most have no limit \(max\)/i);
  const [, t0] = await cell(dev[0].addr);
  assert.match(t0, /SwVout no limit \(max\)/i);
  assert.match(t0, /UvinLvHi 17\.00 V/);
  assert.match(t0, /AcLoss 0/);
  assert.doesNotMatch(t0, /UclLimit|BaseOverride|\b01\b/);
  assert.match(t5, /Settings not reported by H9 firmware/);
  assert.equal((await cell(dev[9].addr))[0], '—');
  assert.equal(await page.isVisible('table.nodes thead th.smart'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test("Devices, on the Pi: a Settings cell opens that optimizer's settings, named, beside what its family has", async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForFunction(() => /Not stored/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const openFor = async (i) => {
    await page.click(`#nodes-tbody tr[data-addr="${dev[i].addr}"] td.smart button`);
    await page.waitForSelector('#smart-panel[open]');
  };
  const rows = () => page.$$eval('#smart-panel .smart-row', (rs) => rs.map((r) => [
    r.querySelector('.smart-code').textContent, r.querySelector('.smart-val').textContent,
    r.querySelector('.smart-most').textContent, r.classList.contains('differs')]));
  // The odd one out: its row marked, beside what most K8s have.
  await openFor(7);
  assert.match(await page.textContent('#smart-panel h3'), new RegExp(`^${dev[7].name}`));
  assert.match(await page.textContent('#smart-panel .smart-verdict'), /Differs from 5 of the 6 K8s in one setting/);
  const r7 = await rows();
  assert.equal(r7.length, 10);
  assert.deepEqual(r7.find((r) => r[0] === 'HwVout'), ['HwVout', '179.20 V', 'No limit (max)', true]);
  assert.deepEqual(r7.find((r) => r[0] === 'SwVout'), ['SwVout', 'No limit (max)', 'No limit (max)', false]);
  assert.equal(r7.filter((r) => r[3]).length, 1);
  assert.match(await page.textContent('#smart-panel'), /never arrive/);
  assert.match(await page.textContent('#smart-panel .smart-raw'), /0E00 .* S 00 …$/);
  await page.keyboard.press('Escape');
  await page.waitForSelector('#smart-panel:not([open])', { state: 'attached' });
  // An H9: no table, it says the firmware answers zeros.
  await openFor(5);
  assert.equal((await rows()).length, 0);
  assert.match(await page.textContent('#smart-panel'), /H9 firmware doesn't report its settings/);
  assert.match(await page.textContent('#smart-panel .smart-verdict'), /not stored yet/i);
  await page.click('#smart-panel .smart-close');
  await page.waitForSelector('#smart-panel:not([open])', { state: 'attached' });
  // A phone: the same panel, as a sheet along the bottom.
  await page.setViewportSize({ width: 390, height: 844 });
  await openFor(0);
  const box = await page.$eval('#smart-panel', (d) => { const r = d.getBoundingClientRect(); return [r.left, r.bottom, r.width]; });
  assert.ok(box[0] <= 1 && Math.abs(box[1] - 844) <= 1 && box[2] >= 388, `sheet at ${box}`);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices, on the Pi with no TAP version heard yet, says where it comes from', async () => {
  const { page, ctx, errors } = await open(true, { taps: [], optimizers: [] });
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  assert.match(await page.textContent('#devices-taps'), /Not heard yet/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices, on the ESP32: no TAP list and no firmware column', async () => {
  const { page, ctx, errors } = await open(false);
  await page.waitForTimeout(500);
  assert.equal(await page.isVisible('#devices-taps'), false);
  assert.equal(await page.isVisible('table.nodes thead th.fw'), false);
  assert.equal(await page.isVisible('table.nodes thead th.smart'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test("Devices, on the Pi: the TAP's radio channel and admission level, once read", async () => {
  const { page, ctx } = await open(true);
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  const row = await page.textContent('#devices-taps tbody tr');
  assert.match(row, /Channel 18/);
  assert.match(row, /admits above 90/);
  await ctx.close();
});

test('Devices, on the Pi: a TAP whose radio was never read says so', async () => {
  const { page, ctx } = await open(true, { ...routes['/api/versions'], radios: [] });
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  const cells = await page.$$eval('#devices-taps tbody tr td', tds => tds.map(td => td.textContent.trim()));
  assert.ok(cells.includes('—'));
  assert.doesNotMatch(cells.join(' '), /Channel/);
  await ctx.close();
});
