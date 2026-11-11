// site/test/dashboard-first-load.test.mjs
// The Dashboard's first refresh runs as the page loads. It read PLATFORM
// before the line that declares it, threw "Cannot access 'PLATFORM' before
// initialization", and showed nothing until the next refresh 5 s later.
// In the real app.html, in Chromium, against the fixtures, ESP32 and Pi.
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
afterEach(async () => { await Promise.all(browser.contexts().map((c) => c.close())); });

async function firstLoad(pi) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', (route) => {
    const p = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    let body = (pi ? piRoutes[p] : undefined) ?? routes[p];
    if (p === '/api/status') body = { ...routes[p], ...(pi ? { platform: 'linux' } : {}) };
    return body === undefined ? route.fulfill({ status: 404, body: 'no' })
      : route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(`http://127.0.0.1:${port}/#dashboard`);
  // Well inside the 5 s refresh: the first refresh must have worked.
  await page.waitForFunction(() => /last update/.test(document.getElementById('dash-meta').textContent), null, { timeout: 2500 });
  const meta = await page.textContent('#dash-meta');
  return { meta, errors, ctx };
}

for (const pi of [false, true]) {
  test(`the Dashboard's first refresh shows data at once (${pi ? 'Pi' : 'ESP32'})`, async () => {
    const { meta, errors, ctx } = await firstLoad(pi);
    assert.doesNotMatch(meta, /refresh error/);
    assert.deepEqual(errors.filter((e) => /PLATFORM|before initialization/.test(e)), []);
    await ctx.close();
  });
}
