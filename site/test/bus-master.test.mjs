// site/test/bus-master.test.mjs
// The Raspberry Pi build's Bus master page: the PV output switch, the
// sidebar's PV pill and the banner, in the real app.html, in Chromium,
// against a mock API (/api/master and its POST in memory; the rest from the
// screenshot fixtures).
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

function master(role, pv = {}, standby = 'off') {
  return {
    role, active: true, fault: 'none', phase: 'running', detail: '', extra_taps: [],
    housekeeping: { on: true, learned_41: 0, sent_41: 3, sent_session: 1, sent_broadcast: 2, sent_walk_pages: 4, walks_done: 2 },
    queries: { on: false, writes: false, sent_reads: 0, sent_writes: 0, answers: 0 },
    tap_version: '', tap: { lost: 0, last_lost_unix: 0, recent_lost_unix: [], back_without_discovery: 0 },
    discovery_read: true, discovery: { running: true, remaining_s: 400, node_count: 44 }, renewals: 2, discovery_renewing: true,
    polls: 1000, timeouts: 0, resyncs: 0, gap_ms: { max: 10, max_at_unix: 0, recent: 9 }, last_reply_unix: 0,
    pv: { on: true, source: 'setting', since: 0, confirmed: true, unconfirmed: false, last_sent: '01', last_reply: '01', ...pv },
    standby: { state: standby, takeover_s: 3, master_heard_ms_ago: standby === 'standing_by' ? 200 : -1, takeovers: 0, last_takeover_unix: 0, yields: 0 },
  };
}

function settings() {
  return {
    serial_port: 'auto', bus_role: 'master', standby_takeover_s: 3, master_startup: true, master_bench: false,
    master_housekeeping: true, master_queries: false, master_optimizer_writes: false, gateway_id: 0x1201,
    web_port: 0, web_port_active: 80, web_username: 'admin', web_password_set: true,
    tls_cert: '', tls_key: '', tls_active: false, tls_from_args: false, power_calibration: 1, publish_interval_s: 30,
    device_id: 'tigomonitor', show_cca_page: true, show_cloud_page: true,
    mqtt: { host: 'broker', port: 1883, username: '', password_set: false, discovery: true,
      discovery_prefix: 'homeassistant', base_topic: 'tigomonitor', standby_panels: false, pv_switch: false },
    mqtt_connected: true, mqtt_error: '',
  };
}

