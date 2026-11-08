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

test('the bus master can be asked to read the TAP\'s radio settings, off unless turned on', async () => {
  const { page, ctx, api } = await open({ hash: '#settings', pages: { bus_role: 'master', master_read_tap_config: false } });
  await page.click('#settings-tab-bus');
  await page.waitForSelector('#pi-tapcfg-row', { state: 'visible' });
  assert.equal(await page.isChecked('#pi-tapcfg'), false);
  await page.check('#pi-tapcfg');
  page.on('dialog', (d) => d.dismiss());
  await page.click('#pi-port-save');
  await page.waitForTimeout(300);
  assert.equal(api.posts.at(-1).master_read_tap_config, true);
  await ctx.close();
});

test('automatic discovery is a switch on the Bus card, off unless turned on', async () => {
  const { page, ctx, api } = await open({ hash: '#settings', pages: { bus_role: 'master', discovery_auto: false } });
  await page.click('#settings-tab-bus');
  await page.waitForSelector('#pi-discauto-row', { state: 'visible' });
  assert.equal(await page.isChecked('#pi-discauto'), false);
  await page.check('#pi-discauto');
  page.on('dialog', (d) => d.dismiss());
  await page.click('#pi-port-save');
  await page.waitForTimeout(300);
  assert.equal(api.posts.at(-1).discovery_auto, true);
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

test('mesh: one node per optimizer around its TAP', async () => {
  const { page, ctx, errors } = await openMesh();
  assert.equal(await page.locator('#mesh-svg .mesh-node').count(), fixtureNodes.length);
  assert.deepEqual(await page.locator('#mesh-svg .mesh-tap').allTextContents(), ['TAP 1201']);
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
