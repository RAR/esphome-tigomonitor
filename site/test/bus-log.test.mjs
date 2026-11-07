// site/test/bus-log.test.mjs
// The Raspberry Pi build's Bus log page and the TAP-reconfiguration banner,
// in the real app.html, in Chromium, against a mock /api/bus (the rest from
// the screenshot fixtures).
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

const T = 1762480000000;
const ev = (o) => ({ unix_ms: T, from_tap: false, ours: false, addr: '1201', cmd: '0B0F', sub: null, node: null,
  len: 5, name: 'Tunnel · DiscState', text: '', reconfig: '', ...o });
const steady = [
  ev({ sub: 46, hex: '00 00 00 2E 05' }),
  ev({ from_tap: true, cmd: '0B10', sub: 47, len: 15, name: 'Tunnel reply · DiscState reply' }),
  ev({ sub: 6, node: '0002', len: 15, name: 'Tunnel · EncapString', text: '^00Info' }),
];
const reconfig = "GatewayCfg write (the TAP's channel, network ID and key)";

function bus(alarm) {
  return {
    alarm: alarm ? { active: true, count: 2, first_unix_ms: T, recent: [{ unix_ms: T, what: reconfig }, { unix_ms: T + 1000, what: 'SET_CHANNEL' }] }
      : { active: false, count: 0, first_unix_ms: 0, recent: [] },
    polls: 1234,
    events: alarm ? [...steady, ev({ sub: 13, len: 43, name: 'Tunnel · GatewayCfg', reconfig, hex: "00 00 00 0D 05 01 00 + 36 bytes hidden (the TAP's network config and key)" }),
      ev({ cmd: '000C', len: 2, name: 'SET_CHANNEL', reconfig: 'SET_CHANNEL', unix_ms: T + 1000 })] : steady,
  };
}

async function open(opts = {}) {
  const api = { bus: bus(opts.alarm), clears: 0 };
  const ctx = await browser.newContext({ viewport: { width: opts.width || 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json(200, { ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/bus') return json(200, api.bus);
    if (path === '/api/bus/alarm/clear' && req.method() === 'POST') {
      api.clears++;
      api.bus = bus(false);
      return json(200, { cleared: true });
    }
    if (path === '/api/master') return json(200, { role: 'listen', active: false });
    if (path === '/api/cloud/status') return json(200, { configured: false });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('dialog', (d) => d.accept());
  await page.goto(`http://127.0.0.1:${port}/${opts.hash || ''}`, { waitUntil: 'networkidle' });
  return { page, ctx, api, errors };
}

test('the Bus log lists the frames by name, with the poll count, and no alarm', async () => {
  const { page, ctx, errors } = await open({ hash: '#buslog' });
  await page.waitForSelector('#nav button[data-view="buslog"]', { state: 'visible' });
  await page.waitForFunction(() => document.querySelectorAll('#bus-box .log-line').length === 3);
  const rows = await page.$$eval('#bus-box .log-line', (els) => els.map((e) => e.textContent));
  assert.match(rows[0], /master → 1201.*Tunnel · DiscState · 5 B00 00 00 2E 05$/);
  assert.match(rows[1], /TAP 1201.*Tunnel reply · DiscState reply · 15 B/);
  assert.match(rows[2], /Tunnel · EncapString · 15 B · node 0002 · \^00Info/);
  assert.match(await page.textContent('#buslog-meta'), /3 frames · 1,234 polls/);
  assert.equal(await page.isVisible('#bus-alarm-card'), false);
  assert.equal(await page.isVisible('#bus-alarm-banner'), false);
  assert.equal(await page.locator('#bus-box').textContent().then((t) => /undefined|null/.test(t)), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('a reconfiguration: the banner on every page, the alarm card, the rows marked, and Clear', async () => {
  const { page, ctx, api, errors } = await open({ alarm: true });
  await page.waitForSelector('#bus-alarm-banner', { state: 'visible' });
  assert.match(await page.textContent('#bus-alarm-banner'), /Another device reconfigured the TAP: SET_CHANNEL/);
  await page.click('#bus-alarm-banner a');
  await page.waitForSelector('#bus-alarm-card', { state: 'visible' });
  const card = await page.textContent('#bus-alarm-card');
  assert.match(card, /GatewayCfg write \(the TAP's channel, network ID and key\)/);
  assert.match(card, /SET_CHANNEL/);
  assert.match(card, /2 frames/);
  await page.waitForFunction(() => document.querySelectorAll('#bus-box .log-line.error').length === 2);
  assert.match(await page.locator('#bus-box .log-line.error .bus-hex').first().textContent(), /\+ 36 bytes hidden \(the TAP's network config and key\)$/);
  await page.click('#bus-alarm-clear');
  await page.waitForSelector('#bus-alarm-card', { state: 'hidden' });
  assert.equal(api.clears, 1);
  await page.waitForSelector('#bus-alarm-banner', { state: 'hidden' });
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the Bus log at phone width: no sideways scroll', async () => {
  const { page, ctx, errors } = await open({ hash: '#buslog', alarm: true, width: 390 });
  await page.waitForSelector('#bus-alarm-card', { state: 'visible' });
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= 390));
  assert.deepEqual(errors, []);
  await ctx.close();
});
