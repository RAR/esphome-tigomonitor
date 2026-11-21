// site/test/ha-ingress.test.mjs
// The page as a Home Assistant app shows it: under the ingress path
// (/api/hassio_ingress/<token>/), every API call must stay under that path,
// or it reaches Home Assistant's own API. And with HA doing the sign-in
// (via_ingress), Settings has no Sign out to offer.
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
const INGRESS = '/api/hassio_ingress/Zx9TokenForTests/';

let server, port, browser;

before(async () => {
  server = createServer((_q, res) => {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
  }).listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox', '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1'] });
});

after(async () => {
  await browser.close();
  server.close();
});

async function open(hash = '') {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const calls = [];
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    if (req.resourceType() === 'document') return route.continue();
    const full = new URL(req.url()).pathname;
    calls.push(full);
    const path = full.replace(/^.*(\/api\/)/, '$1');
    const json = (body) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json({ ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/settings') return json({ ...(routes['/api/settings'] || {}), web_username: 'admin', web_password_set: true,
      via_ingress: true, mqtt: { host: 'core-mosquitto', port: 1883, username: 'addons', password_set: true, discovery: true,
        discovery_prefix: 'homeassistant', base_topic: 'tigomonitor' } });
    return json(routes[path] !== undefined ? routes[path] : {});
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}${INGRESS}${hash}`, { waitUntil: 'networkidle' });
  return { page, ctx, calls, errors };
}

test('under the ingress path, every API call stays under it', async () => {
  const { page, ctx, calls, errors } = await open();
  await page.waitForSelector('.app', { state: 'visible' });
  for (const view of ['nodes', 'topology', 'diagnostics', 'settings']) {
    await page.click(`button[data-view="${view}"]`);
    await page.waitForLoadState('networkidle');
  }
  assert.ok(calls.length > 5, `the page made only ${calls.length} API calls`);
  assert.deepEqual(calls.filter((c) => !c.startsWith(INGRESS + 'api/')), []);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('signed in by Home Assistant, Settings offers no Sign out', async () => {
  const { page, ctx, errors } = await open('#settings');
  await page.click('#settings-tab-web');
  await page.waitForFunction(() => document.getElementById('pi-web-pass').placeholder === 'unchanged');
  assert.equal(await page.isVisible('#pi-web-signout'), false);
  assert.deepEqual(errors, []);
  await ctx.close();
});
