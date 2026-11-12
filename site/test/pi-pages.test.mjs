// site/test/pi-pages.test.mjs
// The Settings and Diagnostics pages' tabs, and the Raspberry Pi build's switches for the CCA
// Info and Tigo Cloud pages, in the real app.html, in Chromium, against a mock API: /api/status says
// platform 'linux', /api/settings is in memory and records every POST. The
// rest of the API comes from the screenshot fixtures.
import { test, before, after } from 'node:test';
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

function piSettings(pages = {}) {
  return {
    serial_port: 'auto', bus_role: 'listen', standby_takeover_s: 3, master_startup: false,
    master_housekeeping: false, master_queries: false, master_optimizer_writes: false, gateway_id: 0x1201,
    web_port: 0, web_port_active: 80, web_username: 'admin', web_password_set: false,
    tls_cert: '', tls_key: '', tls_active: false, tls_from_args: false, power_calibration: 1, publish_interval_s: 30,
    device_id: 'tigomonitor', show_cca_page: true, show_cloud_page: true, ...pages,
    mqtt: { host: '', port: 1883, username: '', password_set: false, discovery: true,
      discovery_prefix: 'homeassistant', base_topic: 'tigomonitor', standby_panels: false },
    mqtt_connected: false, mqtt_error: '',
  };
}

