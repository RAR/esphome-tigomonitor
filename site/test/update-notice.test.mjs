// site/test/update-notice.test.mjs
// The sidebar's notice of a newer TigoMonitor release, in the real app.html,
// in Chromium: /api/status gives the running version, and GitHub's
// latest-release API is answered here (the browser has no other DNS).
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

const RELEASE_API = 'https://api.github.com/repos/RAR/esphome-tigomonitor/releases/latest';

// opts.running: the device's version. opts.latest: GitHub's tag_name, or a
// status number for a failed answer. opts.storage: localStorage to start with.
async function open(opts = {}) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const api = { github: 0 };
  await ctx.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (body) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/status') return json({ ...routes['/api/status'], tigomonitor_version: opts.running });
    if (routes[path] !== undefined) return json(routes[path]);
    return route.fulfill({ status: 404, body: 'no fixture' });
  });
  await ctx.route(RELEASE_API, async (route) => {
    api.github++;
    if (typeof opts.latest === 'number') return route.fulfill({ status: opts.latest, body: '{}' });
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ tag_name: opts.latest, html_url: `https://github.com/RAR/esphome-tigomonitor/releases/tag/${opts.latest}` }) });
  });
  if (opts.storage) await ctx.addInitScript((s) => { for (const [k, v] of Object.entries(s)) localStorage.setItem(k, v); }, opts.storage);
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/app`, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => document.getElementById('update-status').textContent !== '');
  return { page, ctx, api, errors };
}

const notice = (page) => page.$eval('#app-update', (el) => ({
  shown: !el.hidden, text: el.textContent.trim(), href: el.querySelector('a').getAttribute('href'),
}));
const status = (page) => page.$eval('#update-status', (el) => el.textContent);

test('a newer release shows under the version, linked to its notes', async () => {
  const { page, ctx, api, errors } = await open({ running: '2.3.0', latest: 'v2.4.0' });
  assert.deepEqual(await notice(page), {
    shown: true, text: 'v2.4.0 available', href: 'https://github.com/RAR/esphome-tigomonitor/releases/tag/v2.4.0',
  });
  assert.match(await status(page), /^v2\.4\.0 is available/);
  assert.equal(api.github, 1);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('the same release, or an older one, shows nothing', async () => {
  for (const latest of ['v2.3.0', 'v2.2.0']) {
    const { page, ctx, errors } = await open({ running: '2.3.0', latest });
    assert.equal((await notice(page)).shown, false);
    assert.match(await status(page), new RegExp(`^Up to date: the latest release is ${latest.replaceAll('.', '\\.')}`));
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

test('a build past a release is told only of a later one', async () => {
  let o = await open({ running: '2.3.0+3.g838855d', latest: 'v2.3.0' });
  assert.equal((await notice(o.page)).shown, false);
  await o.ctx.close();
  o = await open({ running: '2.3.0+3.g838855d', latest: 'v2.3.1' });
  assert.equal((await notice(o.page)).text, 'v2.3.1 available');
  await o.ctx.close();
});

test('a release candidate is told of its release', async () => {
  const { page, ctx } = await open({ running: '2.4.0-rc.1', latest: 'v2.4.0' });
  assert.equal((await notice(page)).text, 'v2.4.0 available');
  await ctx.close();
});

test('a branch build, or a device with no version, never asks GitHub', async () => {
  for (const running of ['main · 8632d50', undefined]) {
    const { page, ctx, api, errors } = await open({ running, latest: 'v9.9.9' });
    assert.equal(api.github, 0);
    assert.equal((await notice(page)).shown, false);
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

test('GitHub is asked at most once a day: a fresh answer is reused', async () => {
  const fresh = JSON.stringify({ tag: 'v2.4.0', at: Date.now() - 3600 * 1000 });
  const { page, ctx, api } = await open({ running: '2.3.0', latest: 'v2.5.0', storage: { tigoLatestRelease: fresh } });
  assert.equal(api.github, 0);
  assert.equal((await notice(page)).text, 'v2.4.0 available');
  await ctx.close();
  const stale = JSON.stringify({ tag: 'v2.4.0', at: Date.now() - 25 * 3600 * 1000 });
  const o = await open({ running: '2.3.0', latest: 'v2.5.0', storage: { tigoLatestRelease: stale } });
  assert.equal(o.api.github, 1);
  assert.equal((await notice(o.page)).text, 'v2.5.0 available');
  await o.ctx.close();
});

test('GitHub failing, or answering with junk, shows nothing and breaks nothing', async () => {
  for (const latest of [403, 500, '<img src=x onerror=alert(1)>', 'main']) {
    const { page, ctx, errors } = await open({ running: '2.3.0', latest });
    assert.equal((await notice(page)).shown, false);
    assert.match(await status(page), /^GitHub couldn't be reached/);
    assert.deepEqual(errors, []);
    await ctx.close();
  }
});

test('Off in Settings › Maintenance stops the check; On starts it again', async () => {
  const { page, ctx, api } = await open({ running: '2.3.0', latest: 'v2.4.0', storage: { tigoUpdateCheck: 'off' } });
  assert.equal(api.github, 0);
  assert.equal((await notice(page)).shown, false);
  assert.equal(await status(page), 'Off.');
  await page.click('button[data-view="settings"]');
  await page.click('#settings-tab-maintenance');
  assert.equal(await page.isChecked('#update-check'), false);
  await page.check('#update-check');
  await page.waitForFunction(() => !document.getElementById('app-update').hidden);
  assert.equal(api.github, 1);
  assert.equal(await page.evaluate(() => localStorage.getItem('tigoUpdateCheck')), null);
  await page.uncheck('#update-check');
  assert.equal((await notice(page)).shown, false);
  assert.equal(await page.evaluate(() => localStorage.getItem('tigoUpdateCheck')), 'off');
  await ctx.close();
});
