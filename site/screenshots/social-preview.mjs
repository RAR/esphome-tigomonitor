// Render the GitHub social preview card (docs/images/social-preview.png).
//
// GitHub wants >=640x320 and shows 1280x640 best, so that is the artboard.
// The card is built from the same brand pieces as the docs site — the
// "Emitting" mark, the silicon-indigo/solar-amber duotone, and the receding
// PV array from site/src/styles/theme.css — so the repo page, the docs hero
// and the device UI all read as one thing.
//
// Fonts are inlined as data URIs from node_modules rather than linked, because
// this renders from a file:// page with no dev server and a silent fallback to
// a system face would be invisible until the image was already published.
//
// Run:  npm run social      (from site/)
// Out:  docs/images/social-preview.png   — COMMITTED; set it on the repo under
//       Settings → General → Social preview. GitHub does not read it from the
//       repo automatically.

import { chromium } from 'playwright';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = resolve(HERE, '../../docs/images/social-preview.png');
const HTML = resolve(HERE, 'social-preview.html');

const font = (p) =>
  `url(data:font/woff2;base64,${readFileSync(resolve(HERE, '../node_modules', p)).toString('base64')}) format('woff2')`;

const DISPLAY = font('@fontsource-variable/big-shoulders-display/files/big-shoulders-display-latin-wght-normal.woff2');
const SANS = font('@fontsource/ibm-plex-sans/files/ibm-plex-sans-latin-400-normal.woff2');
const MONO = font('@fontsource/ibm-plex-mono/files/ibm-plex-mono-latin-500-normal.woff2');

// The mark, at the full-detail cut — at 128px the six-block frame reads, so
// this is the docs/images/logo.svg drawing rather than the collapsed header one.
const MARK = `
<svg viewBox="0 0 64 64" class="mark" role="img" aria-label="Tigo Monitor">
  <rect x="9" y="17" width="34" height="28" rx="5" fill="#1D2A63" stroke="#C6D0E2" stroke-width="3.5"/>
  <rect x="16" y="24" width="20" height="5" rx="1" fill="#C6D0E2" fill-opacity=".65"/>
  <rect x="16" y="34" width="13" height="5" rx="1" fill="#F4B23C"/>
  <g fill="none" stroke="#F4B23C" stroke-width="4" stroke-linecap="round">
    <path d="M48 25 a11 11 0 0 1 0 14"/>
    <path d="M53 19 a19 19 0 0 1 0 26"/>
  </g>
  <g fill="none" stroke="#C6D0E2" stroke-width="4.5" stroke-linecap="round">
    <path d="M17 45 v5 a4 4 0 0 1 -4 4 h-4"/>
    <path d="M35 45 v5 a4 4 0 0 0 4 4 h4"/>
  </g>
</svg>`;

