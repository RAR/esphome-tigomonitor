// site/test/error-words.test.mjs
// A failed request reads as words, not as "HTTP 500", "/api/settings 400"
// or the browser's "Failed to fetch". The server's own reason comes first;
// with none, the status in words with its number in brackets.
// In the real app.html, in Chromium, against the fixtures.
import { test, before, after, afterEach } from 'node:test';
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
  server = createServer((_q, res) => { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); res.end(html); })
    .listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => { await browser.close(); server.close(); });
afterEach(async () => { await Promise.all(browser.contexts().map((c) => c.close())); });

// The ESP32 fixtures, with one path answered by `broken`.
async function open(view, path, broken) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', (route) => {
    const p = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    if (p === path) return broken(route);
    const body = routes[p];
    return body === undefined ? route.fulfill({ status: 404, body: 'no' })
      : route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  const page = await ctx.newPage();
  await page.goto(`http://127.0.0.1:${port}/#${view}`);
  return page;
}
const metaSays = (page, id, re) =>
  page.waitForFunction(([i, s]) => new RegExp(s).test(document.getElementById(i).textContent), [id, re.source], { timeout: 4000 })
    .then(() => page.textContent('#' + id));

test('no answer at all reads as "TigoMonitor didn\'t answer"', async () => {
  const page = await open('dashboard', '/api/devices', (r) => r.abort('connectionrefused'));
  const meta = await metaSays(page, 'dash-meta', /Couldn't refresh/);
  assert.equal(meta, "Couldn't refresh: TigoMonitor didn't answer");
});

test('a 500 with no reason reads as the status in words, not a JSON parse error', async () => {
  const page = await open('dashboard', '/api/devices', (r) => r.fulfill({ status: 500, body: 'oops' }));
  const meta = await metaSays(page, 'dash-meta', /Couldn't refresh/);
  assert.equal(meta, "Couldn't refresh: TigoMonitor hit an error (500)");
});

test("the server's own reason is shown as it is", async () => {
  const page = await open('dashboard', '/api/devices',
    (r) => r.fulfill({ status: 503, contentType: 'application/json', body: JSON.stringify({ error: 'monitor not bound' }) }));
  const meta = await metaSays(page, 'dash-meta', /Couldn't refresh/);
  assert.equal(meta, "Couldn't refresh: monitor not bound");
});

test('an error toast is announced and stays long enough to read', async () => {
  const page = await open('dashboard', '/none', () => {});
  await page.evaluate(() => showToast('Not saved: refused (400)', 'err'));
  assert.equal(await page.getAttribute('#app-toast', 'role'), 'status');
  await page.waitForTimeout(3000);
  assert.match(await page.getAttribute('#app-toast', 'class'), /show/);
});
