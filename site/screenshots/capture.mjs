// Screenshot the device's web UI for the docs, with no firmware and no hardware.
//
// The SPA is one self-contained HTML file whose every request goes through a
// single apiFetch(), so page.route() can answer all of it from fixtures.mjs.
// Views are plain location.hash and the theme is a localStorage key.
//
// Run:  npm run screenshots        (from site/)
// Out:  site/src/assets/screenshots/*.png   — generated, git-ignored
//
// Wired into `npm run build`, so the images can never drift from the UI: they
// are rebuilt from the current app.html on every docs build.

import { chromium } from 'playwright';
import { routes, patterns } from './fixtures.mjs';
import { readFileSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const APP = resolve(HERE, '../../components/tigo_server/web/app.html');
const OUT = resolve(HERE, '../src/assets/screenshots');

// Only the views worth putting in front of a reader. CCA Info and Tigo Cloud
// are deliberately absent: both are mostly account and connection state, which
// is exactly the material we do not want on a public page even as fixtures.
const VIEWS = [
  ['dashboard',   'Dashboard'],
  ['topology',    'Topology'],
  ['history',     'History'],
  ['nodes',       'Node Table'],
  ['diagnostics', 'Diagnostics'],
];

const THEMES = ['dark', 'light'];

const html = readFileSync(APP, 'utf8').replaceAll('__TIGO_API_TOKEN__', '');
const server = createServer((_req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(html);
}).listen(0, '127.0.0.1');
await new Promise((r) => server.once('listening', r));
const port = server.address().port;

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch({
  args: ['--no-sandbox', '--force-color-profile=srgb', '--font-render-hinting=none'],
});

const missing = new Set();
const written = [];
let pageErrors = 0;

for (const theme of THEMES) {
  const ctx = await browser.newContext({
    viewport: { width: 1440, height: 900 },
    deviceScaleFactor: 2,
    reducedMotion: 'reduce',      // no entry animations mid-capture
    colorScheme: theme,
  });

  await ctx.route('**/api/**', async (route) => {
    const url = route.request().url();
    const path = new URL(url).pathname.replace(/^.*(\/api\/)/, '$1');
    let body = routes[path];
    if (body === undefined) {
      const hit = patterns.find(([re]) => re.test(path));
      if (hit) body = hit[1](url);
    }
    if (body === undefined) {
      missing.add(path);
      return route.fulfill({ status: 404, body: 'no fixture' });
    }
    return typeof body === 'string'
      ? route.fulfill({ status: 200, contentType: 'text/plain', body })
      : route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });

  await ctx.addInitScript((t) => {
    localStorage.setItem('tigoTheme', t);
    localStorage.setItem('tempUnit', 'C');
  }, theme);

  const page = await ctx.newPage();
  page.on('pageerror', (e) => { pageErrors++; console.error(`  ! page error: ${e.message}`); });

  // Load ONCE at the default view, then switch by clicking the sidebar.
  // Navigating straight to #diagnostics leaves the view on its "Loading…"
  // placeholder — those views populate from the hashchange handler, which
  // never fires when the hash is already correct at load.
  await page.goto(`http://127.0.0.1:${port}/app`, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1500);

  for (const [view, name] of VIEWS) {
    await page.click(`button[data-view="${view}"]`);
    await page.waitForLoadState('networkidle');
    await page.waitForTimeout(1500);
    const file = `${view}-${theme}.png`;
    await page.screenshot({ path: `${OUT}/${file}` });
    written.push(file);
    console.log(`  ✓ ${name.padEnd(12)} ${theme.padEnd(5)} → ${file}`);
  }
  await ctx.close();
}

await browser.close();
server.close();

if (missing.size) {
  console.log('\n  endpoints with no fixture (served 404):');
  for (const m of [...missing].sort()) console.log(`    ${m}`);
}

writeFileSync(`${OUT}/manifest.json`,
  JSON.stringify({ generated: new Date().toISOString(), views: VIEWS.map(([v]) => v), themes: THEMES, written }, null, 2));

console.log(`\n  ${written.length} screenshots → src/assets/screenshots/`);

// A page error means the SPA threw while rendering, which usually means a
// fixture no longer matches the firmware's JSON. Fail loudly rather than
// publishing screenshots of a half-rendered UI.
if (pageErrors > 0) {
  console.error(`\n  FAILED: ${pageErrors} page error(s) during capture.`);
  process.exit(1);
}