// opts.linux: false for the ESP32. opts.pages: the saved switches.
// api.saveStatus makes the next settings save fail.
async function open(opts = {}) {
  const linux = opts.linux !== false;
  const api = { settings: piSettings(opts.pages), posts: [], saveStatus: 200 };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json(200, { ...routes['/api/status'], ...(linux ? { platform: 'linux' } : {}) });
    if (path === '/api/settings' && linux) {
      if (req.method() === 'POST') {
        const body = JSON.parse(req.postData());
        api.posts.push(body);
        if (api.saveStatus !== 200) return json(api.saveStatus, { error: 'disk full' });
        Object.assign(api.settings, body);
        return json(200, { saved: true });
      }
      return json(200, api.settings);
    }
    if (path === '/api/master' && linux && opts.master) return json(200, piRoutes['/api/master']);
    if (path === '/api/mesh/joins' && linux) {
      api.joinReads = [...(api.joinReads || []), new URL(req.url()).searchParams.get('addr')];
      return json(200, { addr: new URL(req.url()).searchParams.get('addr'), ...(opts.joins ?? piRoutes['/api/mesh/joins']) });
    }
    if (path === '/api/mesh' && linux) { api.meshReads = (api.meshReads || 0) + 1; return json(200, opts.mesh ?? piRoutes['/api/mesh']); }
    if (path === '/api/logs') { api.logReads = (api.logReads || 0) + 1; return json(200, { lines: [], next: 0 }); }
    if (path === '/api/cloud/status') return json(200, { configured: true });
    if (path === '/api/cloud/health') return json(200, { summary: {} });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  if (opts.init) await ctx.addInitScript(opts.init);
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/${opts.hash || ''}`, { waitUntil: 'networkidle' });
  return { page, ctx, api, errors };
}

const navShown = (page, view) => page.isVisible(`#nav button[data-view="${view}"]`);
const activeView = (page) => page.$eval('.view.active', (v) => v.id);

// The sidebar's groups, each with the items it shows, in order.
const navGroups = (page) => page.$$eval('#nav .nav-group', (gs) => gs.map((g) => [
  g.querySelector('.nav-head').textContent.trim(),
  [...g.querySelectorAll('button')].filter((b) => b.offsetParent !== null)
    .map((b) => b.textContent.trim().replace(/^\S+\s*/, '')),
]));

test('the Pi\'s sidebar is grouped: Monitor, Inspect, then Control', async () => {
  const { page, ctx, errors } = await open();
  await page.waitForSelector('#nav button[data-view="buslog"]', { state: 'visible' });
  assert.deepEqual(await navGroups(page), [
    ['Monitor', ['Dashboard', 'History', 'Topology', 'Devices']],
    ['Inspect', ['Diagnostics', 'Bus traffic', 'CCA Info', 'Tigo Cloud']],
    ['Control', ['System setup', 'Settings']],
  ]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the ESP32\'s sidebar keeps the groups without the Pi\'s pages', async () => {
  const { page, ctx, errors } = await open({ linux: false });
  await page.waitForTimeout(600);
  assert.deepEqual(await navGroups(page), [
    ['Monitor', ['Dashboard', 'History', 'Topology', 'Devices']],
    ['Inspect', ['Diagnostics', 'CCA Info', 'Tigo Cloud']],
    ['Control', ['Settings']],
  ]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('on the Pi both pages are in the sidebar until turned off', async () => {
  const { page, ctx, errors } = await open();
  await page.waitForSelector('#nav-cloudstatus', { state: 'visible' });
  assert.equal(await navShown(page, 'cca'), true);
  assert.equal(await navShown(page, 'cloudstatus'), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('show_cca_page off hides CCA Info and leaves Tigo Cloud', async () => {
  const { page, ctx } = await open({ pages: { show_cca_page: false } });
  await page.waitForSelector('#nav-cloudstatus', { state: 'visible' });
  assert.equal(await navShown(page, 'cca'), false);
  assert.equal(await navShown(page, 'cloudstatus'), true);
  await ctx.close();
});

test('show_cloud_page off hides Tigo Cloud and its sidebar box, even with a cloud account connected', async () => {
  const { page, ctx } = await open({ pages: { show_cloud_page: false } });
  await page.waitForTimeout(800);  // past the cloud status check that would show them
  assert.equal(await navShown(page, 'cloudstatus'), false);
  assert.equal(await page.isVisible('#cloud-box'), false);
  assert.equal(await navShown(page, 'cca'), true);
  await ctx.close();
});

test('a link to a hidden page opens the Dashboard instead', async () => {
  const { page, ctx } = await open({ pages: { show_cca_page: false }, hash: '#cca' });
  await page.waitForFunction(() => document.querySelector('.view.active').id === 'view-dashboard');
  assert.equal(page.url().endsWith('#dashboard'), true);
  await page.evaluate(() => { window.location.hash = '#cca'; });
  await page.waitForTimeout(300);
  assert.equal(await activeView(page), 'view-dashboard');
  await ctx.close();
});

test('the Settings switches save at once and show or hide the page', async () => {
  const { page, ctx, api } = await open({ hash: '#settings' });
  await page.click('#settings-tab-web');
  await page.waitForSelector('#pi-show-cca', { state: 'attached' });
  await page.waitForFunction(() => document.getElementById('pi-show-cca').checked);
  await page.click('label:has(#pi-show-cca)');
  await page.waitForFunction(() => !document.querySelector('#nav button[data-view="cca"]').offsetParent);
  assert.deepEqual(api.posts, [{ show_cca_page: false }]);
  await page.click('label:has(#pi-show-cloud)');
  await page.waitForFunction(() => !document.getElementById('nav-cloudstatus').offsetParent);
  await page.click('label:has(#pi-show-cca)');
  await page.waitForSelector('#nav button[data-view="cca"]', { state: 'visible' });
  assert.deepEqual(api.posts, [{ show_cca_page: false }, { show_cloud_page: false }, { show_cca_page: true }]);
  assert.equal(await activeView(page), 'view-settings');
  await ctx.close();
});

test('a switch whose save fails flips back and says why', async () => {
  const { page, ctx, api } = await open({ hash: '#settings' });
  await page.click('#settings-tab-web');
  await page.waitForFunction(() => document.getElementById('pi-show-cca').checked);
  api.saveStatus = 500;
  await page.click('label:has(#pi-show-cca)');
  await page.waitForFunction(() => /disk full/.test(document.getElementById('pi-pages-status').textContent));
  assert.equal(await page.isChecked('#pi-show-cca'), true);
  assert.equal(await navShown(page, 'cca'), true);
  await ctx.close();
});

test('the ESP32 has no such switches and keeps CCA Info', async () => {
  const { page, ctx } = await open({ linux: false, hash: '#settings' });
  await page.waitForTimeout(500);
  assert.equal(await page.isVisible('#pi-show-cca'), false);
  assert.equal(await navShown(page, 'cca'), true);
  await ctx.close();
});

const tabsShown = (page) => page.$$eval('#settings-tabs [role="tab"]',
  (ts) => ts.filter((t) => t.offsetParent).map((t) => t.textContent));
const tabSelected = (page) => page.$eval('#settings-tabs [aria-selected="true"]', (t) => t.textContent);
const panelShown = (page, name) => page.isVisible(`#settings-${name}`);

test('the Pi\'s Settings opens on Bus, one tab at a time', async () => {
  const { page, ctx, errors } = await open({ hash: '#settings' });
  await page.waitForSelector('#settings-tab-bus', { state: 'visible' });
  assert.deepEqual(await tabsShown(page), ['Bus', 'MQTT', 'Inverters', 'Web UI', 'General', 'Maintenance']);
  assert.equal(await tabSelected(page), 'Bus');
  assert.equal(await page.isVisible('#pi-serial'), true);
  assert.equal(await panelShown(page, 'ha'), false);
  await page.click('#settings-tab-ha');
  assert.equal(await page.isVisible('#pi-mqtt-host'), true);
  assert.equal(await page.isVisible('#pi-serial'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the ESP32\'s Settings has General, YAML and Maintenance', async () => {
  const { page, ctx } = await open({ linux: false, hash: '#settings' });
  await page.waitForSelector('#settings-tab-yaml', { state: 'visible' });
  await page.waitForTimeout(300);
  assert.deepEqual(await tabsShown(page), ['General', 'YAML', 'Maintenance']);
  assert.equal(await tabSelected(page), 'General');
  await page.click('#settings-tab-yaml');
  assert.equal(await page.isVisible('#tools-yaml'), true);
  await ctx.close();
});

test('an old link to Tools opens Settings', async () => {
  const { page, ctx } = await open({ hash: '#tools' });
  await page.waitForFunction(() => document.querySelector('.view.active').id === 'view-settings');
  assert.equal(page.url().endsWith('#settings'), true);
  assert.equal(await page.isVisible('#nav button[data-view="settings"].active'), true);
  await ctx.close();
});

test('Settings reopens on the tab last opened', async () => {
  const { page, ctx } = await open({ hash: '#settings' });
  await page.click('#settings-tab-web');
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForSelector('#settings-tab-web', { state: 'visible' });
  assert.equal(await tabSelected(page), 'Web UI');
  assert.equal(await page.isVisible('#pi-web-port'), true);
  await ctx.close();
});

test('the arrow keys move along the tabs', async () => {
  const { page, ctx } = await open({ hash: '#settings' });
  await page.click('#settings-tab-bus');
  await page.keyboard.press('ArrowLeft');
  assert.equal(await tabSelected(page), 'Maintenance');
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  assert.equal(await tabSelected(page), 'MQTT');
  await ctx.close();
});

test('the Web UI card saves the web settings with the bus ones', async () => {
  const { page, ctx, api } = await open({ hash: '#settings' });
  await page.click('#settings-tab-web');
  page.on('dialog', (d) => d.dismiss());
  await page.fill('#pi-web-port', '8080');
  await page.click('#pi-web-save');
  await page.waitForFunction(() => true);
  await page.waitForTimeout(300);
  const body = api.posts.at(-1);
  assert.equal(body.web_port, 8080);
  assert.equal(body.bus_role, 'listen');
  await ctx.close();
});

// Running the bus, Settings → Bus has two switches' worth: the CCA's way of
// running it, all of a piece, and Allow changes to the TAPs. The CCA's
// parts are no longer switches each; one an older settings file turned off
// is named, with a way to turn them all on.
const CCA_PARTS = { master_startup: true, master_housekeeping: true, master_queries: true, master_optimizer_writes: true,
  master_read_tap_config: true, discovery_auto: true };

test('running the bus, Settings → Bus has one switch: Allow changes to the TAPs', async () => {
  const { page, ctx, api } = await open({ hash: '#settings', pages: { bus_role: 'master', ...CCA_PARTS, master_table_writes: false } });
  await page.click('#settings-tab-bus');
  await page.waitForSelector('#pi-cca-row', { state: 'visible' });
  const switches = await page.$$eval('#settings-bus .cfg-switch input', (is) => is.filter((i) => i.offsetParent).map((i) => i.id));
  assert.deepEqual(switches, ['pi-tablewrites']);
  assert.match(await page.textContent('#pi-tablewrites-row'), /Allow changes to the TAPs/);
  assert.match(await page.textContent('#pi-cca-row'), /Runs the whole bus/);
  assert.equal(await page.isVisible('#pi-cca-all'), false);
  await page.check('#pi-tablewrites');
  page.on('dialog', (d) => d.dismiss());
  await page.click('#pi-port-save');
  await page.waitForTimeout(300);
  const body = api.posts.at(-1);
  assert.equal(body.master_table_writes, true);
  // Saving the card leaves the CCA's parts, and Take on a new TAP, as they are.
  for (const k of [...Object.keys(CCA_PARTS), 'master_adopt_taps']) assert.equal(k in body, false, k);
  await ctx.close();
});

test('a part of the CCA\'s running turned off in the settings file is named, and Turn them all on sends them', async () => {
  const { page, ctx, api } = await open({ hash: '#settings',
    pages: { bus_role: 'master', ...CCA_PARTS, master_queries: false, master_optimizer_writes: false } });
  await page.click('#settings-tab-bus');
  await page.waitForSelector('#pi-cca-row', { state: 'visible' });
  assert.match(await page.textContent('#pi-cca-row'), /Off in the settings file: querying the optimizers, the writes their answers call for/);
  page.on('dialog', (d) => d.accept());
  await page.click('#pi-cca-all');
  await page.waitForTimeout(300);
  assert.deepEqual(api.posts.at(-1), CCA_PARTS);
  await ctx.close();
});

test('listening, neither shows', async () => {
  const { page, ctx } = await open({ hash: '#settings', pages: { bus_role: 'listen' } });
  await page.click('#settings-tab-bus');
  await page.waitForSelector('#pi-bus-role');
  assert.equal(await page.isVisible('#pi-cca-row'), false);
  assert.equal(await page.isVisible('#pi-tablewrites-row'), false);
  await ctx.close();
});

const diagTabs = (page, view) => page.$$eval(`#view-${view} .page-tabs a`,
  (as) => as.filter((a) => a.offsetParent).map((a) => `${a.textContent}${a.getAttribute('aria-selected') === 'true' ? '*' : ''}`));

test('on the Pi, Logs is a tab of Diagnostics, not a sidebar item', async () => {
  const { page, ctx, api, errors } = await open({ hash: '#diagnostics' });
  await page.waitForSelector('#view-diagnostics .page-tabs', { state: 'visible' });
  assert.deepEqual(await diagTabs(page, 'diagnostics'), ['Health*', 'Logs']);
  assert.equal(await page.$('#nav button[data-view="logs"]'), null);
  await page.click('#view-diagnostics .page-tabs a[href="#logs"]');
  await page.waitForFunction(() => document.querySelector('.view.active').id === 'view-logs');
  assert.deepEqual(await diagTabs(page, 'logs'), ['Health', 'Logs*']);
  assert.equal(await page.textContent('#view-logs h1'), 'Diagnostics');
  assert.equal(await page.isVisible('#nav button[data-view="diagnostics"].active'), true);
  await page.waitForFunction(() => document.getElementById('logs-meta').textContent !== '—');
  assert.ok(api.logReads > 0);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the ESP32 has no Logs, so Diagnostics has no tabs', async () => {
  const { page, ctx } = await open({ linux: false, hash: '#diagnostics' });
  await page.waitForTimeout(500);
  assert.deepEqual(await diagTabs(page, 'diagnostics'), []);
  await ctx.close();
});

// ---- the mesh map --------------------------------------------------------

test('bus: the gateway version is the figure, its build details beneath', async () => {
  const { page, ctx, errors } = await open({ master: true });
  await page.click('#nav button[data-view="busmaster"]');
  await page.waitForFunction(() => document.getElementById('diag-tap-version').textContent !== '—');
  assert.equal(await page.textContent('#diag-tap-version'), 'G8.59');
  // A non-breaking hyphen, so the model doesn't wrap as GW- / H1.
  assert.equal(await page.textContent('#diag-tap-version-sub'), 'Mgate · Jan 1 2026 10:00:00 GW\u2011H1');
  assert.equal(await page.getAttribute('#diag-tap-version', 'title'), 'Mgate Version G8.59 Jan  1 2026 10:00:00 GW-H1');
  assert.deepEqual(errors, []);
  await ctx.close();
});

async function openMesh(opts = {}) {
  const o = await open(opts);
  await o.page.click('#nav button[data-view="topology"]');
  await o.page.click('#topology-tabs [data-tab="mesh"]');
  await o.page.waitForSelector('#topology-mesh .mesh-node, #topology-mesh .mesh-empty', { state: 'attached' });
  return o;
}
const meshNode = (page, label) => page.locator(`#mesh-svg .mesh-node[data-label="${label}"]`);
const fixtureNodes = piRoutes['/api/mesh'].nodes;

test('mesh: tab only on the Pi', async () => {
  const { page, ctx } = await open({ linux: false });
  await page.click('#nav button[data-view="topology"]');
  assert.equal(await page.isVisible('#topology-tabs [data-tab="mesh"]'), false);
  assert.equal(await page.isVisible('#topo-tree'), true);
  await ctx.close();
});

test('mesh: the Pi opens Topology on Mesh, first of the tabs, until Layout is picked', async () => {
  const { page, ctx, errors } = await open();
  await page.click('#nav button[data-view="topology"]');
  await page.waitForSelector('#topology-mesh .mesh-node', { state: 'attached' });
  assert.deepEqual(await page.locator('#topology-tabs [role="tab"]').evaluateAll((ts) => ts.map((t) => t.dataset.tab)), ['mesh', 'layout', 'roof']);
  assert.equal(await page.getAttribute('#topology-tab-mesh', 'aria-selected'), 'true');
  assert.equal(await page.isVisible('#topology-layout'), false);
  await page.click('#topology-tabs [data-tab="layout"]');
  await page.reload();
  await page.click('#nav button[data-view="topology"]');
  assert.equal(await page.getAttribute('#topology-tab-layout', 'aria-selected'), 'true');
  assert.equal(await page.isVisible('#topology-mesh'), false);
  // Roof is remembered the same way.
  await page.click('#topology-tabs [data-tab="roof"]');
  await page.reload();
  await page.click('#nav button[data-view="topology"]');
  assert.equal(await page.getAttribute('#topology-tab-roof', 'aria-selected'), 'true');
  assert.equal(await page.isVisible('#topology-layout'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: one node per optimizer around its TAP', async () => {
  const { page, ctx, errors } = await openMesh();
  assert.equal(await page.locator('#mesh-svg .mesh-node').count(), fixtureNodes.length);
  // The TAP by the name it was given on the Devices page, its address beneath.
  assert.deepEqual(await page.locator('#mesh-svg .mesh-tap text.mesh-tap-name').allTextContents(), ['Garage']);
  assert.deepEqual(await page.locator('#mesh-svg .mesh-tap text.mesh-tap-addr').allTextContents(), ['TAP 1201']);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: tiers', async () => {
  const { page, ctx } = await openMesh();
  const tier = (i) => meshNode(page, fixtureNodes[i].label).getAttribute('class');
  assert.match(await tier(4), /tier-poor/);    // 6 % lost
  assert.match(await tier(9), /tier-poor/);    // RSSI 72
  assert.match(await tier(15), /tier-silent/); // 900 s since heard
  assert.match(await tier(0), /tier-good/);
  await ctx.close();
});

test('mesh: side panel', async () => {
  const { page, ctx } = await openMesh();
  const n = fixtureNodes[4];
  await meshNode(page, n.label).hover();
  const side = await page.textContent('#mesh-side');
  assert.match(side, new RegExp(`\\b${n.label}\\b`));
  assert.match(side, new RegExp(`\\b${n.rssi}\\b`));
  assert.match(side, new RegExp(`${n.lost_today.toLocaleString('en-US')} of ${(n.reports_today + n.lost_today).toLocaleString('en-US')} reports`));
  assert.doesNotMatch(side, /dBm/);
  await ctx.close();
});

test('mesh: keyboard focus fills the side panel too', async () => {
  const { page, ctx } = await openMesh();
  await meshNode(page, fixtureNodes[9].label).focus();
  assert.match(await page.textContent('#mesh-side'), new RegExp(`\\b${fixtureNodes[9].label}\\b`));
  await ctx.close();
});

test('mesh: an optimizer joined through another is drawn from it', async () => {
  const { page, ctx, errors } = await openMesh();
  const relay = fixtureNodes[2], via = fixtureNodes[7];
  const link = page.locator(`#mesh-svg .mesh-link[data-addr="${via.addr}"]`);
  assert.equal(await link.getAttribute('data-via'), 'node');
  const dot = meshNode(page, relay.label).locator('.mesh-dot');
  assert.equal(await link.getAttribute('x1'), await dot.getAttribute('cx'));
  assert.equal(await link.getAttribute('y1'), await dot.getAttribute('cy'));
  await meshNode(page, via.label).hover();
  assert.match(await page.textContent('#mesh-side .mesh-via'), new RegExp(`^${relay.label}, another optimizer`));
  await meshNode(page, relay.label).hover();
  assert.match(await page.textContent('#mesh-side'), new RegExp(`Relays for\\s*${via.label}`));
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: an optimizer through a relay sits just beyond it, away from the TAP', async () => {
  // As the daemon sends it: the parent's NWID, and the optimizer holding it.
  const node = (addr, parent, rssi = 150) => ({ addr, label: 'N' + addr, string: '', tap: '1201', parent, rssi,
    relay: parent === '0001' ? null : parent, rssi_24h: Array(288).fill(null), reports_today: 100, lost_today: 0, last_heard_s: 2, last_join_unix: 1 });
  const nodes = [];
  for (let i = 2; i < 14; i++) nodes.push(node(i.toString(16).toUpperCase().padStart(4, '0'), '0001'));
  // Two through 0005, one through one of those, and a pair naming each other.
  nodes.push(node('0010', '0005', 70), node('0011', '0005', 90), node('0012', '0010', 60), node('0013', '0014'), node('0014', '0013'));
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: true, taps: [{ id: '1201' }], nodes } });
  const at = async (addr) => { const d = meshNode(page, 'N' + addr).locator('.mesh-dot');
    return [+await d.getAttribute('cx'), +await d.getAttribute('cy')]; };
  const r = await page.locator('#mesh-svg .mesh-tap rect').first();
  const hub = [+await r.getAttribute('x') + +await r.getAttribute('width') / 2, +await r.getAttribute('y') + +await r.getAttribute('height') / 2];
  const polar = ([x, y]) => [Math.hypot(x - hub[0], y - hub[1]), Math.atan2(y - hub[1], x - hub[0])];
  for (const [child, parent] of [['0010', '0005'], ['0011', '0005'], ['0012', '0010']]) {
    const c = await at(child), p = await at(parent), [rc, ac] = polar(c), [rp, ap] = polar(p);
    assert.ok(rc > rp + 20, `${child} (${rc.toFixed(0)}) beyond ${parent} (${rp.toFixed(0)})`);
    const da = Math.abs(Math.atan2(Math.sin(ac - ap), Math.cos(ac - ap)));
    assert.ok(da < 0.6, `${child} within 0.6 rad of ${parent}'s bearing, was ${da.toFixed(2)}`);
    assert.ok(Math.hypot(c[0] - p[0], c[1] - p[1]) < 80, `${child} close to ${parent}`);
  }
  const [a, b] = [await at('0010'), await at('0011')];
  assert.ok(Math.hypot(a[0] - b[0], a[1] - b[1]) > 18, 'siblings fanned apart');
  assert.equal(await meshNode(page, 'N0013').count(), 1);
  assert.equal(await meshNode(page, 'N0014').count(), 1);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// A parent is a NWID, a different number from an address: one that happens
// to equal some optimizer's address is not that optimizer unless the daemon
// says so.
test('mesh: a parent NWID equal to an address is no relay without the daemon naming one', async () => {
  const node = (addr, parent, relay) => ({ addr, label: 'N' + addr, string: '', tap: '1201', parent, relay, rssi: 150,
    rssi_24h: Array(288).fill(null), reports_today: 100, lost_today: 0, last_heard_s: 2, last_join_unix: 1 });
  const nodes = [node('0002', '0002', null), node('0003', '0002', null), node('0004', '0045', '0003'), node('0005', '0001', null)];
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: true, taps: [{ id: '1201' }], nodes } });
  const via = async (addr) => { await meshNode(page, 'N' + addr).hover(); return page.textContent('#mesh-side .mesh-via'); };
  assert.match(await via('0002'), /^A relay not identified yet \(network ID 0002\)/);
  assert.match(await via('0003'), /^A relay not identified yet \(network ID 0002\)/);
  assert.match(await via('0004'), /^N0003, another optimizer/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: direct, unidentified relay, and not known', async () => {
  const { page, ctx } = await openMesh();
  const via = async (i) => { await meshNode(page, fixtureNodes[i].label).hover(); return page.textContent('#mesh-side .mesh-via'); };
  assert.equal(await page.locator(`#mesh-svg .mesh-link[data-addr="${fixtureNodes[0].addr}"]`).getAttribute('data-via'), 'tap');
  assert.match(await via(0), /^The TAP directly/);
  assert.equal(await page.locator(`#mesh-svg .mesh-link[data-addr="${fixtureNodes[11].addr}"]`).getAttribute('data-via'), 'unknown');
  assert.match(await via(11), /^A relay not identified yet \(network ID 0091\)/);
  assert.match(await via(20), /^Not known: no join heard yet/);
  await ctx.close();
});

test('mesh: last joined says when, with the date once it is not today', async () => {
  const now = Date.now() / 1000;
  const node = (label, join) => ({ addr: label === 'old' ? '0002' : label === 'new' ? '0003' : '0004', label, string: '', tap: '1201', parent: join ? '0001' : null,
    rssi: 120, rssi_24h: Array(288).fill(null), reports_today: 100, lost_today: 0, last_heard_s: 2, last_join_unix: join });
  const nodes = [node('old', now - 3 * 86400), node('new', now - 60), node('never', null)];
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: true, taps: [{ id: '1201' }], nodes } });
  const joined = async (label) => { await meshNode(page, label).hover(); return page.textContent('#mesh-side .mesh-joined'); };
  assert.equal(await joined('never'), 'No join heard yet');
  assert.doesNotMatch(await joined('new'), /[A-Z][a-z]{2}/);
  assert.match(await joined('old'), /[A-Z][a-z]{2} \d+/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: by string puts each string on a row, panels in order', async () => {
  const { page, ctx, errors } = await openMesh();
  await page.click('#mesh-view button[data-mode="strings"]');
  await page.waitForSelector('#mesh-strings .mesh-row');
  assert.equal(await page.getAttribute('#mesh-view button[data-mode="strings"]', 'aria-pressed'), 'true');
  assert.equal(await page.isVisible('#mesh-svg'), false);
  const strings = [...new Set(fixtureNodes.map((n) => n.string || 'Not in a string'))];
  assert.equal(await page.locator('#mesh-strings .mesh-row').count(), strings.length);
  const first = page.locator('#mesh-strings .mesh-row').first();
  const name = await first.locator('.mesh-row-name').textContent();
  const want = fixtureNodes.filter((n) => (n.string || 'Not in a string') === name).map((n) => n.label)
    .sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));
  assert.deepEqual(await first.locator('.mesh-card').evaluateAll((cs) => cs.map((c) => c.dataset.label)), want);
  const card = (i) => page.locator(`#mesh-strings .mesh-card[data-addr="${fixtureNodes[i].addr}"]`);
  assert.equal(await card(7).locator('.mesh-card-via').textContent(), `via ${fixtureNodes[2].label}`);
  assert.equal(await card(11).locator('.mesh-card-via').textContent(), 'via ?');
  assert.equal(await card(0).locator('.mesh-card-via').count(), 0);
  await card(4).click();
  assert.match(await page.textContent('#mesh-side h3'), new RegExp(`^${fixtureNodes[4].label}`));
  assert.equal(await card(4).getAttribute('aria-pressed'), 'true');
  await page.reload();
  await page.click('#nav button[data-view="topology"]');
  await page.click('#topology-tabs [data-tab="mesh"]');
  await page.waitForSelector('#mesh-strings .mesh-row');
  await page.click('#mesh-view button[data-mode="map"]');
  await page.waitForSelector('#mesh-svg .mesh-node');
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: the legend matches the view, lines for the map and dots and tags by string', async () => {
  const { page, ctx, errors } = await openMesh();
  const shown = (sel) => page.locator(`#mesh-legend ${sel}`).first().isVisible();
  assert.equal(await shown('.mesh-sw.via-unknown'), true);
  assert.equal(await shown('.mesh-card-via'), false);
  await page.click('#mesh-view button[data-mode="strings"]');
  await page.waitForSelector('#mesh-strings .mesh-row');
  assert.equal(await shown('.mesh-sw.via-unknown'), false);
  assert.deepEqual(await page.locator('#mesh-legend .mesh-card-via').evaluateAll((es) => es.filter((e) => e.offsetParent).map((e) => e.textContent)), ['via D9', 'via ?']);
  assert.equal(await page.$eval('#mesh-legend .mesh-sw.tier-good', (e) => getComputedStyle(e).borderRadius), '50%');
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: by string marks a string losing over 5% of its reports', async () => {
  const node = (label, string, lost) => ({ addr: (0x10 + parseInt(label.slice(1), 10) + (string === 'S1' ? 0 : 0x20)).toString(16).toUpperCase().padStart(4, '0'),
    label, string, tap: '1201', parent: null, rssi: 150, rssi_24h: Array(288).fill(null),
    reports_today: 100 - lost, lost_today: lost, last_heard_s: 2, last_join_unix: null });
  const nodes = [node('A1', 'S1', 9), node('A2', 'S1', 8), node('A3', 'S1', 0), node('B1', 'S2', 0), node('B2', 'S2', 1)];
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: true, taps: [{ id: '1201' }], nodes } });
  await page.click('#mesh-view button[data-mode="strings"]');
  await page.waitForSelector('#mesh-strings .mesh-row');
  const rows = page.locator('#mesh-strings .mesh-row');
  assert.match(await rows.nth(0).getAttribute('class'), /\bpoor\b/);
  assert.match(await rows.nth(0).locator('.mesh-row-med').textContent(), /^8\.0%/);
  assert.doesNotMatch(await rows.nth(1).getAttribute('class'), /\bpoor\b/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: no reports today', async () => {
  const { page, ctx } = await openMesh();
  await meshNode(page, fixtureNodes[20].label).hover();
  assert.match(await page.textContent('#mesh-side .mesh-loss'), /^—/);
  await ctx.close();
});

test('mesh: empty history', async () => {
  const { page, ctx } = await openMesh();
  await meshNode(page, fixtureNodes[20].label).hover();
  assert.match(await page.textContent('#mesh-side'), /No history yet/);
  await ctx.close();
});

test('mesh: empty', async () => {
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: false, taps: [], nodes: [] } });
  assert.match(await page.textContent('#topology-mesh'), /Waiting for the first reports/);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: the note', async () => {
  const { page, ctx } = await openMesh();
  assert.equal(await page.isVisible('#mesh-note'), true);
  await ctx.close();
});

test('mesh: remembered as the last tab, it is not fetched until Topology opens', async () => {
  const { page, ctx, api } = await open({ init: () => localStorage.setItem('topology-tab', 'mesh') });
  await page.waitForTimeout(1500);
  assert.equal(api.meshReads || 0, 0);
  await page.click('#nav button[data-view="topology"]');
  await page.waitForSelector('#topology-mesh .mesh-node', { state: 'attached' });
  assert.ok(api.meshReads >= 1);
  await ctx.close();
});

test('mesh: a TAP with no name is shown by its address, and named in the side panel', async () => {
  const { page, ctx, errors } = await openMesh({ mesh: { ...piRoutes['/api/mesh'], taps: [{ id: '1201' }] } });
  assert.deepEqual(await page.locator('#mesh-svg .mesh-tap text').allTextContents(), ['TAP 1201']);
  await ctx.close();
  const named = await openMesh();
  await meshNode(named.page, fixtureNodes[0].label).hover();
  assert.match(await named.page.textContent('#mesh-side'), /Reports through\s*Garage \(TAP 1201\)/);
  assert.deepEqual([...errors, ...named.errors], []);
  await named.ctx.close();
});

// The roof's mesh after a TAP restart, 2026-11-09: 17 relays, some in chains
// of three, the rest straight to the TAP or through one not named yet. Each
// relay's optimizers fan out in its own slice of the circle, so no two lines
// cross and none runs through another optimizer.
test('mesh: relays build outward without lines crossing or running through a dot', async () => {
  const relays = { A1: 'A10', A2: 'A9', A7: 'A2', A8: 'C3', B1: 'C8', B2: 'B8', B5: 'A10', B6: 'B8', B7: 'B1', B9: 'B8',
                   C10: 'C5', C4: 'D6', C7: 'C9', C8: 'C6', D6: 'D9', D7: 'D2', D8: 'D2' };
  const unknown = { B10: '000E', B4: '0039', C2: '002F', D3: '0098' };
  const labels = [], addr = {};
  'ABCD'.split('').forEach((s, k) => { for (let i = 1; i <= 10; i++) { const l = s + i; labels.push(l); addr[l] = (k * 10 + i + 1).toString(16).toUpperCase().padStart(4, '0'); } });
  const nodes = labels.map((l, i) => ({ addr: addr[l], label: l, string: 'String ' + l[0], tap: '1201',
    parent: relays[l] ? '00' + (0x40 + i).toString(16).toUpperCase() : unknown[l] || '0001', relay: relays[l] ? addr[relays[l]] : null,
    rssi: 80 + (i * 37) % 130, rssi_24h: Array(288).fill(null), reports_today: 100, lost_today: 0, last_heard_s: 2, last_join_unix: 1 }));
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: true, taps: [{ id: '1201' }], nodes } });
  const segs = await page.$$eval('#mesh-svg .mesh-link', (ls) => ls.map((l) => [l.dataset.addr,
    l.x1.baseVal.value, l.y1.baseVal.value, l.x2.baseVal.value, l.y2.baseVal.value]));
  const dots = await page.$$eval('#mesh-svg .mesh-node', (gs) => gs.map((g) => { const c = g.querySelector('.mesh-dot');
    return [g.dataset.addr, c.cx.baseVal.value, c.cy.baseVal.value]; }));
  assert.equal(segs.length, 40);
  const cross = (a, b) => {
    const o = (p, q, r) => Math.sign((q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]));
    const [p1, p2, p3, p4] = [[a[1], a[2]], [a[3], a[4]], [b[1], b[2]], [b[3], b[4]]];
    const same = (p, q) => Math.hypot(p[0] - q[0], p[1] - q[1]) < 0.5;
    if ([p3, p4].some((p) => same(p, p1) || same(p, p2))) return false;  // they meet at a dot
    return o(p1, p2, p3) * o(p1, p2, p4) < 0 && o(p3, p4, p1) * o(p3, p4, p2) < 0;
  };
  const crossings = [];
  for (let i = 0; i < segs.length; i++) for (let j = i + 1; j < segs.length; j++) if (cross(segs[i], segs[j])) crossings.push(segs[i][0] + '×' + segs[j][0]);
  assert.deepEqual(crossings, []);
  // A line comes no nearer than 9 px to a dot it doesn't end at.
  const near = [];
  for (const s of segs) for (const [a, x, y] of dots) {
    const [, x1, y1, x2, y2] = s, dx = x2 - x1, dy = y2 - y1;
    if (Math.hypot(x - x1, y - y1) < 0.5 || Math.hypot(x - x2, y - y2) < 0.5) continue;
    const t = Math.max(0, Math.min(1, ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)));
    if (Math.hypot(x - x1 - t * dx, y - y1 - t * dy) < 9) near.push(s[0] + '→' + a);
  }
  assert.deepEqual(near, []);
  let closest = Infinity;
  for (let i = 0; i < dots.length; i++) for (let j = i + 1; j < dots.length; j++)
    closest = Math.min(closest, Math.hypot(dots[i][1] - dots[j][1], dots[i][2] - dots[j][2]));
  assert.ok(closest >= 16, `two dots ${closest.toFixed(1)} px apart`);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// Fitted as it first opens, not only once the window is resized: from the
