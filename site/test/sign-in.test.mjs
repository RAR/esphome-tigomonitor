// site/test/sign-in.test.mjs
// Signing in on the Raspberry Pi build, in the real app.html, in Chromium,
// against a mock API: with a password set the API answers 401
// {"sign_in": true} and the page shows its own sign-in (never the browser's
// popup); a wrong password says so, the right one opens the app, and Sign
// out returns to the sign-in. An ESP32's 401 (no sign_in) shows no sign-in.
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

async function open({ signInField = true } = {}) {
  const api = { signedIn: false, logins: [], logouts: 0 };
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (path === '/api/login') {
      const body = JSON.parse(req.postData());
      api.logins.push(body);
      if (body.username === 'admin' && body.password === 'right') {
        api.signedIn = true;
        return json(200, { signed_in: true });
      }
      return json(401, { error: "That username and password don't match." });
    }
    if (path === '/api/logout') { api.logouts++; api.signedIn = false; return json(200, { signed_in: false }); }
    if (!api.signedIn) return json(401, signInField ? { error: 'Sign in first.', sign_in: true } : { error: 'Unauthorized' });
    if (path === '/api/status') return json(200, { ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/settings') return json(200, { ...(routes['/api/settings'] || {}), web_username: 'admin', web_password_set: true,
      mqtt: { host: '', port: 1883, username: '', password_set: false, discovery: true, discovery_prefix: 'homeassistant', base_topic: 'tigomonitor' } });
    return json(200, routes[path] !== undefined ? routes[path] : {});
  });
  const page = await ctx.newPage();
  const errors = [], native = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('dialog', (d) => { native.push(d.message()); d.dismiss(); });
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'networkidle' });
  return { page, ctx, api, errors, native };
}

test('the page asks to sign in; a wrong password says so, the right one opens the app', async () => {
  const { page, ctx, api, errors, native } = await open();
  await page.waitForSelector('#signin', { state: 'visible' });
  assert.equal(await page.isVisible('.app'), false);
  assert.equal(await page.evaluate(() => document.activeElement.id), 'signin-user');
  assert.equal(await page.getAttribute('#signin-pass', 'autocomplete'), 'current-password');
  await page.fill('#signin-user', 'admin');
  await page.fill('#signin-pass', 'wrong');
  await page.click('#signin-go');
  await page.waitForFunction(() => document.getElementById('signin-error').textContent !== '');
  assert.equal(await page.textContent('#signin-error'), "That username and password don't match.");
  assert.equal(await page.inputValue('#signin-pass'), '');
  assert.equal(await page.evaluate(() => document.activeElement.id), 'signin-pass');
  await page.fill('#signin-pass', 'right');
  await Promise.all([page.waitForNavigation(), page.press('#signin-pass', 'Enter')]);
  await page.waitForSelector('.app', { state: 'visible' });
  assert.equal(await page.isVisible('#signin'), false);
  assert.deepEqual(api.logins.map((l) => l.password), ['wrong', 'right']);
  assert.deepEqual(native, []);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('Sign out, in Settings, ends the session and the page asks again', async () => {
  const { page, ctx, api, errors } = await open();
  await page.fill('#signin-user', 'admin');
  await page.fill('#signin-pass', 'right');
  await Promise.all([page.waitForNavigation(), page.click('#signin-go')]);
  // A fresh load at #settings, as the Pi pages' tests open it: a hash change
  // on the loaded page can run before the page knows it's on a Pi.
  await page.goto('about:blank');
  await page.goto(`http://127.0.0.1:${port}/#settings`, { waitUntil: 'networkidle' });
  await page.click('#settings-tab-web');
  await page.waitForSelector('#pi-web-signout', { state: 'visible' });
  await Promise.all([page.waitForNavigation(), page.click('#pi-web-signout')]);
  await page.waitForSelector('#signin', { state: 'visible' });
  assert.equal(api.logouts, 1);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test("an ESP32's 401 (no sign_in) shows no sign-in", async () => {
  const { page, ctx } = await open({ signInField: false });
  await page.waitForTimeout(500);
  assert.equal(await page.isVisible('#signin'), false);
  await ctx.close();
});
