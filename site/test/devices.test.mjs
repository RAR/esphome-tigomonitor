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
