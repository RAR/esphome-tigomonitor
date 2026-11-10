// site/test/setup.test.mjs
// System setup (Raspberry Pi): replacing an optimizer step by step against a
// mocked bus master, which answers the check (a dry run), the replace, the
// wait and the join the way /api/master reports them.
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
  server = createServer((_q, res) => { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); res.end(html); })
    .listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  port = server.address().port;
  browser = await chromium.launch({ args: ['--no-sandbox'] });
});
after(async () => { await browser.close(); server.close(); });

const NEW_MAC = '04C05B0000001234';  // printed 0-1234R
const now = () => Math.floor(Date.now() / 1000);

// A2 (0005) has gone quiet; the master runs the bus with table writes on.
function world(writes = true) {
  const devices = routes['/api/devices'].devices.map((d) => d.addr === '0005' ? { ...d, data_age_ms: 3 * 3600e3, stale: true } : d);
  const nodes = routes['/api/nodes'].nodes;
  const old = nodes.find((n) => n.addr === '0005');
  const edit = { kind: '', active: false, finished: false, ok: false, dry_run: false, mac: '', label: '', id: '', ack: -1,
                 step: '', message: '', started_unix: 0, finished_unix: 0, dry_frames: [], interrupted: false };
  const master = { ...piRoutes['/api/master'], role: 'master', active: true,
                   table: { writes, edit, waiting: [], joins: [] },
                   discovery: { running: false, remaining_s: 0, node_count: 24, joined: 24, in_table: 24 }, discovery_read: true };
  return { devices, nodes, old, master, posts: [] };
}

async function open(w) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  await ctx.route('**/api/**', async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace(/^.*(\/api\/)/, '$1');
    const json = (status, body) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    if (req.method() === 'POST' && path.startsWith('/api/master/table/')) {
      const b = JSON.parse(req.postData());
      w.posts.push({ path, ...b });
      const t = w.master.table;
      if (path.endsWith('/replace')) {
        const frames = ['2BBABE', '290A…', '290A…', '2904…' + NEW_MAC + '8005'];  // 23 others and the new one
        t.edit = { ...t.edit, kind: 'replace', active: false, finished: true, ok: true, dry_run: !!b.dry_run, mac: b.old_mac,
                   id: '0005', step: 'done', started_unix: now(), finished_unix: now(),
                   message: b.dry_run ? 'dry run: would erase the table, write back the other 23 and the new one at 0005 (not joined)'
                                      : 'replaced 0005; the other 23 written back as they were, the new one waiting to join',
                   dry_frames: b.dry_run ? frames : [] };
        if (!b.dry_run) t.waiting = [{ mac: NEW_MAC, label: '0-1234R', id: '0005', since_unix: now() }];
      }
      if (path.endsWith('/stop')) t.waiting = [];
      return json(200, { started: true });
    }
    if (path === '/api/status') return json(200, { ...routes['/api/status'], platform: 'linux' });
    if (path === '/api/master') return json(200, w.master);
    if (path === '/api/devices') return json(200, { count: w.devices.length, devices: w.devices });
    if (path === '/api/nodes') return json(200, { ...routes['/api/nodes'], nodes: w.nodes });
    const body = piRoutes[path] ?? routes[path];
    return body === undefined ? route.fulfill({ status: 404, body: 'no' }) : json(200, body);
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/#setup`, { waitUntil: 'networkidle' });
  await page.waitForSelector('#nav-setup', { state: 'visible' });
  await page.waitForSelector('.setup-tasks');
  return { page, ctx, errors };
}

test('System setup: replace a quiet optimizer, step by step', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  // The quiet one is offered, and its place is what carries over.
  assert.match(await page.textContent('.setup-note'), /A2 has not reported for 3 h\./);
  assert.match(await page.textContent('.setup-holds'), /24\s*listed/);
  await page.click('.setup-note button[data-setup-start="replace"]');
  // Step 2: the label is checked as it's typed.
  await page.waitForSelector('#setup-label');
  assert.match(await page.textContent('.setup-rail'), /Removing A2/);
  await page.fill('#setup-label', '0-1234S');
  assert.match(await page.textContent('#setup-label-check'), /This label is not valid/);
  assert.equal(await page.isDisabled('button[data-setup-next]'), true);
  await page.fill('#setup-label', w.nodes[5].long_address.replace(/^04C05B(.)0*(.*)$/, '$1-$2') + 'X');
  await page.fill('#setup-label', '0-1234r');
  assert.match(await page.textContent('#setup-label-check'), /Valid label · radio address 04C05B0000001234/);
  // Step 3: the check is a dry run, and the plan is in words.
  await page.click('button[data-setup-next]');
  await page.waitForSelector('.setup-plan');
  const plan = await page.textContent('.setup-plan');
  assert.match(plan, /without A2's optimizer, adding the new optimizer at 0005 \(A2's address\)\. The other 23 optimizers are written back unchanged/);
  assert.match(plan, /Assign the new optimizer to A2's position: String A\./);
  assert.deepEqual(w.posts, [{ path: '/api/master/table/replace', old_mac: w.old.long_address, label: '0-1234r', dry_run: true }]);
  // Step 4 then 5: the real thing, then the wait.
  await page.click('button[data-setup-go]');
  await page.waitForSelector('.setup-wait');
  assert.equal(w.posts.length, 2);
  assert.equal(w.posts[1].dry_run, false);
  assert.match(await page.textContent('.setup-wait'), /join window open/);
  // It joins and takes A2's place: step 6.
  w.master.table.waiting = [];
  w.master.table.joins = [{ mac: NEW_MAC, label: '0-1234R', id: '0005', joined: true, text: 'joined at 0005', unix: now() }];
  w.nodes = w.nodes.map((n) => n.addr === '0005' ? { ...n, long_address: NEW_MAC } : n);
  w.devices = w.devices.map((d) => d.addr === '0005' ? { ...d, barcode: NEW_MAC, data_age_ms: 2000, stale: false } : d);
  await page.waitForSelector('.setup-done', { timeout: 10000 });
  assert.match(await page.textContent('.setup-done'), /A2 has been replaced.*Position\s*String A, named A2.*0-1234R at 0005/s);
  assert.deepEqual(errors, []);
  await ctx.close();
});