const html = `<!doctype html>
<meta charset="utf-8">
<style>
@font-face { font-family: 'Big Shoulders Display'; src: ${DISPLAY}; font-weight: 100 900; }
@font-face { font-family: 'IBM Plex Sans'; src: ${SANS}; font-weight: 400; }
@font-face { font-family: 'IBM Plex Mono'; src: ${MONO}; font-weight: 500; }

:root {
  --cell: #1d2a63;
  --busbar: #c6d0e2;
  --frame: #b9c6e4;
  --gap: #05070d;
  --amber: #f4b23c;
  --indigo: #4c63c4;
  --silver: #c6d0e2;
}
* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { width: 1280px; height: 640px; }
body {
  background: linear-gradient(160deg, #0d1424 0%, #05070d 60%);
  font-family: 'IBM Plex Sans', sans-serif;
  overflow: hidden;
}

.card { position: relative; width: 1280px; height: 640px; isolation: isolate; }

/* The PV array, same construction as the docs hero: cell gaps at 56px, busbars
   on the 14px quarter-pitch, and module edges drawn frame/gap/frame so two
   modules actually separate instead of reading as one more cell line. Here it
   is pushed to the right half and tipped harder, so it sits behind the type
   without competing with it. */
.array {
  position: absolute;
  inset: -30% -25% -10% 30%;
  z-index: -1;
  transform: perspective(900px) rotateX(38deg) rotateZ(-4deg) scale(1.2);
  transform-origin: 50% 100%;
  background:
    repeating-linear-gradient(90deg, transparent 0 13px,
      color-mix(in srgb, var(--busbar) 14%, transparent) 13px 14px),
    repeating-linear-gradient(0deg, transparent 0 53px,
      color-mix(in srgb, var(--busbar) 13%, transparent) 53px 56px),
    repeating-linear-gradient(90deg, transparent 0 53px,
      color-mix(in srgb, var(--busbar) 13%, transparent) 53px 56px),
    repeating-linear-gradient(0deg, transparent 0 213px,
      color-mix(in srgb, var(--frame) 30%, transparent) 213px 217px,
      color-mix(in srgb, var(--gap) 70%, transparent) 217px 220px,
      color-mix(in srgb, var(--frame) 30%, transparent) 220px 224px),
    repeating-linear-gradient(90deg, transparent 0 325px,
      color-mix(in srgb, var(--frame) 30%, transparent) 325px 329px,
      color-mix(in srgb, var(--gap) 70%, transparent) 329px 332px,
      color-mix(in srgb, var(--frame) 30%, transparent) 332px 336px),
    linear-gradient(0deg, color-mix(in srgb, var(--cell) 58%, transparent),
                          color-mix(in srgb, var(--cell) 58%, transparent));
  -webkit-mask-image: radial-gradient(85% 75% at 62% 55%, #000 10%, transparent 82%);
          mask-image: radial-gradient(85% 75% at 62% 55%, #000 10%, transparent 82%);
}

/* Low amber sun, top right — the one warm thing in the frame, and the same
   signal colour the UI uses for live telemetry. */
.sun {
  position: absolute; inset: 0; z-index: -1;
  background:
    radial-gradient(38% 46% at 84% 12%, color-mix(in srgb, var(--amber) 30%, transparent), transparent 70%),
    radial-gradient(50% 50% at 18% 22%, color-mix(in srgb, var(--indigo) 22%, transparent), transparent 72%);
}

.content {
  position: absolute;
  left: 84px; top: 0; bottom: 0;
  width: 780px;
  display: flex; flex-direction: column; justify-content: center;
  gap: 26px;
}

/* The mark hangs off the cap-height of TIGO rather than the centre of the
   two-line wordmark — centred, it floats in the gap between the lines. */
.lockup { display: flex; align-items: flex-start; gap: 24px; }
.mark { width: 108px; height: 108px; flex: none; margin-top: -14px; }

.wordmark {
  font-family: 'Big Shoulders Display', sans-serif;
  font-weight: 700;
  font-size: 116px;
  line-height: 0.84;
  letter-spacing: -0.005em;
  color: #f2f5fb;
  text-wrap: balance;
}
.wordmark span { display: block; color: var(--silver); }

.tagline {
  font-size: 27px;
  line-height: 1.38;
  color: #b6c0d4;
  max-width: 726px;
}
.tagline b { color: #eef2f9; font-weight: 400; }

.chips { display: flex; gap: 10px; flex-wrap: wrap; }
.chip {
  font-family: 'IBM Plex Mono', monospace;
  font-weight: 500;
  font-size: 16px;
  letter-spacing: 0.12em;
  text-transform: uppercase;
  color: #a9b6d2;
  border: 1px solid color-mix(in srgb, var(--silver) 22%, transparent);
  border-radius: 999px;
  padding: 7px 16px 6px;
  background: color-mix(in srgb, #0b111f 55%, transparent);
}
.chip.live {
  color: #0d1424;
  background: var(--amber);
  border-color: var(--amber);
}

/* A hairline of amber along the bottom edge: the telemetry frame arriving.
   It also gives the card a defined edge against light backgrounds. */
.rule {
  position: absolute; left: 0; right: 0; bottom: 0; height: 6px;
  background: linear-gradient(90deg,
    var(--amber) 0%, var(--amber) 22%,
    color-mix(in srgb, var(--amber) 35%, transparent) 40%,
    color-mix(in srgb, var(--indigo) 45%, transparent) 70%,
    transparent 100%);
}
</style>
<div class="card">
  <div class="sun"></div>
  <div class="array"></div>
  <div class="content">
    <div class="lockup">
      ${MARK}
      <div class="wordmark">TIGO<span>MONITOR</span></div>
    </div>
    <p class="tagline">
      <b>Per-panel solar data in Home Assistant</b>, read straight off the
      Tigo RS485 bus. No cloud account, no Tigo API.
    </p>
    <div class="chips">
      <span class="chip live">ESPHome</span>
      <span class="chip">ESP32</span>
      <span class="chip">RS485</span>
      <span class="chip">Home Assistant</span>
    </div>
  </div>
  <div class="rule"></div>
</div>`;

writeFileSync(HTML, html);

const browser = await chromium.launch({
  args: ['--no-sandbox', '--force-color-profile=srgb', '--font-render-hinting=none'],
});
const page = await browser.newPage({
  viewport: { width: 1280, height: 640 },
  deviceScaleFactor: 1,
  reducedMotion: 'reduce',
});
let failed = 0;
page.on('pageerror', (e) => { failed++; console.error(`  ! ${e.message}`); });
await page.goto(`file://${HTML}`, { waitUntil: 'load' });
await page.evaluate(() => document.fonts.ready);

// A missing @font-face silently falls back, and the card would look merely
// "off" rather than broken. Assert the display face actually loaded.
const gotDisplay = await page.evaluate(() =>
  document.fonts.check('700 116px "Big Shoulders Display"'));
if (!gotDisplay) {
  console.error('  FAILED: Big Shoulders Display did not load — check node_modules paths.');
  process.exit(1);
}

await page.screenshot({ path: OUT });
await browser.close();

console.log(`  ✓ 1280x640 → docs/images/social-preview.png`);
if (failed) process.exit(1);
