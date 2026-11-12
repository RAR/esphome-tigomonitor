// site/test/ui-dialog.test.mjs
// The app's own dialog in place of the browser's confirm() and alert(), in
// the real app.html, in Chromium, against the screenshot fixtures: it asks,
// Cancel, Escape and a click outside each send nothing, the action sends,
// a red action starts on Cancel, and on a phone it is a sheet at the bottom.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { routes } from '../screenshots/fixtures.mjs';
import { answer } from './ui-dialog.mjs';

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

async function open(viewport = { width: 1280, height: 900 }) {
  const api = { posts: [] };
  const ctx = await browser.newContext({ viewport });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    if (req.method() === 'POST') api.posts.push(path);
    const body = routes[path] !== undefined ? routes[path] : {};
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  const page = await ctx.newPage();
  const errors = [], native = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('dialog', (d) => { native.push(d.message()); d.dismiss(); });
  await page.goto(`http://127.0.0.1:${port}/#settings`, { waitUntil: 'networkidle' });
  await page.click('#settings-tab-maintenance');
  await page.waitForSelector('#tools-reset-peak', { state: 'visible' });
  return { page, ctx, api, errors, native };
}

test('asking first: Cancel, Escape and a click outside send nothing; the action sends', async () => {
  const { page, ctx, api, errors, native } = await open();
  await page.click('#tools-reset-peak');
  await page.waitForSelector('#ui-dialog[open]');
  assert.equal(await page.getAttribute('#ui-dialog', 'role'), 'alertdialog');
  assert.equal(await page.textContent('#ui-dialog h3'), 'Reset every peak-power value?');
  assert.equal(await page.textContent('#ui-dialog [data-answer="ok"]'), 'Reset peaks');
  assert.match(await page.getAttribute('#ui-dialog [data-answer="ok"]', 'class'), /\bdanger\b/);
  // A red action starts on Cancel, so Enter doesn't take it.
  assert.equal(await page.evaluate(() => document.activeElement.dataset.answer), 'cancel');
  await page.keyboard.press('Escape');
  await page.waitForSelector('#ui-dialog:not([open])', { state: 'attached' });
  await page.click('#tools-reset-peak');
  await page.waitForSelector('#ui-dialog[open]');
  await page.mouse.click(8, 8);  // the backdrop
  await page.waitForSelector('#ui-dialog:not([open])', { state: 'attached' });
  // A click inside the dialog's padding isn't outside.
  await page.click('#tools-reset-peak');
  const box = await (await page.$('#ui-dialog')).boundingBox();
  await page.mouse.click(box.x + 6, box.y + 6);
  assert.equal(await page.$eval('#ui-dialog', (d) => d.open), true);
  await answer(page, false);
  await page.waitForTimeout(200);
  assert.deepEqual(api.posts.filter((p) => p === '/api/reset_peak_power'), []);
  await page.click('#tools-reset-peak');
  await answer(page, true);
  await page.waitForFunction(() => document.getElementById('app-toast').textContent.length > 0);
  assert.deepEqual(api.posts.filter((p) => p === '/api/reset_peak_power'), ['/api/reset_peak_power']);
  assert.deepEqual(native, []);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('on a phone the question is a sheet along the bottom, its buttons full width', async () => {
  const { page, ctx, errors } = await open({ width: 390, height: 800 });
  await page.click('#tools-reset-peak');
  await page.waitForSelector('#ui-dialog[open]');
  await page.waitForTimeout(250);  // its entrance
  const box = await (await page.$('#ui-dialog')).boundingBox();
  assert.ok(Math.abs(box.y + box.height - 800) <= 1, `bottom at ${box.y + box.height}`);
  assert.ok(Math.abs(box.width - 390) <= 1, `width ${box.width}`);
  const ok = await (await page.$('#ui-dialog [data-answer="ok"]')).boundingBox();
  assert.ok(ok.width > 300, `button ${ok.width}`);
  await answer(page, false);
  assert.deepEqual(errors, []);
  await ctx.close();
});
