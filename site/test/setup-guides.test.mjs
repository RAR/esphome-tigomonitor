// site/test/setup-guides.test.mjs
// System setup's guides beside the optimizer tasks: "Set up a new system", a
// checklist from the bus to the layout that ticks itself off from what the
// Pi already knows, and "Add a TAP", from the CCA's setup to its name. In
// the real app.html, in Chromium, against a mock API.
import { test, before, after, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { routes, piRoutes } from '../screenshots/fixtures.mjs';

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
// A test that fails before its own ctx.close() leaves the page polling the
// mock; close whatever is still open.
afterEach(async () => { await Promise.all(browser.contexts().map((c) => c.close())); });

const tap = (id, state = 'running') => ({ id, mac: '', version: 'Mgate Version G8.59', state,
  discovery: { read: true, running: false, remaining_s: 0, node_count: 24, joined: 24 } });

function world() {
  const edit = { kind: '', active: false, finished: false, ok: false, dry_run: false, mac: '', label: '', id: '', ack: -1,
                 step: '', message: '', started_unix: 0, finished_unix: 0, dry_frames: [], interrupted: false };
  const master = { ...piRoutes['/api/master'], role: 'master', active: true, phase: 'running', adopt_taps: false,
                   taps: [tap('1201')], table: { writes: true, edit, waiting: [], joins: [] },
                   discovery: { running: false, remaining_s: 0, node_count: 24, joined: 24, in_table: 24 }, discovery_read: true };
  const settings = { ...piRoutes['/api/settings'], master_adopt_taps: false };
  const layout = JSON.parse(JSON.stringify(piRoutes['/api/layout'] ?? routes['/api/layout']));
  return { master, settings, layout, names: {}, posts: [] };
}

// Next, then wait for the step list to show the step named: each step
// renders after the page has fetched again.
async function next(page, name) {
  await page.click('button[data-setup-next]');
  await page.waitForFunction((n) => (document.querySelector('.setup-rail li[aria-current="step"]') || {}).textContent?.endsWith(n), name);
}

async function open(w) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (req.method() === 'POST') {
      const b = req.postData() ? JSON.parse(req.postData()) : {};
      w.posts.push({ path, ...b });
      if (path === '/api/settings') {
        Object.assign(w.settings, b);
        if ('master_adopt_taps' in b) w.master.adopt_taps = b.master_adopt_taps;
        return json(200, { saved: true });
      }
      if (path === '/api/master/taps/herd') {
        // The herd runs, then the new TAP is at the next free id.
        w.master.phase = 'discovering';
        setTimeout(() => { w.master.phase = 'running'; w.master.taps = [tap('1201'), tap('1202')]; }, 300);
        return json(200, { started: true });
      }
      if (path === '/api/taps/name') { w.names[b.tap] = b.name; return json(200, b); }
      if (path === '/api/master/discovery') { w.master.discovery_renewing = b.renew; return json(200, b); }
      if (path === '/api/layout') { w.layout = { manual: true, unassigned: w.layout.unassigned, inverters: b.inverters }; return json(200, { success: true }); }
      if (path === '/api/strings/rating') return json(200, { success: true });
      if (path === '/api/master/taps/forget') {
        w.master.taps = w.master.taps.filter((t) => t.id !== b.tap);
        return json(200, { retired: true });
      }
      return json(404, {});
    }
    if (path === '/api/status') return json(200, { ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/master') return json(200, w.master);
    if (path === '/api/settings') return json(200, w.settings);
    if (path === '/api/layout') return json(200, w.layout);
    if (path === '/api/serial-ports') return json(200, { ports: [{ path: '/dev/serial/by-id/usb-FTDI_FT232R-if00', device: '/dev/ttyUSB0', description: 'FT232R USB UART' }] });
    if (path === '/api/versions') {
      const v = piRoutes['/api/versions'] ?? routes['/api/versions'];
      return json(200, { ...v, taps: (v.taps || []).map((t) => ({ ...t, name: w.names[t.addr] ?? t.name })) });
    }
    const body = piRoutes[path] ?? routes[path];
    return body === undefined ? route.fulfill({ status: 404, body: 'no' }) : json(200, body);
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('dialog', (d) => d.accept());
  await page.goto(`http://127.0.0.1:${port}/#setup`, { waitUntil: 'networkidle' });
  await page.waitForSelector('.setup-tasks');
  return { page, ctx, errors };
}

test('System setup: "Set up a new system" ticks off what is done and points at what is left', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  // The fixtures: frames heard, a TAP's version, 24 optimizers, one panel not
  // placed, every string rated.
  const card = page.locator('.setup-start[data-task="system"]');
  assert.match(await card.textContent(), /4 of 5 done · next: Lay out the array/);
  await card.click();
  await page.waitForSelector('.setup-flow[data-task="system"]');
  assert.match(await page.textContent('.setup-body'), /Done.*8,629,414 messages heard/s);
  const steps = ['Connect the bus', 'Find the TAP', 'Optimizers', 'Lay out the array', 'Names and ratings', 'Finish setup'];
  assert.deepEqual(await page.$$eval('.setup-rail li', (lis) => lis.map((l) => l.textContent.replace(/^[\d✓]+/, ''))), steps);
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array']) await next(page, n);
  const layout = await page.textContent('.setup-body');
  assert.match(layout, /To do/);
  assert.match(layout, /1 optimizer isn't placed yet/);
  // Where to do the rest is a card: a title, a line, the place it opens.
  assert.equal(await page.getAttribute('.setup-body a.setup-card[href="#topology"]', 'href'), '#topology');
  assert.match(await page.textContent('.setup-body a.setup-card[href="#topology"]'), /Topology.*Place each panel on its string/s);
  for (const n of ['Names and ratings', 'Finish setup']) await next(page, n);
  const done = await page.textContent('.setup-body');
  assert.match(done, /4 of 5 done/);
  assert.match(done, /Lay out the array/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: Connect the bus is done on the step: the port and the role, saved', async () => {
  const w = world();
  w.settings = { ...w.settings, serial_port: '', bus_role: 'listen' };
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  await page.waitForSelector('#setup-serial option[value="/dev/serial/by-id/usb-FTDI_FT232R-if00"]', { state: 'attached' });
  await page.selectOption('#setup-serial', '/dev/serial/by-id/usb-FTDI_FT232R-if00');
  await page.check('input[name="setup-role"][value="master"]');
  await page.click('button[data-setup-bus-save]');
  for (let i = 0; i < 40 && !w.posts.some((p) => p.path === '/api/settings'); i++) await page.waitForTimeout(100);
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/settings'),
    [{ path: '/api/settings', serial_port: '/dev/serial/by-id/usb-FTDI_FT232R-if00', bus_role: 'master' }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: Find the TAP shows it answering, and lets optimizers join again when renewal is off', async () => {
  const w = world();
  w.master = { ...w.master, discovery_renewing: false };
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  await next(page, 'Find the TAP');
  const tiles = await page.textContent('.setup-body .setup-stats');
  assert.match(tiles, /Answering/);
  assert.match(tiles, /G8\.59/);
  assert.match(tiles, /24 of 24/);
  await page.click('button[data-setup-window]');
  for (let i = 0; i < 40 && !w.posts.some((p) => p.path === '/api/master/discovery'); i++) await page.waitForTimeout(100);
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/master/discovery'), [{ path: '/api/master/discovery', renew: true }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: with no layout, Lay out the array makes the inverters, MPPTs and strings', async () => {
  const w = world();
  w.layout = { manual: false, inverters: [], unassigned: w.layout.unassigned.concat(w.layout.inverters.flatMap((i) => i.mppts.flatMap((m) => m.strings.flatMap((x) => x.panels)))) };
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array']) await next(page, n);
  await page.fill('#setup-invs', '2');
  await page.fill('#setup-mppts', '2');
  await page.fill('#setup-strings', '1');
  assert.match(await page.textContent('button[data-setup-layout]'), /2 inverters, 4 strings/);
  await page.click('button[data-setup-layout]');
  for (let i = 0; i < 40 && !w.posts.some((p) => p.path === '/api/layout'); i++) await page.waitForTimeout(100);
  const sent = w.posts.find((p) => p.path === '/api/layout');
  assert.deepEqual(sent.inverters.map((i) => [i.name, i.mppts.map((m) => [m.label, m.display_label, m.strings.map((x) => x.label)])]), [
    ['Inverter 1', [['MPPT 1', '', ['A']], ['MPPT 2', '', ['B']]]],
    ['Inverter 2', [['MPPT 3', 'MPPT 1', ['C']], ['MPPT 4', 'MPPT 2', ['D']]]],
  ]);
  assert.ok(sent.inverters.every((i) => i.mppts.every((m) => m.strings.every((x) => x.panels.length === 0))));
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: an existing layout is never replaced from the step', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array']) await next(page, n);
  assert.equal(await page.locator('button[data-setup-layout]').count(), 0);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: Names and ratings takes each string\'s panel rating on the step', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array', 'Names and ratings']) await next(page, n);
  const first = w.layout.inverters[0].mppts[0].strings[0];
  const box = page.locator(`input[data-setup-rating="${first.label}"]`);
  assert.equal(await box.inputValue(), String(first.rating_w));
  await box.fill('410');
  await box.press('Tab');
  for (let i = 0; i < 40 && !w.posts.some((p) => p.path === '/api/strings/rating'); i++) await page.waitForTimeout(100);
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/strings/rating'), [{ path: '/api/strings/rating', label: first.label, rating_w: 410 }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: Finish setup sets the password, which ends setup mode', async () => {
  const w = world();
  w.settings = { ...w.settings, web_password_set: false, setup_open: true, setup_open_until: Math.floor(Date.now() / 1000) + 3600 };
  const { page, ctx, errors } = await open(w);
  // Setup mode is said on every page, with the way out.
  assert.match(await page.textContent('#setup-banner'), /anyone on your network can change the bus/);
  await page.click('.setup-start[data-task="system"]');
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array', 'Names and ratings', 'Finish setup']) await next(page, n);
  await page.fill('#setup-pass', 'sunny-roof');
  await page.fill('#setup-pass2', 'sunny-roof');
  await page.click('button[data-setup-finish]');
  for (let i = 0; i < 40 && !w.posts.some((p) => p.path === '/api/settings'); i++) await page.waitForTimeout(100);
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/settings'), [{ path: '/api/settings', web_password: 'sunny-roof' }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: two passwords that differ are not sent', async () => {
  const w = world();
  w.settings = { ...w.settings, web_password_set: false, setup_open: true };
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-start[data-task="system"]');
  for (const n of ['Find the TAP', 'Optimizers', 'Lay out the array', 'Names and ratings', 'Finish setup']) await next(page, n);
  await page.fill('#setup-pass', 'sunny-roof');
  await page.fill('#setup-pass2', 'sunny-rofo');
  assert.equal(await page.isDisabled('button[data-setup-finish]'), true);
  assert.equal(w.posts.length, 0);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('with setup closed, no banner', async () => {
  const w = world();
  w.settings = { ...w.settings, setup_open: false };
  const { page, ctx } = await open(w);
  assert.equal(await page.isVisible('#setup-banner'), false);
  await ctx.close();
});

test('System setup: "Add a TAP" from the CCA to its name, the switch on for it and off after', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-task[data-task="addtap"]');
  await page.waitForSelector('.setup-flow[data-task="addtap"]');
  assert.match(await page.textContent('.setup-body'), /set up by a CCA first/);
  await next(page, 'Wire it in');
  assert.match(await page.textContent('.setup-body'), /Wire it onto this bus/);
  await next(page, 'Allow a new TAP');
  // The switch is off: Next waits for it.
  assert.equal(await page.isDisabled('button[data-setup-next]'), true);
  await page.click('button[data-setup-adopt="on"]');
  await page.waitForFunction(() => !document.querySelector('button[data-setup-next]').disabled);
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/settings'), [{ path: '/api/settings', master_adopt_taps: true }]);
  await next(page, 'Look for TAPs');
  await page.click('button[data-setup-look]');
  await page.waitForFunction(() => /Found TAP 1202/.test(document.querySelector('.setup-body').textContent), null, { timeout: 15000 });
  assert.equal(w.posts.filter((p) => p.path === '/api/master/taps/herd').length, 1);
  await next(page, 'Name it');
  await page.fill('#setup-tap-name', 'Shed');
  await next(page, 'Complete');
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/taps/name'), [{ path: '/api/taps/name', tap: '1202', name: 'Shed' }]);
  const done = await page.textContent('.setup-body');
  assert.match(done, /TAP 1202 is on the bus/);
  await page.click('button[data-setup-adopt="off"]');
  await page.waitForFunction(() => !document.querySelector('button[data-setup-adopt="off"]'));
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/settings').map((p) => p.master_adopt_taps), [true, false]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: "Add a TAP" says so when this Pi is not running the bus', async () => {
  const w = world();
  w.master = { ...w.master, role: 'listen', active: false };
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-task[data-task="addtap"]');
  await next(page, 'Wire it in');
  await next(page, 'Allow a new TAP');
  assert.match(await page.textContent('.setup-body'), /not running the bus/);
  assert.equal(await page.isDisabled('button[data-setup-next]'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: "Retire a TAP" drops one that has gone for good, never one still answering', async () => {
  const w = world();
  w.master.taps = [tap('1201'), tap('1202', 'removed')];
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-task[data-task="retiretap"]');
  await page.waitForSelector('.setup-flow[data-task="retiretap"]');
  assert.equal(await page.isDisabled('input[name="setup-tap-retire"][value="1201"]'), true);
  assert.match(await page.textContent('.setup-body'), /answering/);
  assert.equal(await page.isDisabled('button[data-setup-retire]'), true);
  await page.check('input[name="setup-tap-retire"][value="1202"]');
  await page.click('button[data-setup-retire]');
  await page.waitForFunction(() => /TAP 1202 is retired/.test(document.querySelector('.setup-body').textContent));
  assert.deepEqual(w.posts.filter((p) => p.path === '/api/master/taps/forget'), [{ path: '/api/master/taps/forget', tap: '1202' }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});
