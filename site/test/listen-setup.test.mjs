// site/test/listen-setup.test.mjs
// System setup on a new install that listens while a CCA runs the bus: no
// add/replace/retire tasks (the CCA's job), and "Set up a new system" waits
// for the system to be heard instead of finding the TAP and its table, with
// a checklist of what has arrived, since some only goes by now and then.
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
  browser = await chromium.launch({ args: ['--no-sandbox', '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1'] });
});
after(async () => { await browser.close(); server.close(); });
afterEach(async () => { await Promise.all(browser.contexts().map((c) => c.close())); });

const devices = routes['/api/devices'].devices;
const listening = { ...piRoutes['/api/master'], role: 'listen', active: false, table: undefined, discovery: undefined, taps: [] };

// A first evening of listening: every optimizer reporting, the TAP heard
// with its version, barcodes for all but two, firmware for none yet, and
// no joins (no sunrise yet). opts.role: the bus role.
function world(opts = {}) {
  const role = opts.role || 'listen';
  const nodes = routes['/api/nodes'].nodes.map((n, i) => i < 2 ? { ...n, long_address: '' } : n);
  const versions = { ...piRoutes['/api/versions'] ?? routes['/api/versions'], optimizers: [], settings: [] };
  const mesh = { ...piRoutes['/api/mesh'], nodes: piRoutes['/api/mesh'].nodes.map((n) => ({ ...n, last_join_unix: null, parent: null })) };
  return { role, nodes, versions, mesh, ...opts.over };
}

async function open(w, hash = '#setup') {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (body) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json({ ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/settings') return json({ ...piRoutes['/api/settings'], bus_role: w.role });
    if (path === '/api/master') return json(w.role === 'listen' ? listening : piRoutes['/api/master']);
    if (path === '/api/nodes') return json({ ...routes['/api/nodes'], nodes: w.nodes });
    if (path === '/api/versions') return json(w.versions);
    if (path === '/api/mesh') return json(w.mesh);
    const body = piRoutes[path] ?? routes[path];
    return body === undefined ? route.fulfill({ status: 404, body: 'no' }) : json(body);
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/${hash}`, { waitUntil: 'networkidle' });
  await page.waitForSelector('#nav-setup', { state: 'visible' });
  await page.waitForSelector('.setup-start');
  return { page, ctx, errors };
}

const railSteps = (page) => page.$$eval('.setup-rail li', (lis) => lis.map((li) => li.textContent.replace(/^[✓\d]+/, '')));

test('listening, System setup leaves the TAP and optimizer tasks to the CCA', async () => {
  const { page, errors } = await open(world());
  assert.equal(await page.locator('[data-setup-start="add"], [data-setup-start="replace"], [data-setup-start="addtap"]').count(), 0);
  assert.match(await page.textContent('#setup-root'), /done by the CCA while it runs the bus/);
  assert.match(await page.textContent('.setup-start'), /of 4 done/);
  assert.deepEqual(errors, []);
});

test('listening, Set up a new system listens for a day in place of finding the TAP and its optimizers', async () => {
  const { page, errors } = await open(world());
  await page.click('.setup-start');
  assert.deepEqual(await railSteps(page), ['Connect the bus', 'Listen for a day', 'Lay out the array', 'Names and ratings', 'Finish setup']);
  await page.click('[data-setup-next]');
  await page.waitForSelector('.setup-heard');
  const items = await page.$$eval('.setup-heard li', (lis) => lis.map((li) => [li.querySelector('strong').textContent, li.classList.contains('ok')]));
  assert.deepEqual(items, [['Power reports', true], ['The TAP', true], ['Barcodes', false], ['Firmware and settings', false], ['Joins', false]]);
  const text = await page.textContent('#setup-root');
  assert.match(text, /full day/);
  assert.match(text, /in the evening, around sunset/);
  assert.match(text, new RegExp(`${devices.length - 2} of ${devices.length}`));
  assert.deepEqual(errors, []);
});

test('listening, the checklist ticks off as the rest arrives', async () => {
  const w = world();
  w.nodes = routes['/api/nodes'].nodes;
  w.versions = routes['/api/versions'];
  w.versions = { ...w.versions, optimizers: devices.map((d) => ({ addr: d.addr, model: 'Mnode', version: 'K8.0120 (2D)' })) };
  w.mesh = piRoutes['/api/mesh'];
  const { page, errors } = await open(w);
  await page.click('.setup-start');
  await page.click('[data-setup-next]');
  await page.waitForSelector('.setup-heard');
  assert.equal(await page.locator('.setup-heard li:not(.ok)').count(), 0);
  assert.equal(await page.locator('h2', { hasText: 'Listen for a day' }).locator('.setup-state.ok').count(), 1, 'the step says Done');
  assert.equal(await page.locator('.setup-rail li.done', { hasText: 'Listen for a day' }).count(), 0, 'the current step is never ticked');
  assert.deepEqual(errors, []);
});

test('running the bus, System setup is as before', async () => {
  const { page, errors } = await open(world({ role: 'master' }));
  assert.equal(await page.locator('[data-setup-start="add"]').count(), 1);
  await page.click('.setup-start');
  assert.deepEqual(await railSteps(page), ['Connect the bus', 'Find the TAP', 'Optimizers', 'Lay out the array', 'Names and ratings', 'Finish setup']);
  assert.deepEqual(errors, []);
});
