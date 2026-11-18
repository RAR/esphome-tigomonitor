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
import { routes, piRoutes } from '../screenshots/fixtures.mjs';
import { answer, answerAll } from './ui-dialog.mjs';

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

async function open(linux, versions = routes['/api/versions'], posts = [], master = { role: 'listen', active: false }, devices = null, nodes = null) {
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 1200 } });
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/taps/name' && route.request().method() === 'POST') {
      const b = JSON.parse(route.request().postData());
      posts.push(b);
      versions = { ...versions, taps: versions.taps.map((t) => t.addr === b.tap ? { ...t, name: b.name } : t) };
      return json(200, b);
    }
    if (path === '/api/status') return json(200, linux ? { ...routes['/api/status'], platform: 'linux' } : routes['/api/status']);
    if (path === '/api/versions') return linux ? json(200, versions) : route.fulfill({ status: 404, body: 'no' });
    if (path === '/api/master/table/stop' && route.request().method() === 'POST') {
      const b = JSON.parse(route.request().postData());
      posts.push({ path, ...b });
      master = { ...master, table: { ...master.table, waiting: master.table.waiting.filter((w) => w.mac !== b.mac) } };
      return json(200, { stopped: true });
    }
    if (path.startsWith('/api/master/table/') && route.request().method() === 'POST') {
      const b = JSON.parse(route.request().postData());
      posts.push({ path, ...b });
      master = { ...master, table: { ...master.table, edit: { kind: path.endsWith('add') ? 'add' : 'remove', active: false, finished: true,
        ok: true, dry_run: b.dry_run, mac: '04C05B0000001234', label: '0-1234R', id: '0008', ack: -1, step: 'done',
        message: 'dry run: would list 04C05B0000001234 at 0008 (not joined)', dry_frames: ['290104C05B00000012348008'], interrupted: false } } };
      return json(200, { started: true, dry_run: b.dry_run });
    }
    if (path === '/api/master') return json(200, master);
    if (path === '/api/devices' && devices) return json(200, { devices, count: devices.length });
    if (path === '/api/nodes' && nodes) return json(200, { ...routes['/api/nodes'], nodes });
    if (path === '/api/nodes/delete' && route.request().method() === 'POST') {
      const addr = new URL(route.request().url()).searchParams.get('addr');
      posts.push({ path, addr });
      if (devices) devices = devices.filter((d) => d.addr !== addr);
      return json(200, { status: 'ok' });
    }
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
  assert.match(tap, /1201.*G series · mesh.*G8\.59 Jan {2}1 2026/s);
  await page.waitForFunction(() => /K8\.0120/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const row = (addr) => page.$eval(`#nodes-tbody tr[data-addr="${addr}"]`, (tr) => tr.textContent);
  assert.match(await row(dev[0].addr), /K8\.0120 \(2D\)/);
  // An update shows what it replaced.
  assert.match(await row(dev[4].addr), /K8\.0130 \(2D\)/);
  // Its row opens to say what it replaced, and when.
  await page.click(`#nodes-tbody tr[data-addr="${dev[4].addr}"] .node-toggle`);
  assert.match(await page.textContent(`#nd-${dev[4].addr.toUpperCase()}`), /Was\s*K8\.0120 \(2D\)/);
  assert.equal(await page.isVisible('table.nodes thead th.fw'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test("Devices, on the Pi: each optimizer's settings, stored or not, and the odd ones out", async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForFunction(() => /Not reported/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const cell = (addr) => page.$eval(`#nodes-tbody tr[data-addr="${addr}"] td.smart`, (td) => [td.textContent.trim(), !!td.querySelector('.warn')]);
  const detail = async (addr) => {
    await page.click(`#nodes-tbody tr[data-addr="${addr}"] .node-toggle`);
    return page.textContent(`#nd-${addr.toUpperCase()}`);
  };
  assert.deepEqual((await cell(dev[0].addr))[0], 'Stored');
  // An H9 answers with fixed zeros and a flag that reads N on every read (S
  // only in the reply to a write), so neither says what it holds: "Not
  // reported", in no warning colour, whichever flag it last gave, and never
  // compared.
  const [n5, warn5] = await cell(dev[5].addr);
  assert.equal(n5, 'Not reported');
  assert.equal(warn5, false);
  const d5 = await detail(dev[5].addr);
  assert.doesNotMatch(d5, /The same as|Differs|not stored/i);
  assert.match(d5, /H9 firmware doesn't report its settings/);
  assert.match(d5, /0000 0000 0000 0000 0000 0000 0000 0000 0000 N 00 0000 0000 0000 00/);
  assert.equal((await cell(dev[6].addr))[0], 'Not reported');
  const [n7, warn7] = await cell(dev[7].addr);
  assert.equal(n7, 'Stored · differs');
  assert.equal(warn7, true);
  assert.match(await detail(dev[7].addr), /Differs from 5 of the 6 K8s in one setting/);
  assert.equal((await cell(dev[9].addr))[0], '—');
  assert.equal(await page.isVisible('table.nodes thead th.smart'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// The four fields after AcLoss arrive only in a whole answer. They're the
// optimizer's own protection settings, which a CCA never writes: the current
// limit only applies below its input-voltage threshold, so the two read as
// one line; OvMinPwm differs by firmware, so none of them is compared.
test("Devices, on the Pi: a whole settings answer's protection fields, in words", async () => {
  const { page, errors } = await open(true);
  await page.waitForFunction(() => /Not reported/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const id = (i) => `#nd-${dev[i].addr.toUpperCase()}`;
  const rows = (i) => page.$$eval(`${id(i)} .smart-row:not(.mppt-row)`, (rs) => rs.map((r) => [
    r.querySelector('.smart-name > span').textContent, r.querySelector('.smart-code').textContent,
    r.querySelector('.smart-val').textContent, r.querySelector('.smart-most').textContent]));
  for (const i of [0, 4]) {
    await page.click(`#nodes-tbody tr[data-addr="${dev[i].addr}"] .node-toggle`);
    await page.waitForSelector(id(i));
  }
  const r0 = await rows(0);
  assert.deepEqual(r0.find((r) => r[1] === 'UclLimit · UclThrsh'), ['Current limit at low input voltage', 'UclLimit · UclThrsh', '1.0 A below 32 V', '—']);
  assert.deepEqual(r0.find((r) => r[1] === 'OvMinPwm'), ['Over-voltage minimum PWM', 'OvMinPwm', '9 (3.5%)', '—']);
  assert.deepEqual(r0.find((r) => r[1] === 'BaseOverride'), ['Base-memory override', 'BaseOverride', 'Off', '—']);
  // AcLoss is active-low: 0 is shutdown on AC loss enabled (a CCA's "ON"
  // writes 0), so it reads On, beside most K8s' On.
  assert.deepEqual(r0.find((r) => r[1] === 'AcLoss'), ['Shut down on AC loss', 'AcLoss', 'On', 'On']);
  assert.equal(r0.some((r) => r[1] === 'UclThrsh'), false);
  assert.doesNotMatch(await page.textContent(id(0)), /Limits|Discharge enabled|never arrive/);
  assert.deepEqual((await rows(4)).find((r) => r[1] === 'OvMinPwm'), ['Over-voltage minimum PWM', 'OvMinPwm', '0 (can switch off)', '—']);
  assert.deepEqual(errors, []);
});

test("Devices, on the Pi: a row opens in place to show that optimizer, its settings beside what its family has", async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForFunction(() => /Not reported/.test(document.getElementById('nodes-tbody').textContent));
  const dev = routes['/api/devices'].devices;
  const id = (i) => `#nd-${dev[i].addr.toUpperCase()}`;
  const toggle = (i) => `#nodes-tbody tr[data-addr="${dev[i].addr}"] .node-toggle`;
  const rows = (i, sel = '.smart-row:not(.mppt-row)') => page.$$eval(`${id(i)} ${sel}`, (rs) => rs.map((r) => [
    r.querySelector('.smart-code').textContent, r.querySelector('.smart-val').textContent,
    r.querySelector('.smart-most').textContent, r.classList.contains('differs')]));
  // Closed to start; a click anywhere on the row opens it, under the row.
  assert.equal(await page.$(id(7)), null);
  assert.equal(await page.getAttribute(toggle(7), 'aria-expanded'), 'false');
  await page.click(`#nodes-tbody tr[data-addr="${dev[7].addr}"] td.num >> nth=0`);
  await page.waitForSelector(id(7));
  assert.equal(await page.getAttribute(toggle(7), 'aria-expanded'), 'true');
  assert.equal(await page.$eval(id(7), (tr) => tr.previousElementSibling.dataset.addr), dev[7].addr);
  // What it is, its firmware and its rating.
  const text = await page.textContent(id(7));
  assert.match(text, new RegExp(dev[7].barcode));
  assert.match(text, /Rated\s*75 V, 700 W/);
  assert.match(text, /ESO075V700W/);
  // The odd one out: its row marked, beside what most K8s have.
  assert.match(text, /Differs from 5 of the 6 K8s in one setting/);
  const r7 = await rows(7);
  assert.equal(r7.length, 10);
  assert.deepEqual(r7.find((r) => r[0] === 'HwVout'), ['HwVout', '179.20 V', 'No limit (max)', true]);
  assert.deepEqual(r7.find((r) => r[0] === 'SwVout'), ['SwVout', 'No limit (max)', 'No limit (max)', false]);
  assert.equal(r7.filter((r) => r[3]).length, 1);
  assert.match(text, /never arrive/);
  assert.match(text, /0E00 .* S 00 …/);
  // Its four MPPT parameters, from its answer to ^00Mppt, beside what most
  // optimizers report; the same as most here.
  assert.deepEqual(await rows(7, '.mppt-row'), [
    ['MPPT 5', '18', '18', false], ['MPPT 6', '0.100 V', '0.100 V', false],
    ['MPPT 7', '8', '8', false], ['MPPT 8', '9.0 V', '9.0 V', false]]);
  // It stays open through the next poll, and another can open beside it.
  await page.evaluate(() => renderNodesTable());
  assert.ok(await page.$(id(7)));
  // One whose PIV activation current differs: marked, whatever its ^00Smrt.
  await page.click(toggle(3));
  await page.waitForSelector(id(3));
  assert.deepEqual((await rows(3, '.mppt-row')).filter((r) => r[3]), [['MPPT 7', '10', '8', true]]);
  // The toggle closes it again, and keeps the keyboard where it was.
  await page.focus(toggle(3));
  await page.keyboard.press('Enter');
  await page.waitForSelector(id(3), { state: 'detached' });
  assert.equal(await page.evaluate(() => document.activeElement.classList.contains('node-toggle')), true);
  // An H9: no settings table, no stored or not; it says the firmware answers
  // fixed zeros. But its MPPT parameters it does report, as every firmware does.
  await page.click(toggle(5));
  await page.waitForSelector(id(5));
  assert.equal((await rows(5)).length, 0);
  assert.equal((await page.textContent(`${id(5)} .smart-pill`)).trim(), 'Not reported');
  assert.match(await page.textContent(id(5)), /H9 firmware doesn't report its settings/);
  assert.doesNotMatch(await page.textContent(id(5)), /not stored/i);
  assert.equal((await rows(5, '.mppt-row')).length, 4);
  // A phone: the details fit the screen, though the table scrolls sideways.
  await page.setViewportSize({ width: 390, height: 844 });
  const w = await page.$eval(`${id(7)} .nd`, (d) => d.getBoundingClientRect().width);
  assert.ok(w <= 390, `details ${w} px wide`);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// Each optimizer's own rating, from its answer to a calibration read,
// against what it carries: over it now (watts or volts in), or a panel rated
// above it (Topology's nameplate), which it limits to its own rating.
test("Devices, on the Pi: an optimizer over its rating, and one whose panel is rated above it", async () => {
  const dev = routes['/api/devices'].devices;
  const v = routes['/api/versions'];
  // A1 makes 350 W on a 300 W optimizer; B2's 400 W panel is on a 375 W one.
  const ratings = v.ratings.map((r) => r.addr === dev[0].addr ? { ...r, watts: 300 }
    : r.addr === dev[7].addr ? { ...r, watts: 375, cal: 'ESO075V375W-TEST' } : r);
  const { page, ctx, errors } = await open(true, { ...v, ratings });
  await page.waitForFunction(() => /Over rating/.test(document.getElementById('nodes-tbody').textContent));
  const row = (i) => page.textContent(`#nodes-tbody tr[data-addr="${dev[i].addr}"] td:first-child`);
  assert.match(await row(0), /Over rating/);
  assert.doesNotMatch(await row(0), /Panel above/);  // over says it; its 400 W panel's note is in its details
  assert.match(await row(7), /Panel above 375 W/);
  assert.doesNotMatch(await row(7), /Over rating/);
  assert.doesNotMatch(await row(1), /Over rating|Panel above/);
  await page.click(`#nodes-tbody tr[data-addr="${dev[0].addr}"] .node-toggle`);
  const d0 = await page.textContent(`#nd-${dev[0].addr.toUpperCase()}`);
  assert.match(d0, /Over its rating now: 350 W in, rated 300 W/);
  await page.click(`#nodes-tbody tr[data-addr="${dev[7].addr}"] .node-toggle`);
  const d7 = await page.textContent(`#nd-${dev[7].addr.toUpperCase()}`);
  assert.match(d7, /Peak\s*396 W, 106% of 375 W/);
  assert.match(d7, /Input now\s*39\.2 V of 75 V/);
  assert.match(d7, /panel is rated 400 W, above this optimizer's 375 W rating: in strong sun it can run over its rating/);
  assert.doesNotMatch(d7, /clip|limits/i);
  // The Dashboard rings the tile that's over, and says so under its watts.
  await page.click('#nav button[data-view="dashboard"]');
  await page.waitForSelector(`.panel-heat-tile.over[data-addr="${dev[0].addr}"]`);
  assert.equal(await page.locator('.panel-heat-tile.over').count(), 1);
  assert.equal(await page.textContent(`.panel-heat-tile[data-addr="${dev[0].addr}"] .pht-over`), 'Over');
  assert.equal(await page.isVisible('#legend-over'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// An optimizer the Pi hears (or heard) that isn't in its node list: a spare
// that still has the network's key reports through the TAP whatever the
// TAP's table says (owner's analysis, 2026-11-10). It is listed apart, and
// Forget drops it from the Dashboard and the mesh map.
const stray = { addr: '0080', barcode: '04C05B0000200001', name: '', string_label: '', power: 0, power_in: 0,
  voltage_in: 0, data_age_ms: 3 * 3600 * 1000, stale: true };
test('Devices, on the Pi: an optimizer heard but not in the node list, and Forget', async () => {
  const posts = [];
  const { page, ctx, errors } = await open(true, routes['/api/versions'], posts, undefined, [...routes['/api/devices'].devices, stray]);
  await answerAll(page, true);
  await page.waitForSelector('#devices-strays', { state: 'visible' });
  const row = await page.textContent('#devices-strays tbody');
  assert.match(row, /0080/);
  assert.match(row, /no data for 3h/);
  assert.equal(await page.locator('#nodes-tbody tr[data-addr="0080"]').count(), 0);
  await page.click('#devices-strays button.stray-forget');
  await page.waitForSelector('#devices-strays', { state: 'hidden' });
  assert.deepEqual(posts.filter((p) => p.path === '/api/nodes/delete'), [{ path: '/api/nodes/delete', addr: '0080' }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// The Model column names the TAP's series and whether it runs the mesh
// firmware, from its version string, as the CCA classifies it. A star-only
// TAP is flagged: on a mesh system it forces the old star stack.
test("Devices, on the Pi: each TAP's series, mesh or star only, from its version", async () => {
  const tap = (addr, model, version) => ({ addr, model, version, previous: '', since_unix_ms: 1767225600000,
    last_unix_ms: 1767225600000, name: '' });
  const versions = { ...routes['/api/versions'], radios: [], ids: [], taps: [
    tap('1201', 'Mgate', 'H1.0007 Apr 22 2025 15:28:25 GW-H158.7.00'), tap('1202', 'Mgate', 'G8.65 May  9 2025 13:52:34 GW-H150.6.0S0.12'),
    tap('1203', 'Gway', 'G5.26 Jan  1 2020'), tap('1204', 'Mgate', 'G5.26 Jan  1 2020'), tap('1205', 'Xgate', 'Z9.01')] };
  const { page, ctx, errors } = await open(true, versions);
  await page.waitForSelector('#devices-taps tbody tr:nth-child(5)');
  const cells = await page.$$eval('#devices-taps tbody tr td.tap-model', (tds) => tds.map((td) =>
    [td.innerText.replace(/\s+/g, ' ').trim(), td.classList.contains('star-only'), td.title]));
  assert.deepEqual(cells.map(([t, star]) => [t, star]), [
    ['H series · mesh GW-H158.7.00', false], ['G series · mesh GW-H150.6.0S0.12', false], ['Star only', true], ['Star only', true], ['Xgate', false]]);
  assert.match(cells[0][2], /Mgate firmware H1\.0007/);
  // The board ID is the version string's GW- suffix (owner's analysis): shown
  // under the series, and not repeated in the Firmware column.
  assert.equal(await page.textContent('#devices-taps tbody tr:nth-child(1) td.tap-model .tap-board'), 'GW-H158.7.00');
  const fw = await page.$$eval('#devices-taps tbody tr td.tap-fw', (tds) => tds.map((td) => td.textContent.trim()));
  // The build time goes to the tooltip with the rest, so the cell stays on one line.
  assert.deepEqual(fw.slice(0, 2), ['H1.0007 Apr 22 2025', 'G8.65 May  9 2025']);
  assert.match(await page.getAttribute('#devices-taps tbody tr:nth-child(1) td.tap-fw', 'title'), /15:28:25 GW-H158\.7\.00/);
  assert.match(cells[2][2], /forces a mesh system back to the old star stack/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices, on the Pi: a node the TAP stopped listing a day ago is listed as heard, not in the table', async () => {
  const T = 1767225600;
  const nodes = routes['/api/nodes'].nodes.map((m) => ({ ...m, last_roster_unix: T }));
  const gone = { ...stray, addr: '002D', barcode: '04C05B0000200002' };
  nodes.push({ ...nodes[0], addr: '002D', long_address: '04C05B0000200002', cca_label: '', cca_string: '', last_roster_unix: T - 2 * 86400 });
  const { page, ctx, errors } = await open(true, routes['/api/versions'], [], undefined, [...routes['/api/devices'].devices, gone], nodes);
  await page.waitForSelector('#devices-strays', { state: 'visible' });
  assert.match(await page.textContent('#devices-strays tbody'), /002D/);
  assert.equal(await page.locator('#devices-strays tbody tr').count(), 1);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices: no stray list when every optimizer is in the node list, or on the ESP32', async () => {
  const pi = await open(true);
  await page_settled(pi.page);
  assert.equal(await pi.page.isVisible('#devices-strays'), false);
  await pi.ctx.close();
  const esp = await open(false, undefined, [], undefined, [...routes['/api/devices'].devices, stray]);
  await page_settled(esp.page);
  assert.equal(await esp.page.isVisible('#devices-strays'), false);
  await esp.ctx.close();
});
async function page_settled(page) { await page.waitForTimeout(500); }

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

test("Devices, on the Pi: the TAP's ID, from its MAC, and the name it was given", async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  const row = page.locator('#devices-taps tbody tr').first();
  assert.equal((await row.locator('td.tap-name').textContent()).trim(), 'Garage');
  assert.equal((await row.locator('td.tap-id').textContent()).trim(), '04C05B0000001234');
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices, on the Pi: a TAP is named in place, and an empty name clears it', async () => {
  const posts = [];
  const unnamed = { ...routes['/api/versions'], taps: routes['/api/versions'].taps.map(({ name, ...t }) => t) };
  const { page, ctx, errors } = await open(true, unnamed, posts);
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  const cell = page.locator('#devices-taps tbody tr td.tap-name');
  assert.match(await cell.textContent(), /Add a name/);
  await cell.locator('button').click();
  const input = cell.locator('input');
  await input.fill('Shed');
  await input.press('Enter');
  await page.waitForFunction(() => document.querySelector('#devices-taps td.tap-name').textContent.trim() === 'Shed');
  assert.deepEqual(posts, [{ tap: '1201', name: 'Shed' }]);
  // Escape leaves it as it was.
  await cell.locator('button').click();
  await cell.locator('input').fill('Barn');
  await cell.locator('input').press('Escape');
  assert.equal((await cell.textContent()).trim(), 'Shed');
  assert.equal(posts.length, 1);
  await cell.locator('button').click();
  await cell.locator('input').fill('');
  await cell.locator('input').press('Enter');
  await page.waitForFunction(() => /Add a name/.test(document.querySelector('#devices-taps td.tap-name').textContent));
  assert.deepEqual(posts.at(-1), { tap: '1201', name: '' });
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Devices, on the Pi: a change to an optimizer\'s settings is listed, newest first', async () => {
  const { page, ctx, errors } = await open(true);
  await page.waitForSelector('#devices-changes', { state: 'visible' });
  const dev = routes['/api/devices'].devices;
  const text = await page.textContent('#devices-changes');
  assert.match(text, new RegExp(`${dev[3].name}.*MPPT.*12 40 08 12.*12 40 0A 12`, 's'));
  await ctx.close();
  const none = await open(true, { ...routes['/api/versions'], changes: [] });
  await none.page.waitForSelector('#devices-taps', { state: 'visible' });
  assert.equal(await none.page.isVisible('#devices-changes'), false);
  assert.deepEqual([...errors, ...none.errors], []);
  await none.ctx.close();
});

test("Devices, on the Pi: the TAP's radio channel and admission level, once read", async () => {
  const { page, ctx } = await open(true);
  await page.waitForSelector('#devices-taps', { state: 'visible' });
  // The channel first; the threshold beneath, named for what it does, and
  // explained in the tooltip, since its scale has no units anyone knows.
  const cell = page.locator('#devices-taps tbody tr td.tap-radio').first();
  assert.equal((await cell.innerText()).replace(/\s+/g, ' ').trim(), 'Channel 18 join threshold 90');
  const title = await cell.getAttribute('title');
  assert.match(title, /must be heard above 90 to join/);
  assert.match(title, /SLm 60/);
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

// Running the bus (as master, or a standby that has taken it), the Devices
// page says what the TAP holds and points to System setup; listening, or a
// standby standing by, it doesn't.
test('Devices, on the Pi: what the TAP holds, and where to change it', async () => {
  const base = { ...piRoutes['/api/master'], active: true, discovery_read: true,
                 discovery: { running: false, remaining_s: 0, node_count: 24, joined: 23, in_table: 24 } };
  for (const [master, shown] of [[{ ...base, role: 'master' }, true],
                                 [{ ...base, role: 'standby', standby: { ...base.standby, state: 'active' } }, true],
                                 [{ ...base, role: 'standby', standby: { ...base.standby, state: 'standing_by' } }, false],
                                 [{ role: 'listen', active: false }, false]]) {
    const { page, ctx, errors } = await open(true, routes['/api/versions'], [], master);
    await page.waitForSelector('#devices-taps', { state: 'visible' });
    await page.waitForTimeout(300);
    assert.equal(await page.isVisible('#devices-setup-link'), shown, JSON.stringify(master.role));
    if (shown) {
      assert.equal(await page.textContent('#devices-setup-link'), 'Your system has 24 optimizers, 23 joined. To replace, add, or retire an optimizer, open System setup.');
      assert.equal(await page.getAttribute('#devices-setup-link a', 'href'), '#setup');
    }
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

// More than one TAP: each optimizer's, by the TAP's name, under its
// address. With one, nothing is added.
test('Devices, on the Pi: with two TAPs, each optimizer shows the one it reports through', async () => {
  const saved = routes['/api/devices'];
  const devices = saved.devices.map((d, i) => ({ ...d, tap: i < 6 ? '1201' : '1202' }));
  routes['/api/devices'] = { ...saved, devices };
  const versions = { ...routes['/api/versions'], taps: [...routes['/api/versions'].taps,
    { addr: '1202', model: 'Mgate', version: 'G8.60 Jan  1 2026', previous: '', since_unix_ms: 1767225600000, last_unix_ms: 1767225600000 }] };
  try {
    const { page, ctx, errors } = await open(true, versions);
    await page.waitForSelector('#nodes-tbody .node-tap');
    const tapOf = (addr) => page.$eval(`#nodes-tbody tr[data-addr="${addr}"] .node-tap`, (e) => e.textContent);
    assert.equal(await tapOf(devices[0].addr), 'Garage');
    assert.equal(await tapOf(devices[7].addr), 'TAP 1202');
    assert.deepEqual(errors, []);
    await ctx.close();
  } finally {
    routes['/api/devices'] = saved;
  }
  const { page, ctx, errors } = await open(true);
  await page.waitForTimeout(300);
  assert.equal(await page.$('#nodes-tbody .node-tap'), null);
  assert.deepEqual(errors, []);
  await ctx.close();
});
