// site/test/dashboard-alerts.test.mjs
// The Dashboard's alerts: a whole string at 0 W while the rest produce is
// "down", but the Pi's "All panels" group, the panels no string claims, is
// not a string, and a lone stray there (a spare that once joined and is now
// off) must not read as one down (2026-11-10, on the roof).
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

async function alerts(devices, inverters) {
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 1200 } });
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (body) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json({ ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/devices') return json({ devices, count: devices.length });
    if (path === '/api/inverters') return json({ ...routes['/api/inverters'], inverters });
    if (routes[path] !== undefined) return json(routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/#dashboard`, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => document.getElementById('alerts').textContent.trim() !== '');
  const text = await page.textContent('#alerts');
  await ctx.close();
  return { text, errors };
}

const base = routes['/api/devices'].devices;
const invs = routes['/api/inverters'].inverters;
// As the Pi's /api/inverters gives it: the strings no inverter claims, here
// only "All panels", under a synthetic Unassigned inverter.
const unassigned = { name: 'Unassigned', display_name: '', unassigned: true, mppts: [], total_power: 0, peak_power: 0,
  active_devices: 0, total_devices: 1,
  strings: [{ label: 'All panels', display_label: '', mppt: '', panel_rating_w: 0, total_power: 0, peak_power: 0,
    active_devices: 0, total_devices: 1 }] };
const stray = { addr: '002C', barcode: '', name: '002C', string_label: 'All panels', power: 0, power_in: 0,
  voltage_in: 0, data_age_ms: 3 * 3600 * 1000, stale: true };

test('a stray in "All panels" at 0 W is not a string down', async () => {
  const { text, errors } = await alerts([...base, stray], [...invs, unassigned]);
  assert.doesNotMatch(text, /no production while the rest of the system is producing/);
  assert.match(text, /002C.*no data for 3h/);
  assert.deepEqual(errors, []);
});

test('a real string at 0 W while the rest produce is still down', async () => {
  const label = base[0].string_label;
  const devices = base.map((d) => d.string_label === label ? { ...d, power: 0, power_in: 0 } : d);
  const { text, errors } = await alerts(devices, invs);
  assert.match(text, /no production while the rest of the system is producing/);
  assert.deepEqual(errors, []);
});
