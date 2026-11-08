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
let nextId = 1;
const ex = (o) => ({ id: nextId++, unix_ms: T, ours: false, other_master: false, addr: '1201', cmd: '0B0F', name: 'Tunnel',
  sub_name: 'DiscState', node: null, category: 'discovery', result: 'answered', late: false, rt_ms: 12,
  summary: 'Open, 412 s left · 40 of the 44 in the table have joined', answer: '', answer_after_ms: null,
  request_hex: '00 00 00 2E 05', reply_hex: '00 0E 00 2F 05 01 01 9C 00 2C 00 28 00 2C 00', reconfig: '', ...o });
const steady = [
  ex({}),
  ex({ sub_name: 'EncapString', category: 'queries', node: '0002', result: 'delivered', rt_ms: 13, summary: '^00Info',
       answer: '!Info 0027 12 0000', answer_after_ms: 1900, request_hex: '00 00 00 06 07 00 02 5E 30 30 49 6E 66 6F 0D', reply_hex: '00 0D 00 07 07' }),
  ex({ sub_name: 'EncapString', category: 'queries', node: '0007', result: 'not_delivered', rt_ms: 9,
       summary: "^00Version · the TAP couldn't deliver it (0x0146): the optimizer may be off or rejoining", reply_hex: '00 02' }),
  ex({ name: 'VERSION', sub_name: '', cmd: '000A', category: 'bringup', result: 'no_reply', rt_ms: null, summary: 'Nothing back', request_hex: '', reply_hex: '' }),
];
const reconfig = "GatewayCfg write (the TAP's channel, network ID and key)";
const stats = (o) => ({ window_s: 300, polls: 13230, polls_lost: 2, polls_empty: 13000, polls_refused: 0,
  count: { discovery: 1, queries: 2, housekeeping: 0, bringup: 1, reconfig: 0, other: 0 }, delivered: 1, not_delivered: 1,
  other_master: 0, reconfig: 0, rt_median_ms: 12, rt_max_ms: 41, discovery: 'Open, 412 s left · 40 of the 44 in the table have joined', ...o });

function bus(alarm) {
  return {
    alarm: alarm ? { active: true, count: 2, first_unix_ms: T, recent: [{ unix_ms: T, what: reconfig }, { unix_ms: T + 1000, what: 'SET_CHANNEL' }] }
      : { active: false, count: 0, first_unix_ms: 0, recent: [] },
    stats: alarm ? stats({ other_master: 2, reconfig: 2 }) : stats({}),
    exchanges: alarm ? [...steady,
      ex({ other_master: true, sub_name: 'GatewayCfg write', category: 'reconfig', summary: reconfig, reconfig,
           request_hex: "00 00 00 0D 32 01 00 + 36 bytes hidden (the TAP's network config and key)", reply_hex: '00 06 00 0E 32 01' }),
      ex({ other_master: true, name: 'SET_CHANNEL', sub_name: '', cmd: '000C', category: 'reconfig', rt_ms: 6, summary: 'SET_CHANNEL',
           reconfig: 'SET_CHANNEL', request_hex: '20 00', reply_hex: '', unix_ms: T + 1000 })] : steady,
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

test('the Bus log: the strip, one row per exchange, and a row opens to its bytes', async () => {
  const { page, ctx, errors } = await open({ hash: '#buslog' });
  await page.waitForSelector('#nav button[data-view="bus"]', { state: 'visible' });
  // Listening: the Bus item is the traffic alone, no Control tab.
  assert.equal((await page.textContent('#nav button[data-view="bus"]')).trim().replace(/^\S+\s*/, ''), 'Bus');
  assert.equal(await page.isVisible('#view-buslog .bus-tabs'), false);
  await page.waitForFunction(() => document.querySelectorAll('#bus-rows .bx-row').length === 4);
  const strip = await page.textContent('#bus-strip');
  assert.match(strip, /Polls13,23044\.1\/s · 2 lost/);
  assert.match(strip, /Round trip12 msmedian · max 41 ms/);
  assert.match(strip, /Other master0none heard/);
  const rows = await page.$$eval('#bus-rows .bx-row', (els) => els.map((e) => e.textContent));
  assert.match(rows[0], /master1201DiscStateTunnel—answered12 msOpen, 412 s left · 40 of the 44 in the table have joined/);
  // A node id carries its panel's name, from /api/devices.
  const panel = routes['/api/devices'].devices.find((d) => d.addr === '0002').name;
  assert.match(rows[1], new RegExp(`0002${panel}delivered13 ms\\^00Info → !Info 0027 12 0000 1\\.9 s later`));
  assert.match(rows[2], /0007not delivered9 ms/);
  assert.match(rows[3], /VERSION—no reply—Nothing back/);
  assert.match(await page.textContent('#buslog-meta'), /4 exchanges · polls 44\.1\/s/);
  // A row opens to its bytes, and stays open through the next refresh.
  await page.click('#bus-rows .bx-row >> nth=0');
  assert.match(await page.textContent('#bus-rows .bx-detail'), /request00 00 00 2E 05reply00 0E 00 2F 05/);
  await page.waitForTimeout(2300);
  assert.equal(await page.locator('#bus-rows .bx-detail').count(), 1);
  // The chips filter.
  await page.click('#bus-chips .bx-chip[data-c="queries"]');
  assert.equal(await page.locator('#bus-rows .bx-row').count(), 2);
  // The chip pressed keeps the keyboard, through its click and the next refresh.
  const focusedChip = () => page.evaluate(() => document.activeElement && document.activeElement.dataset.c);
  assert.equal(await focusedChip(), 'queries');
  await page.waitForTimeout(2300);
  assert.equal(await focusedChip(), 'queries');
  assert.equal(await page.isVisible('#bus-alarm-card'), false);
  assert.equal(await page.isVisible('#bus-alarm-banner'), false);
  assert.equal(/undefined|null|NaN/.test(await page.textContent('#view-buslog')), false);
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
  assert.match(card, /2 frames/);
  await page.waitForFunction(() => document.querySelectorAll('#bus-rows .bx-row.reconfig').length === 2);
  assert.match(await page.textContent('#bus-rows .bx-row.reconfig >> nth=0'), /another master.*GatewayCfg write.*reconfigures/);
  assert.match(await page.textContent('#bus-strip'), /Other master22 reconfigure the TAP/);
  await page.click('#bus-chips .bx-chip[data-c="reconfig"]');
  assert.equal(await page.locator('#bus-rows .bx-row').count(), 2);
  await page.click('#bus-rows .bx-row >> nth=0');
  assert.match(await page.textContent('#bus-rows .bx-detail'), /\+ 36 bytes hidden \(the TAP's network config and key\)/);
  await page.click('#bus-alarm-clear');
  await page.waitForSelector('#bus-alarm-card', { state: 'hidden' });
  assert.equal(api.clears, 1);
  await page.waitForSelector('#bus-alarm-banner', { state: 'hidden' });
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the Bus log at phone width: rows stack, no sideways scroll', async () => {
  const { page, ctx, errors } = await open({ hash: '#buslog', alarm: true, width: 390 });
  await page.waitForSelector('#bus-alarm-card', { state: 'visible' });
  await page.waitForFunction(() => document.querySelectorAll('#bus-rows .bx-row').length === 6);
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= 390));
  assert.equal(await page.isVisible('#bus-box .bx-cols'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});
