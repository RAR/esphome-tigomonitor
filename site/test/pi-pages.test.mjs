// site/test/pi-pages.test.mjs
// The Settings page's tabs, and the Raspberry Pi build's switches for the CCA
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
    if (path === '/api/cloud/status') return json(200, { configured: true });
    if (path === '/api/cloud/health') return json(200, { summary: {} });
    if (routes[path] !== undefined) return json(200, routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
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