// menu, and from a link straight to Topology.
test('mesh: the radio map is fitted to the window as it opens', async () => {
  for (const hash of ['', '#topology']) {
    const { page, ctx, errors } = hash ? await open({ hash }) : await openMesh();
    await page.waitForSelector('#mesh-svg .mesh-node');
    await page.waitForTimeout(300);
    const legend = await page.$eval('#mesh-legend', (e) => e.getBoundingClientRect().bottom);
    assert.ok(legend <= 900 && legend >= 860, `legend ends at ${legend} in a 900 px window, opened ${hash || 'from the menu'}`);
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

// The map fills the window: its box runs down to leave the legend just in
// view, on a short window and a tall one.
test('mesh: the radio map fills the height of the window, legend in view', async () => {
  for (const h of [800, 1300]) {
    const { page, ctx, errors } = await openMesh();
    await page.setViewportSize({ width: 1400, height: h });
    await page.waitForTimeout(200);
    const legend = await page.$eval('#mesh-legend', (e) => e.getBoundingClientRect().bottom);
    assert.ok(legend <= h && legend >= h - 40, `legend ends at ${legend} in a ${h} px window`);
    const svg = await page.$eval('#mesh-svg', (s) => s.getBoundingClientRect().height);
    assert.ok(svg >= h - 420, `map ${svg} px tall in a ${h} px window`);
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

// Each string's share of reports lost, by day: a row per string, a day per
// column, coloured as the map is.
test('mesh: reports lost by day, a row per string', async () => {
  const { page, ctx, errors } = await openMesh();
  const days = piRoutes['/api/mesh'].loss_days;
  const strings = Object.keys(days[0].strings);
  assert.equal(await page.locator('#mesh-days .mesh-day-row:not(.total)').count(), strings.length);
  const row = page.locator('#mesh-days .mesh-day-row').nth(1);
  assert.equal((await row.locator('.mesh-day-name').textContent()).trim(), strings[1]);
  const cells = await row.locator('.mesh-day-cell').allTextContents();
  assert.deepEqual(cells.map((c) => c.trim()), ['7.4%', '7.4%', '7.4%', '7.4%', '2.4%']);
  assert.deepEqual(await row.locator('.mesh-day-cell').evaluateAll((cs) => cs.map((c) => c.className.replace('mesh-day-cell ', ''))),
    ['tier-poor', 'tier-poor', 'tier-poor', 'tier-poor', 'tier-fair']);
  assert.equal(await page.locator('#mesh-days .mesh-day-head span').count(), days.length + 1);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// And the whole array under the strings: every string's reports summed, so
// no row reads as a total that isn't one.
test('mesh: reports lost by day, with the whole array summed under the strings', async () => {
  const { page, ctx, errors } = await openMesh();
  const days = piRoutes['/api/mesh'].loss_days;
  const rows = page.locator('#mesh-days .mesh-day-row');
  assert.equal(await rows.count(), Object.keys(days[0].strings).length + 1);
  const total = rows.last();
  assert.equal(await total.getAttribute('class'), 'mesh-day-row total');
  assert.equal((await total.locator('.mesh-day-name').textContent()).trim(), 'Whole array');
  const want = days.map((d) => {
    const [ok, lost] = Object.values(d.strings).reduce((a, c) => [a[0] + c[0], a[1] + c[1]], [0, 0]);
    return (100 * lost / (ok + lost)).toFixed(1) + '%';
  });
  assert.deepEqual((await total.locator('.mesh-day-cell').allTextContents()).map((c) => c.trim()), want);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// An optimizer's joins over the last month, in its side panel.
test('mesh: the side panel lists the joins it made, and through whom', async () => {
  const { page, ctx, errors, api } = await openMesh();
  await meshNode(page, fixtureNodes[7].label).click();
  await page.waitForFunction(() => document.querySelectorAll('#mesh-side .mesh-joins li').length === 3);
  const items = await page.locator('#mesh-side .mesh-joins li').allTextContents();
  assert.equal(items.length, 3);
  assert.match(items[0], new RegExp(`via ${fixtureNodes[2].label}`));
  assert.match(items[1], /the TAP/);
  assert.match(items[2], /a relay not identified yet/);
  assert.ok(api.joinReads.includes(fixtureNodes[7].addr));
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('mesh: six TAPs side by side, each optimizer clear of its hub', async () => {
  const nodes = [];
  const taps = ['1201', '1202', '1203', '1204', '1205', '1206'];
  taps.forEach((t, k) => { for (let i = 0; i < 8; i++) nodes.push({
    addr: (k * 16 + i + 2).toString(16).toUpperCase().padStart(4, '0'), label: `T${k}-${i}`, string: '', tap: t, parent: null,
    rssi: 90 + i * 10, rssi_24h: Array(288).fill(null), reports_today: 100, lost_today: 0, last_heard_s: 2, last_join_unix: null }); });
  const { page, ctx, errors } = await openMesh({ mesh: { loss_method: 'slot', parent_confirmed: false, taps: taps.map((id) => ({ id })), nodes } });
  const gaps = await page.$$eval('#mesh-svg .mesh-link', (ls) => ls.map((l) => Math.hypot(l.x2.baseVal.value - l.x1.baseVal.value, l.y2.baseVal.value - l.y1.baseVal.value)));
  assert.equal(gaps.length, nodes.length);
  assert.ok(Math.min(...gaps) >= 40, `closest optimizer ${Math.min(...gaps).toFixed(1)} from its hub`);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('on the ESP32 a #logs link polls nothing (it has no /api/logs)', async () => {
  const { page, ctx, api } = await open({ linux: false, hash: '#logs' });
  await page.waitForTimeout(4500);
  assert.equal(api.logReads || 0, 0);
  await ctx.close();
});

test('on the Pi a #logs link still polls the log', async () => {
  const { page, ctx, api } = await open({ hash: '#logs' });
  await page.waitForTimeout(2500);
  assert.ok((api.logReads || 0) >= 1);
  await ctx.close();
});