// opts.role: 'master' | 'standby' | 'listen'. opts.standby: the standby state.
// opts.pv: the PV fields. api.pvStatus: the POST's answer.
async function open(opts = {}) {
  const api = { master: master(opts.role || 'master', opts.pv, opts.standby), posts: [], settingsPosts: [], discoveryPosts: [], pvStatus: 200 };
  if (opts.discovery) api.master.discovery = { ...api.master.discovery, ...opts.discovery };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json(200, { ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/master') return json(200, api.master);
    if (path === '/api/master/pv' && req.method() === 'POST') {
      const body = JSON.parse(req.postData());
      api.posts.push(body);
      if (api.pvStatus !== 200) return json(api.pvStatus, { error: 'set a web password first' });
      api.master.pv = { ...api.master.pv, on: body.on, source: 'setting', since: 1762200000, confirmed: true };
      return json(200, { on: body.on });
    }
    if (path === '/api/master/discovery' && req.method() === 'POST') {
      const body = JSON.parse(req.postData());
      api.discoveryPosts.push(body);
      api.master.discovery_renewing = body.renew;
      return json(200, { renew: body.renew });
    }
    if (path === '/api/settings') {
      if (req.method() === 'POST') { api.settingsPosts.push(JSON.parse(req.postData())); return json(200, { saved: true }); }
      return json(200, settings());
    }
    if (path === '/api/serial-ports') return json(200, { ports: [] });
    if (path === '/api/devices' && opts.devices) return json(200, { devices: opts.devices, count: opts.devices.length });
    if (path === '/api/cloud/status') return json(200, { configured: false });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/${opts.hash || ''}`, { waitUntil: 'networkidle' });
  return { page, ctx, api, errors };
}

const nav = (page) => page.isVisible('#nav button[data-view="busmaster"]');

test('listening, there is no Bus master page and no PV pill', async () => {
  const { page, ctx, errors } = await open({ role: 'listen' });
  await page.waitForTimeout(600);
  assert.equal(await nav(page), false);
  assert.equal(await page.isVisible('#pv-pill'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('running the bus with PV on: the page, the pill says PV on, no banner', async () => {
  const { page, ctx, errors } = await open();
  await page.waitForSelector('#pv-pill', { state: 'visible' });
  assert.equal(await nav(page), true);
  assert.equal((await page.textContent('#pv-pill')).trim(), 'PV on');
  assert.equal(await page.isVisible('#pv-banner'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('turning PV off asks first; dismissed it sends nothing, accepted it sends off', async () => {
  const { page, ctx, api } = await open({ hash: '#busmaster' });
  await page.waitForSelector('#pv-toggle', { state: 'visible' });
  const seen = [];
  page.once('dialog', (d) => { seen.push(d.message()); d.dismiss(); });
  await page.click('#pv-toggle');
  await page.waitForTimeout(300);
  assert.deepEqual(seen, ["Turn the array's output off? This is not rapid shutdown."]);
  assert.deepEqual(api.posts, []);
  page.once('dialog', (d) => d.accept());
  await page.click('#pv-toggle');
  await page.waitForFunction(() => document.getElementById('pv-pill').textContent.trim() === 'PV off');
  assert.deepEqual(api.posts, [{ on: false }]);
  assert.equal(await page.isVisible('#pv-banner'), true);
  assert.match(await page.textContent('#pv-banner'), /PV output is off, turned off from this monitor at /);
  assert.match(await page.getAttribute('#pv-pill', 'class'), /\boff\b/);
  // Back on: no confirm.
  await page.click('#pv-toggle');
  await page.waitForFunction(() => document.getElementById('pv-pill').textContent.trim() === 'PV on');
  assert.deepEqual(api.posts, [{ on: false }, { on: true }]);
  assert.equal(await page.isVisible('#pv-banner'), false);
  await ctx.close();
});

test('heard off from the master running the bus: the banner says so', async () => {
  const { page, ctx } = await open({ role: 'standby', standby: 'standing_by', pv: { on: false, source: 'heard', since: 1762200000 } });
  await page.waitForSelector('#pv-banner', { state: 'visible' });
  assert.match(await page.textContent('#pv-banner'), /turned off from the monitor running the bus at /);
  await ctx.close();
});

test('standing by, the switch is read-only and says where to change it', async () => {
  const { page, ctx, api } = await open({ role: 'standby', standby: 'standing_by', hash: '#busmaster' });
  await page.waitForSelector('#pv-toggle', { state: 'visible' });
  await page.waitForFunction(() => document.getElementById('pv-toggle').disabled);
  assert.match(await page.textContent('#pv-status'), /change it on the device running the bus/);
  assert.deepEqual(api.posts, []);
  await ctx.close();
});

test('a refused switch says why and changes nothing', async () => {
  const { page, ctx, api } = await open({ hash: '#busmaster' });
  await page.waitForSelector('#pv-toggle', { state: 'visible' });
  api.pvStatus = 403;
  page.once('dialog', (d) => d.accept());
  await page.click('#pv-toggle');
  await page.waitForFunction(() => /web password/.test(document.getElementById('pv-status').textContent));
  assert.equal((await page.textContent('#pv-pill')).trim(), 'PV on');
  await ctx.close();
});

test('the bus master figures moved from Diagnostics to Bus master', async () => {
  const { page, ctx } = await open({ hash: '#busmaster' });
  await page.waitForSelector('#diag-master', { state: 'visible' });
  assert.equal(await page.$eval('#diag-master', (e) => e.closest('.view').id), 'view-busmaster');
  await ctx.close();
});

test('the Home Assistant PV switch setting is on the Tools MQTT card', async () => {
  const { page, ctx, api } = await open({ hash: '#tools' });
  await page.waitForSelector('#pi-mqtt-pv-switch', { state: 'attached' });
  await page.click('label:has(#pi-mqtt-pv-switch)');
  await page.click('#pi-save');
  await page.waitForFunction(() => true);
  await page.waitForTimeout(500);
  assert.equal(api.settingsPosts.length > 0 && api.settingsPosts.at(-1).mqtt.pv_switch, true);
  await ctx.close();
});

test('the page says optimizers that also follow the RSS signal keep producing', async () => {
  const { page, ctx } = await open({ hash: '#busmaster' });
  await page.waitForSelector('#pv-card', { state: 'visible' });
  assert.match(await page.textContent('#view-busmaster'), /also follow the RSS signal.*keep producing/s);
  await ctx.close();
});

// The fixture's panels, all producing but for the ones named.
function devicesProducing(names) {
  return routes['/api/devices'].devices.map((d) => ({ ...d, power: names.includes(d.name) ? d.power : 0,
    power_out: names.includes(d.name) ? d.power_out : 0, power_in: names.includes(d.name) ? d.power_in : 0 }));
}

test('a minute after off, it lists the panels still producing', async () => {
  const since = Math.floor(Date.now() / 1000) - 120;
  const { page, ctx } = await open({ hash: '#busmaster', pv: { on: false, since }, devices: devicesProducing(['D5', 'D6']) });
  await page.waitForFunction(() => /still producing/.test(document.getElementById('pv-still').textContent));
  assert.match(await page.textContent('#pv-still'), /^2 panels are still producing: D5, D6\./);
  assert.match(await page.textContent('#pv-banner'), /2 panels still producing/);
  await ctx.close();
});

test('all stopped: it says so', async () => {
  const since = Math.floor(Date.now() / 1000) - 120;
  const { page, ctx } = await open({ hash: '#busmaster', pv: { on: false, since }, devices: devicesProducing([]) });
  await page.waitForFunction(() => /stopped/.test(document.getElementById('pv-still').textContent));
  assert.match(await page.textContent('#pv-still'), /^Every panel has stopped producing\./);
  await ctx.close();
});

test('in the first minute it is still checking, and with PV on it says nothing', async () => {
  const since = Math.floor(Date.now() / 1000) - 10;
  const { page, ctx } = await open({ hash: '#busmaster', pv: { on: false, since }, devices: devicesProducing(['D6']) });
  await page.waitForFunction(() => /checking/i.test(document.getElementById('pv-still').textContent));
  await ctx.close();
  const on = await open({ hash: '#busmaster', devices: devicesProducing(['D6']) });
  await on.page.waitForSelector('#pv-card', { state: 'visible' });
  await on.page.waitForTimeout(600);
  assert.equal((await on.page.textContent('#pv-still')).trim(), '');
  await on.ctx.close();
});

test('the discovery window can be left to close, and renewed again', async () => {
  const { page, ctx, api, errors } = await open({ hash: '#busmaster' });
  await page.waitForSelector('#disc-card', { state: 'visible' });
  assert.equal(await page.textContent('#disc-state'), 'Enabled');
  assert.equal(await page.textContent('#disc-toggle'), 'Disable');
  await page.click('#disc-toggle');
  await page.waitForFunction(() => document.getElementById('disc-toggle').textContent === 'Enable');
  assert.deepEqual(api.discoveryPosts, [{ renew: false }]);
  assert.match(await page.textContent('#disc-state'), /^Disabled$/);
  assert.match(await page.textContent('#diag-discovery-sub'), /disabled/);
  await page.click('#disc-toggle');
  await page.waitForFunction(() => document.getElementById('disc-toggle').textContent === 'Disable');
  assert.deepEqual(api.discoveryPosts, [{ renew: false }, { renew: true }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the discovery window card says how many of the TAP\'s table have joined', async () => {
  const { page, ctx, errors } = await open({ hash: '#busmaster', discovery: { joined: 40, in_table: 44 } });
  await page.waitForFunction(() => /left/.test(document.getElementById('disc-detail').textContent));
  assert.equal(await page.textContent('#disc-detail'), 'Now open, 400 s left · 40 of the 44 in the TAP\'s table have joined');
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('an empty table is said as such, and a reply without the counts shows none', async () => {
  const empty = await open({ hash: '#busmaster', discovery: { joined: 0, in_table: 0 } });
  await empty.page.waitForFunction(() => /left/.test(document.getElementById('disc-detail').textContent));
  assert.equal(await empty.page.textContent('#disc-detail'), 'Now open, 400 s left · the TAP\'s table is empty');
  await empty.ctx.close();
  // An older daemon: no joined or in_table.
  const { page, ctx, errors } = await open({ hash: '#busmaster' });
  await page.waitForFunction(() => /left/.test(document.getElementById('disc-detail').textContent));
  assert.equal(await page.textContent('#disc-detail'), 'Now open, 400 s left');
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('standing by, the discovery switch is disabled', async () => {
  const { page, ctx } = await open({ role: 'standby', standby: 'standing_by', hash: '#busmaster' });
  await page.waitForSelector('#disc-card', { state: 'visible' });
  await page.waitForFunction(() => document.getElementById('disc-toggle').disabled);
  await ctx.close();
});
