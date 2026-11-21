// site/test/mobile-sidebar.test.mjs
// The sidebar on a phone, in the real app.html, in Chromium: opened from the
// menu button on a screen shorter than the sidebar, its bottom (connection,
// links, version) must be reachable by scrolling the sidebar.
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
  browser = await chromium.launch({ args: ['--no-sandbox', '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1'] });
});

after(async () => {
  await browser.close();
  server.close();
});

async function open(viewport) {
  const ctx = await browser.newContext({ viewport, isMobile: true, hasTouch: true });
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    if (routes[path] !== undefined) {
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(routes[path]) });
    }
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/app`, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => !document.getElementById('app-version').hidden);
  return { page, ctx, errors };
}

test('on a short phone screen the sidebar scrolls to its version line', async () => {
  const { page, ctx, errors } = await open({ width: 390, height: 560 });
  await page.click('#mobile-menu-btn');
  await page.waitForTimeout(300); // the slide-in
  const r = await page.$eval('aside', (aside) => {
    aside.scrollTop = aside.scrollHeight;
    const bottom = (sel) => document.querySelector(sel).getBoundingClientRect().bottom;
    return { taller: aside.scrollHeight > aside.clientHeight, conn: bottom('.conn'), version: bottom('#app-version') };
  });
  assert.ok(r.taller, 'the test screen should be shorter than the sidebar');
  assert.ok(r.conn <= 560 && r.version <= 560, `bottom rows below the screen: ${JSON.stringify(r)}`);
  assert.deepEqual(errors, []);
  await ctx.close();
});