test('System setup: with changes off, a check still works and the button says why', async () => {
  const w = world(false);
  const { page, ctx, errors } = await open(w);
  assert.match(await page.textContent('#setup-root'), /Changes are disabled/);
  await page.click('.setup-task[data-setup-start="retire"]');
  await page.click('.setup-pick input[value="' + w.old.long_address + '"]');
  w.master.table.edit = { ...w.master.table.edit };
  await page.route('**/api/master/table/remove', (route) => {
    const b = JSON.parse(route.request().postData());
    w.posts.push(b);
    w.master.table.edit = { ...w.master.table.edit, kind: 'remove', finished: true, ok: true, dry_run: true, id: '0005', started_unix: now(),
      message: 'dry run: would erase the table and write back the other 23', dry_frames: ['2BBABE', '290A…', '290A…', '2903…'] };
    return route.fulfill({ status: 200, contentType: 'application/json', body: '{"started":true}' });
  });
  await page.click('button[data-setup-next]');
  await page.waitForSelector('.setup-plan');
  assert.match(await page.textContent('.setup-plan'), /rewrite the other 23 optimizers unchanged/);
  assert.equal(await page.isDisabled('button[data-setup-go]'), true);
  assert.match(await page.textContent('.setup-actions .why'), /Changes are disabled/);
  assert.deepEqual(w.posts, [{ mac: w.old.long_address, dry_run: true }]);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// Picking an optimizer far down the list keeps the list where it was: a
// click, and the page's own refresh, used to send it back to the top.
test('System setup: the list stays where it was scrolled', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.setViewportSize({ width: 1280, height: 700 });
  await page.click('.setup-task[data-setup-start="replace"]');
  await page.waitForSelector('.setup-pick');
  await page.$eval('.setup-pick', (el) => { el.scrollTop = el.scrollHeight; });
  const before = await page.$eval('.setup-pick', (el) => el.scrollTop);
  assert.ok(before > 100, `the list scrolls (${before})`);
  const last = w.nodes[w.nodes.length - 1].long_address;
  await page.click(`.setup-pick input[value="${last}"]`);
  assert.equal(await page.$eval('.setup-pick', (el) => el.scrollTop), before);
  assert.equal(await page.isDisabled('button[data-setup-next]'), false);
  await page.waitForTimeout(11000);  // past the page's 10 s refresh
  assert.equal(await page.$eval('.setup-pick', (el) => el.scrollTop), before);
  assert.equal(await page.isChecked(`.setup-pick input[value="${last}"]`), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});

// The list's search: every word typed must match the row (name, string,
// label, address); a choice made and hidden by the search stays made; Back
// from the label step comes back to the list.
test('System setup: search the list, and Back returns to it', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-task[data-setup-start="replace"]');
  await page.waitForSelector('.setup-pick');
  const visible = () => page.$$eval('.setup-pick label', (ls) => ls.filter((l) => !l.hidden).map((l) => l.querySelector('strong').textContent));
  assert.equal((await visible()).length, 24);
  await page.fill('#setup-search', 'string b');
  const sb = await visible();  // String B, plus any whose label holds a b
  assert.ok(['B1', 'B2', 'B3', 'B4', 'B5', 'B6'].every((n) => sb.includes(n)) && !sb.some((n) => n.startsWith('A')), sb.join(','));
  await page.fill('#setup-search', 'B4');
  const b4 = await visible();
  assert.ok(b4.includes('B4') && b4.length < 24, b4.join(','));
  await page.click('.setup-pick label:not([hidden]):has(strong:text-is("B4")) input');
  await page.fill('#setup-search', 'nothing like this');
  assert.deepEqual(await visible(), []);
  assert.equal(await page.isVisible('#setup-none'), true);
  assert.equal(await page.isDisabled('button[data-setup-next]'), false);  // B4 is still chosen
  await page.click('button[data-setup-next]');
  await page.waitForSelector('#setup-label');
  assert.match(await page.textContent('.setup-rail'), /Removing B4/);
  await page.click('button[data-setup-back]');
  await page.waitForSelector('#setup-search');
  assert.equal(await page.inputValue('#setup-search'), 'nothing like this');  // the search is kept
  assert.equal(await page.$eval('.setup-pick input:checked', (i) => i.closest('label').querySelector('strong').textContent), 'B4');
  assert.deepEqual(errors, []);
  await ctx.close();
});

// The review's bus frames, once opened, stay open through the page's own
// refresh (every 1.5 s while it follows the bus), which used to close them.
test('System setup: the bus frames stay open', async () => {
  const w = world();
  const { page, ctx, errors } = await open(w);
  await page.click('.setup-note button[data-setup-start="replace"]');
  await page.fill('#setup-label', '0-1234R');
  await page.click('button[data-setup-next]');
  await page.waitForSelector('.setup-frames');
  await page.click('.setup-frames summary');
  assert.equal(await page.$eval('.setup-frames', (d) => d.open), true);
  await page.waitForTimeout(4000);  // a few of the page's refreshes
  assert.equal(await page.$eval('.setup-frames', (d) => d.open), true);
  assert.deepEqual(errors, []);
  await ctx.close();
});
