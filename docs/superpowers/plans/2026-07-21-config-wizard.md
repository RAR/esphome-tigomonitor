# Config Wizard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a static, GitHub Pages–hosted wizard that generates a complete, ready-to-flash ESPHome starter config tailored to the user's curated board.

**Architecture:** Pure static site in `site/` — no framework, no bundler, no build step. Hand-authored board data (`boards.js`) + cross-cutting toggle transforms (`rules.js`) feed a deterministic YAML serializer (`yaml.js`); `wizard.js` wires the DOM and live preview. A Node (`node --test`) drift-guard asserts the board data still matches `boards/*.yaml`. GitHub Actions runs the tests, then deploys `site/` to Pages.

**Tech Stack:** ES modules (run unchanged in browser and Node ≥ 20), vanilla HTML/CSS, `node --test` (zero external deps), GitHub Actions Pages deploy.

## Global Constraints

- **Zero runtime/test dependencies.** No npm install; tests use only `node --test` and Node built-ins (`node:fs`, `node:test`, `node:assert`, global `crypto`/`btoa`). Node ≥ 20 assumed in CI.
- **ES modules everywhere.** Every `site/*.js` and `site/lib/*.mjs` uses `export`/`import` so the same file loads in the browser (`<script type="module">`) and under Node. Browser-loaded modules use the `.js` extension; Node-only test/lib helpers use `.mjs`.
- **Client-side only.** No secret, password, or form value is ever sent over the network. No analytics, no external script/CDN.
- **Generated YAML must compile as-is.** Always emit empty `text_sensor:` and `binary_sensor:` sections (the components fail to compile without them — CLAUDE.md rule). Apply block order exactly: `esphome → esp32 → psram → esp32_hosted → wifi → captive_portal → logger → api → ota → uart → tigo_monitor → tigo_server → sensor → text_sensor → binary_sensor → (display overlay)`.
- **Board data is not the source of truth for board correctness — `boards/*.yaml` is.** Any safety-critical value in `boards.js` (flash_size, partitions, psram mode/speed, `enable_idf_experimental_features`, `esp32_hosted` presence, esp_tsdb ref) must be verified by the drift-guard against the matching board file.
- **Two-space YAML indent, no tabs.** ESPHome rejects tabs.

---

## File Structure

```
site/
  index.html          # markup: form sections + preview pane
  style.css           # dark theme (mirrors on-device dashboard palette)
  boards.js           # export BOARDS, getBoard(id)  — curated board data
  rules.js            # export assembleConfig(board, form) -> cfg
  yaml.js             # export toYaml(cfg), toSecretsYaml(cfg)
  wizard.js           # DOM wiring, live preview, copy/download (browser only)
  lib/
    apikey.mjs        # export generateApiKey()  — Web Crypto base64 key
    yaml-extract.mjs  # export extractBoardFields(text) — pull safety keys from a board yaml
  test/
    boards.shape.test.mjs   # boards.js self-consistency
    yaml-extract.test.mjs   # extractor unit tests (fixtures)
    drift.test.mjs          # boards.js vs boards/*.yaml
    rules.test.mjs          # toggle transforms
    yaml.test.mjs           # serialization + secrets
.github/workflows/
  pages.yml           # test job -> deploy job
```

`apikey.mjs` and `yaml-extract.mjs` use the `.mjs` extension but are imported by browser code too **only** where noted; `apikey.mjs` is imported by `wizard.js` in the browser, so it must stay browser-safe (Web Crypto `crypto.getRandomValues` + `btoa`, both global in browsers and Node ≥ 20). `yaml-extract.mjs` is Node/test-only (uses no fs itself — it takes text).

---

### Task 1: Board data module (`boards.js`) + shape test

**Files:**
- Create: `site/boards.js`
- Test: `site/test/boards.shape.test.mjs`

**Interfaces:**
- Produces: `export const BOARDS: Board[]` and `export function getBoard(id: string): Board | undefined`.
- `Board` shape (consumed by Tasks 4–6 and the drift-guard):
  ```
  {
    id: string, label: string,
    chip: 'esp32s3'|'esp32p4'|'esp32',
    board: string, variant?: string,
    flash_size?: string,                    // omit -> board default
    partitions?: { default: string, ble?: string },
    psram: { mode: string, speed: string } | null,
    frameworkAdvanced: { enable_idf_experimental_features: boolean },
    frameworkComponents: string[],          // registry component lines, e.g. 'zakery292/esp_tsdb^2.1.0'
    hostedComponent: null | { source: string, ref: string },  // git managed component (P4 esp_tsdb fork)
    sdkconfig: Record<string,string>,
    hosted: null | { variant, slot, clk_pin, cmd_pin, d0_pin, d1_pin, d2_pin, d3_pin, reset_pin, active_high },
    uartDefault: { tx_pin: string, rx_pin: string, rx_buffer_size: number },
    numberOfDevices: number,
    supports: { ble: boolean, display: boolean },
    displayOverlay: string | null,          // verbatim YAML appended when display enabled
    notes: string[],
  }
  ```

- [ ] **Step 1: Write the failing shape test**

```js
// site/test/boards.shape.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { BOARDS, getBoard } from '../boards.js';

test('every board has required identity fields', () => {
  for (const b of BOARDS) {
    assert.ok(b.id && b.label && b.chip && b.board, `missing identity on ${b.id}`);
    assert.ok(['esp32s3', 'esp32p4', 'esp32'].includes(b.chip), `bad chip on ${b.id}`);
    assert.ok(b.uartDefault?.tx_pin && b.uartDefault?.rx_pin, `missing uart on ${b.id}`);
    assert.ok(Number.isInteger(b.numberOfDevices), `bad numberOfDevices on ${b.id}`);
  }
});

test('board ids are unique and getBoard resolves them', () => {
  const ids = BOARDS.map((b) => b.id);
  assert.equal(new Set(ids).size, ids.length, 'duplicate board id');
  for (const id of ids) assert.equal(getBoard(id)?.id, id);
});

test('P4 carries the experimental flag and a hosted radio', () => {
  const p4 = getBoard('esp32p4-evboard');
  assert.equal(p4.frameworkAdvanced.enable_idf_experimental_features, true);
  assert.ok(p4.hosted, 'P4 must define an esp32_hosted companion');
});

test('BLE is only offered where a BLE partition exists', () => {
  for (const b of BOARDS) {
    if (b.supports.ble) assert.ok(b.partitions?.ble, `${b.id} offers BLE without a ble partition`);
  }
});

test('a board that offers display defines a displayOverlay', () => {
  for (const b of BOARDS) {
    if (b.supports.display) assert.ok(b.displayOverlay, `${b.id} supports display without an overlay`);
  }
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `node --test site/test/boards.shape.test.mjs`
Expected: FAIL — `Cannot find module '../boards.js'`.

- [ ] **Step 3: Write `site/boards.js`**

Populate with the four curated bases below. Values are copied from the matching `boards/*.yaml` (drift-guard enforces this in Task 3). Leave `displayOverlay: null` for now — Task 8 fills the AtomS3R overlay verbatim from `boards/atoms3r-display.yaml`.

```js
// site/boards.js
export const BOARDS = [
  {
    id: 'esp32s3-atoms3r',
    label: 'M5Stack AtomS3R (ESP32-S3, 8MB PSRAM)',
    chip: 'esp32s3', board: 'm5stack-atoms3', variant: 'esp32s3',
    flash_size: '8MB',
    partitions: { default: 'partitions/tigo-8mb.csv', ble: 'partitions/tigo-8mb-ble.csv' },
    psram: { mode: 'octal', speed: '80MHz' },
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: ['zakery292/esp_tsdb^2.1.0', 'joltwallet/littlefs^1.16'],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '2048',
      CONFIG_UART_TX_BUFFER_SIZE: '512',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '32',
      CONFIG_SPIRAM_MODE_OCT: 'y',
      CONFIG_SPIRAM_SPEED_80M: 'y',
      CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: 'y',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO1', rx_pin: 'GPIO2', rx_buffer_size: 2048 },
    numberOfDevices: 30,
    supports: { ble: true, display: true },
    displayOverlay: null,
    notes: ['Built-in tail485 RS485 transceiver on GPIO1/GPIO2.'],
  },
  {
    id: 'esp32s3-atoms3',
    label: 'M5Stack AtomS3 / AtomS3 Lite (ESP32-S3, no PSRAM)',
    chip: 'esp32s3', board: 'm5stack-atoms3', variant: 'esp32s3',
    psram: null,
    partitions: null,
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: [],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '1024',
      CONFIG_UART_TX_BUFFER_SIZE: '512',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '16',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO6', rx_pin: 'GPIO5', rx_buffer_size: 1024 },
    numberOfDevices: 15,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: ['No PSRAM: History/tsdb persistence is not configured on this board.'],
  },
  {
    id: 'esp32p4-evboard',
    label: 'ESP32-P4 Function EV Board (32MB PSRAM, C6 Wi-Fi)',
    chip: 'esp32p4', board: 'esp32-p4-function-ev-board', variant: 'esp32p4',
    flash_size: '16MB',
    partitions: { default: 'partitions/tigo-16mb.csv' },
    psram: { mode: 'hex', speed: '200MHz' },
    frameworkAdvanced: { enable_idf_experimental_features: true },
    frameworkComponents: ['joltwallet/littlefs^1.16'],
    hostedComponent: { source: 'https://github.com/RAR/esp_tsdb.git', ref: 'tigomonitor' },
    sdkconfig: {
      CONFIG_ESP32P4_DEFAULT_CPU_FREQ_400: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '16384',
      CONFIG_UART_TX_BUFFER_SIZE: '4096',
      CONFIG_FREERTOS_HZ: '1000',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '128',
      CONFIG_FREERTOS_USE_TICKLESS_IDLE: 'n',
      CONFIG_LWIP_MAX_SOCKETS: '16',
      CONFIG_LWIP_MAX_ACTIVE_TCP: '16',
      CONFIG_LWIP_MAX_LISTENING_TCP: '16',
    },
    hosted: {
      variant: 'ESP32C6', slot: 1, active_high: true,
      clk_pin: 'GPIO18', cmd_pin: 'GPIO19',
      d0_pin: 'GPIO14', d1_pin: 'GPIO15', d2_pin: 'GPIO16', d3_pin: 'GPIO17',
      reset_pin: 'GPIO54',
    },
    uartDefault: { tx_pin: 'GPIO20', rx_pin: 'GPIO21', rx_buffer_size: 16384 },
    numberOfDevices: 100,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: [
      'No native Wi-Fi — uses an ESP32-C6 companion over SDIO (esp32_hosted).',
      '200MHz hex PSRAM requires enable_idf_experimental_features (set automatically).',
    ],
  },
  {
    id: 'esp32-dev',
    label: 'Generic ESP32 DevKit (classic ESP32)',
    chip: 'esp32', board: 'esp32dev',
    psram: null,
    partitions: null,
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: [],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '1024',
      CONFIG_UART_TX_BUFFER_SIZE: '256',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '16',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO1', rx_pin: 'GPIO3', rx_buffer_size: 1024 },
    numberOfDevices: 12,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: ['Limited RAM: keep number_of_devices low; History/tsdb not configured.'],
  },
];

export function getBoard(id) {
  return BOARDS.find((b) => b.id === id);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `node --test site/test/boards.shape.test.mjs`
Expected: PASS (5 tests). The display test passes because no board sets `supports.display` true yet — AtomS3R's `display` flag stays paired with the overlay in Task 8; temporarily set AtomS3R `supports.display` to `false` here and flip it to `true` in Task 8 when the overlay lands.

- [ ] **Step 5: Commit**

```bash
git add site/boards.js site/test/boards.shape.test.mjs
git commit -m "feat(site): curated board data model for config wizard"
```

---

### Task 2: Board-field extractor (`lib/yaml-extract.mjs`) + tests

**Files:**
- Create: `site/lib/yaml-extract.mjs`
- Test: `site/test/yaml-extract.test.mjs`

**Interfaces:**
- Produces: `export function extractBoardFields(text: string): { flash_size, partitions, psramMode, psramSpeed, experimental, hasHosted, components }`.
  - `flash_size`: string or `null`; `partitions`: string or `null`; `psramMode`/`psramSpeed`: string or `null` (from the top-level `psram:` block); `experimental`: boolean (`enable_idf_experimental_features: yes/true`); `hasHosted`: boolean (`esp32_hosted:` block present); `components`: string[] of trimmed `- <component>` lines under `framework: components:`.
- Consumed by: Task 3 (drift-guard).

- [ ] **Step 1: Write the failing test**

```js
// site/test/yaml-extract.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { extractBoardFields } from '../lib/yaml-extract.mjs';

const P4 = `
esp32:
  board: esp32-p4-function-ev-board
  flash_size: 16MB
  partitions: partitions/tigo-16mb.csv
  framework:
    advanced:
      enable_idf_experimental_features: yes
    components:
      - name: zakery292/esp_tsdb
      - joltwallet/littlefs^1.16
psram:
  mode: hex
  speed: 200MHz
esp32_hosted:
  variant: ESP32C6
`;

test('extracts P4 safety fields', () => {
  const f = extractBoardFields(P4);
  assert.equal(f.flash_size, '16MB');
  assert.equal(f.partitions, 'partitions/tigo-16mb.csv');
  assert.equal(f.psramMode, 'hex');
  assert.equal(f.psramSpeed, '200MHz');
  assert.equal(f.experimental, true);
  assert.equal(f.hasHosted, true);
});

test('absent fields come back null/false', () => {
  const f = extractBoardFields('esp32:\n  board: esp32dev\n');
  assert.equal(f.flash_size, null);
  assert.equal(f.partitions, null);
  assert.equal(f.psramMode, null);
  assert.equal(f.experimental, false);
  assert.equal(f.hasHosted, false);
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `node --test site/test/yaml-extract.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement the extractor**

```js
// site/lib/yaml-extract.mjs
// Targeted, line-based reader for the handful of safety-critical keys the
// drift-guard checks. NOT a general YAML parser — deliberately narrow.
export function extractBoardFields(text) {
  const line = (re) => {
    const m = text.match(re);
    return m ? m[1].trim() : null;
  };
  const flash_size = line(/^\s*flash_size:\s*(\S+)/m);
  const partitions = line(/^\s*partitions:\s*(\S+)/m);
  // top-level psram: block
  const psramBlock = text.match(/^psram:\s*$([\s\S]*?)(?=^\S|\Z)/m);
  const pick = (blk, key) => {
    if (!blk) return null;
    const m = blk.match(new RegExp(`^\\s*${key}:\\s*(\\S+)`, 'm'));
    return m ? m[1].trim() : null;
  };
  const psramMode = pick(psramBlock?.[1], 'mode');
  const psramSpeed = pick(psramBlock?.[1], 'speed');
  const experimental = /enable_idf_experimental_features:\s*(yes|true)\b/.test(text);
  const hasHosted = /^esp32_hosted:\s*$/m.test(text);
  const components = [...text.matchAll(/^\s*-\s*(?:name:\s*)?([A-Za-z0-9_./^-]+)\s*$/gm)]
    .map((m) => m[1])
    .filter((c) => c.includes('/'));
  return { flash_size, partitions, psramMode, psramSpeed, experimental, hasHosted, components };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `node --test site/test/yaml-extract.test.mjs`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add site/lib/yaml-extract.mjs site/test/yaml-extract.test.mjs
git commit -m "feat(site): targeted board-yaml field extractor for drift-guard"
```

---

### Task 3: Drift-guard (`test/drift.test.mjs`)

**Files:**
- Test: `site/test/drift.test.mjs`

**Interfaces:**
- Consumes: `BOARDS`/`getBoard` (Task 1), `extractBoardFields` (Task 2), and the real files under `boards/` at repo root.
- Produces: no exports — a CI gate.

Board id → board file map (only boards that have a standalone base file are checked):

| board id | file |
| --- | --- |
| `esp32s3-atoms3r` | `boards/esp32s3-atoms3r.yaml` |
| `esp32s3-atoms3` | `boards/esp32s3-atoms3.yaml` |
| `esp32p4-evboard` | `boards/esp32p4-evboard.yaml` |
| `esp32-dev` | `boards/esp32-dev.yaml` |

- [ ] **Step 1: Write the failing test**

```js
// site/test/drift.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { BOARDS, getBoard } from '../boards.js';
import { extractBoardFields } from '../lib/yaml-extract.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const FILE = {
  'esp32s3-atoms3r': 'boards/esp32s3-atoms3r.yaml',
  'esp32s3-atoms3': 'boards/esp32s3-atoms3.yaml',
  'esp32p4-evboard': 'boards/esp32p4-evboard.yaml',
  'esp32-dev': 'boards/esp32-dev.yaml',
};

for (const [id, rel] of Object.entries(FILE)) {
  test(`${id} board data matches ${rel}`, () => {
    const b = getBoard(id);
    const f = extractBoardFields(readFileSync(join(repoRoot, rel), 'utf8'));
    assert.equal(b.flash_size ?? null, f.flash_size, 'flash_size drift');
    assert.equal(b.partitions?.default ?? null, f.partitions, 'partitions drift');
    assert.equal(b.psram?.mode ?? null, f.psramMode, 'psram.mode drift');
    assert.equal(b.psram?.speed ?? null, f.psramSpeed, 'psram.speed drift');
    assert.equal(b.frameworkAdvanced.enable_idf_experimental_features, f.experimental, 'experimental flag drift');
    assert.equal(Boolean(b.hosted), f.hasHosted, 'esp32_hosted presence drift');
  });
}

test('every checked board still exists in BOARDS', () => {
  for (const id of Object.keys(FILE)) assert.ok(getBoard(id), `missing board ${id}`);
});
```

- [ ] **Step 2: Run to verify it passes immediately**

Run: `node --test site/test/drift.test.mjs`
Expected: PASS. (If it FAILS, the mismatch is a real drift — fix `boards.js` to match the board file, do not loosen the test.)

Note: this test is written to pass on first run because Task 1's data was copied from these files. It exists to fail *later* if either side changes. That is its whole purpose.

- [ ] **Step 3: Commit**

```bash
git add site/test/drift.test.mjs
git commit -m "test(site): drift-guard tying board data to boards/*.yaml"
```

---

### Task 4: Toggle transforms + config assembly (`rules.js`)

**Files:**
- Create: `site/rules.js`
- Test: `site/test/rules.test.mjs`

**Interfaces:**
- Consumes: a `Board` (Task 1).
- Produces: `export function assembleConfig(board, form): Cfg`.
  - `form` shape: `{ name, numberOfDevices, updateInterval, uart:{tx_pin,rx_pin}, wifi:{ssid,password,staticIp?}, useSecrets:boolean, apiKey:string, otaPassword:string, cca:'none'|'http'|'ble', ccaIp?:string, cloudImport:boolean, display:boolean }`.
  - `Cfg` shape (consumed by Task 5 `toYaml`/`toSecretsYaml`):
    ```
    {
      name, friendlyName, minVersion: '2026.5.0',
      esp32: { board, variant?, flash_size?, frameworkAdvanced, frameworkComponents, hostedComponent, sdkconfig },
      partitions: string | null,
      psram: {mode,speed} | null,
      hosted: object | null,
      wifi: { ssid, password, staticIp? , useSecrets },
      api: { useSecrets, key },
      ota: { useSecrets, password },
      uart: { tx_pin, rx_pin, rx_buffer_size },
      tigoMonitor: { numberOfDevices, updateInterval, ccaIp: string|null },
      tigoServer: { ccaSource: 'http'|'ble'|null, cloudImport: boolean },
      displayOverlay: string | null,
      secrets: { ... } | null,
    }
    ```

Rules encoded:
- `cca:'ble'` → `partitions` switches to `board.partitions.ble`; `tigoServer.ccaSource='ble'`. Only reachable when `board.supports.ble` (UI disables otherwise); `assembleConfig` throws if asked for BLE on an unsupported board.
- `cca:'http'` → `tigoServer.ccaSource='http'`, `tigoMonitor.ccaIp = form.ccaIp || '192.168.1.100'`.
- `cca:'none'` → `ccaSource=null`, `ccaIp=null`.
- `cloudImport:true` → `tigoServer.cloudImport=true`.
- `display:true` → `displayOverlay = board.displayOverlay` (only when `board.supports.display`).
- `useSecrets:true` → credential fields resolve to `!secret` names and a parallel `secrets` map is populated; else inline values.

- [ ] **Step 1: Write the failing test**

```js
// site/test/rules.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { getBoard } from '../boards.js';
import { assembleConfig } from '../rules.js';

const baseForm = {
  name: 'tigo-server', numberOfDevices: 30, updateInterval: '30s',
  uart: { tx_pin: 'GPIO1', rx_pin: 'GPIO2' },
  wifi: { ssid: 'net', password: 'pw' }, useSecrets: false,
  apiKey: 'KEY==', otaPassword: 'ota',
  cca: 'none', cloudImport: false, display: false,
};

test('base config carries board scaffolding', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), baseForm);
  assert.equal(cfg.esp32.flash_size, '8MB');
  assert.equal(cfg.partitions, 'partitions/tigo-8mb.csv');
  assert.equal(cfg.psram.mode, 'octal');
  assert.equal(cfg.tigoServer.ccaSource, null);
});

test('BLE swaps to the ble partition', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), { ...baseForm, cca: 'ble' });
  assert.equal(cfg.partitions, 'partitions/tigo-8mb-ble.csv');
  assert.equal(cfg.tigoServer.ccaSource, 'ble');
});

test('BLE on an unsupported board throws', () => {
  assert.throws(() => assembleConfig(getBoard('esp32-dev'), { ...baseForm, cca: 'ble' }), /BLE/);
});

test('HTTP CCA sets a cca_ip', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), { ...baseForm, cca: 'http', ccaIp: '10.0.0.5' });
  assert.equal(cfg.tigoServer.ccaSource, 'http');
  assert.equal(cfg.tigoMonitor.ccaIp, '10.0.0.5');
});

test('secrets mode populates a secrets map', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), { ...baseForm, useSecrets: true });
  assert.equal(cfg.secrets.wifi_ssid, 'net');
  assert.equal(cfg.secrets.api_encryption_key, 'KEY==');
  assert.equal(cfg.secrets.ota_password, 'ota');
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `node --test site/test/rules.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement `rules.js`**

```js
// site/rules.js
export function assembleConfig(board, form) {
  if (form.cca === 'ble' && !board.supports.ble) {
    throw new Error(`BLE is not supported on ${board.id}`);
  }
  const useSecrets = Boolean(form.useSecrets);
  const partitions =
    form.cca === 'ble' && board.partitions?.ble
      ? board.partitions.ble
      : board.partitions?.default ?? null;

  const secrets = useSecrets
    ? {
        wifi_ssid: form.wifi.ssid,
        wifi_password: form.wifi.password,
        api_encryption_key: form.apiKey,
        ota_password: form.otaPassword,
      }
    : null;

  return {
    name: form.name || 'tigo-server',
    friendlyName: 'Tigo Server',
    minVersion: '2026.5.0',
    esp32: {
      board: board.board,
      variant: board.variant ?? null,
      flash_size: board.flash_size ?? null,
      frameworkAdvanced: board.frameworkAdvanced,
      frameworkComponents: board.frameworkComponents,
      hostedComponent: board.hostedComponent,
      sdkconfig: board.sdkconfig,
    },
    partitions,
    psram: board.psram,
    hosted: board.hosted,
    wifi: {
      ssid: form.wifi.ssid,
      password: form.wifi.password,
      staticIp: form.wifi.staticIp || null,
      useSecrets,
    },
    api: { useSecrets, key: form.apiKey },
    ota: { useSecrets, password: form.otaPassword },
    uart: {
      tx_pin: form.uart.tx_pin || board.uartDefault.tx_pin,
      rx_pin: form.uart.rx_pin || board.uartDefault.rx_pin,
      rx_buffer_size: board.uartDefault.rx_buffer_size,
    },
    tigoMonitor: {
      numberOfDevices: form.numberOfDevices || board.numberOfDevices,
      updateInterval: form.updateInterval || '30s',
      ccaIp: form.cca === 'http' ? form.ccaIp || '192.168.1.100' : null,
    },
    tigoServer: {
      ccaSource: form.cca === 'none' ? null : form.cca,
      cloudImport: Boolean(form.cloudImport),
    },
    displayOverlay: form.display && board.supports.display ? board.displayOverlay : null,
    secrets,
  };
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `node --test site/test/rules.test.mjs`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add site/rules.js site/test/rules.test.mjs
git commit -m "feat(site): config assembly + CCA/cloud/secrets toggle rules"
```

---

### Task 5: API-key generator (`lib/apikey.mjs`)

**Files:**
- Create: `site/lib/apikey.mjs`
- Test: `site/test/apikey.test.mjs`

**Interfaces:**
- Produces: `export function generateApiKey(): string` — 32 random bytes, base64-encoded (44 chars ending `=`). Uses global `crypto.getRandomValues` + `btoa` (browser + Node ≥ 20).

- [ ] **Step 1: Write the failing test**

```js
// site/test/apikey.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateApiKey } from '../lib/apikey.mjs';

test('generates a 44-char base64 key', () => {
  const k = generateApiKey();
  assert.equal(k.length, 44);
  assert.match(k, /^[A-Za-z0-9+/]{43}=$/);
});

test('successive keys differ', () => {
  assert.notEqual(generateApiKey(), generateApiKey());
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `node --test site/test/apikey.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement**

```js
// site/lib/apikey.mjs
export function generateApiKey() {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `node --test site/test/apikey.test.mjs`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add site/lib/apikey.mjs site/test/apikey.test.mjs
git commit -m "feat(site): in-browser API encryption-key generator"
```

---

### Task 6: YAML serializer (`yaml.js`)

**Files:**
- Create: `site/yaml.js`
- Test: `site/test/yaml.test.mjs`

**Interfaces:**
- Consumes: a `Cfg` (Task 4).
- Produces: `export function toYaml(cfg): string` and `export function toSecretsYaml(cfg): string | null` (null when `cfg.secrets` is null).

Serialization requirements (from Global Constraints): fixed block order; two-space indent; always-present empty `text_sensor:`/`binary_sensor:`; `!secret` references when `cfg.*.useSecrets`; `esp32_hosted:` block only when `cfg.hosted`; `psram:` only when `cfg.psram`; `flash_size`/`partitions` only when set; append `cfg.displayOverlay` verbatim when present; leading header comment.

- [ ] **Step 1: Write the failing test**

```js
// site/test/yaml.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { getBoard } from '../boards.js';
import { assembleConfig } from '../rules.js';
import { toYaml, toSecretsYaml } from '../yaml.js';

const form = {
  name: 'tigo-server', numberOfDevices: 30, updateInterval: '30s',
  uart: { tx_pin: 'GPIO1', rx_pin: 'GPIO2' },
  wifi: { ssid: 'net', password: 'pw' }, useSecrets: false,
  apiKey: 'KEY==', otaPassword: 'ota', cca: 'none', cloudImport: false, display: false,
};

test('emits blocks in order with required empty sections', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), form));
  for (const key of ['esphome:', 'esp32:', 'psram:', 'wifi:', 'api:', 'ota:', 'uart:', 'tigo_monitor:', 'tigo_server:', 'sensor:', 'text_sensor:', 'binary_sensor:']) {
    assert.ok(y.includes(key), `missing ${key}`);
  }
  assert.ok(y.indexOf('esp32:') < y.indexOf('psram:'), 'esp32 before psram');
  assert.ok(y.indexOf('psram:') < y.indexOf('wifi:'), 'psram before wifi');
  assert.ok(!y.includes('\t'), 'no tabs allowed');
});

test('P4 emits esp32_hosted and the experimental flag; no psram-less board emits psram', () => {
  const p4 = toYaml(assembleConfig(getBoard('esp32p4-evboard'), { ...form, uart: { tx_pin: 'GPIO20', rx_pin: 'GPIO21' } }));
  assert.ok(p4.includes('esp32_hosted:'));
  assert.ok(p4.includes('enable_idf_experimental_features: true'));
  const s3lite = toYaml(assembleConfig(getBoard('esp32s3-atoms3'), { ...form, uart: { tx_pin: 'GPIO6', rx_pin: 'GPIO5' } }));
  assert.ok(!s3lite.includes('\npsram:'), 'no-PSRAM board must not emit psram');
});

test('secrets mode emits !secret refs and a matching secrets.yaml', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, useSecrets: true });
  const y = toYaml(cfg);
  assert.ok(y.includes('password: !secret wifi_password'));
  assert.ok(y.includes('key: !secret api_encryption_key'));
  const s = toSecretsYaml(cfg);
  assert.ok(s.includes('wifi_password: pw'));
  assert.ok(s.includes('api_encryption_key: KEY=='));
});

test('toSecretsYaml is null when not using secrets', () => {
  assert.equal(toSecretsYaml(assembleConfig(getBoard('esp32s3-atoms3r'), form)), null);
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `node --test site/test/yaml.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement `yaml.js`**

```js
// site/yaml.js
const I = (n) => '  '.repeat(n);
const val = (useSecrets, secretName, raw) => (useSecrets ? `!secret ${secretName}` : raw);

export function toYaml(cfg) {
  const L = [];
  L.push(`# Generated by the Tigo Monitor config wizard`);
  L.push(`# Board: ${cfg.esp32.board}`);
  L.push(`# https://github.com/RAR/esphome-tigomonitor`);
  L.push('');

  // esphome
  L.push('esphome:');
  L.push(`${I(1)}name: ${cfg.name}`);
  L.push(`${I(1)}friendly_name: "${cfg.friendlyName}"`);
  L.push(`${I(1)}min_version: "${cfg.minVersion}"`);
  L.push('');

  // esp32
  L.push('esp32:');
  L.push(`${I(1)}board: ${cfg.esp32.board}`);
  if (cfg.esp32.variant) L.push(`${I(1)}variant: ${cfg.esp32.variant}`);
  if (cfg.esp32.flash_size) L.push(`${I(1)}flash_size: ${cfg.esp32.flash_size}`);
  if (cfg.partitions) L.push(`${I(1)}partitions: ${cfg.partitions}`);
  L.push(`${I(1)}framework:`);
  L.push(`${I(2)}type: esp-idf`);
  L.push(`${I(2)}version: recommended`);
  if (cfg.esp32.frameworkAdvanced.enable_idf_experimental_features) {
    L.push(`${I(2)}advanced:`);
    L.push(`${I(3)}enable_idf_experimental_features: true`);
  }
  const comps = [...cfg.esp32.frameworkComponents];
  if (cfg.esp32.hostedComponent || comps.length) {
    L.push(`${I(2)}components:`);
    if (cfg.esp32.hostedComponent) {
      L.push(`${I(3)}- name: zakery292/esp_tsdb`);
      L.push(`${I(4)}source: ${cfg.esp32.hostedComponent.source}`);
      L.push(`${I(4)}ref: ${cfg.esp32.hostedComponent.ref}`);
    }
    for (const c of comps) L.push(`${I(3)}- ${c}`);
  }
  L.push(`${I(2)}sdkconfig_options:`);
  for (const [k, v] of Object.entries(cfg.esp32.sdkconfig)) L.push(`${I(3)}${k}: "${v}"`);
  L.push('');

  // psram
  if (cfg.psram) {
    L.push('psram:');
    L.push(`${I(1)}mode: ${cfg.psram.mode}`);
    L.push(`${I(1)}speed: ${cfg.psram.speed}`);
    L.push('');
  }

  // esp32_hosted
  if (cfg.hosted) {
    const h = cfg.hosted;
    L.push('esp32_hosted:');
    L.push(`${I(1)}variant: ${h.variant}`);
    L.push(`${I(1)}active_high: ${h.active_high}`);
    L.push(`${I(1)}slot: ${h.slot}`);
    for (const p of ['clk_pin', 'cmd_pin', 'd0_pin', 'd1_pin', 'd2_pin', 'd3_pin', 'reset_pin']) {
      L.push(`${I(1)}${p}: ${h[p]}`);
    }
    L.push('');
  }

  // wifi
  L.push('wifi:');
  L.push(`${I(1)}ssid: ${val(cfg.wifi.useSecrets, 'wifi_ssid', cfg.wifi.ssid)}`);
  L.push(`${I(1)}password: ${val(cfg.wifi.useSecrets, 'wifi_password', cfg.wifi.password)}`);
  if (cfg.wifi.staticIp) {
    L.push(`${I(1)}manual_ip:`);
    L.push(`${I(2)}static_ip: ${cfg.wifi.staticIp}`);
  }
  L.push('');
  L.push('captive_portal:');
  L.push('');

  // logger
  L.push('logger:');
  L.push(`${I(1)}level: INFO`);
  L.push(`${I(1)}baud_rate: 0  # UART is the Tigo bus`);
  L.push('');

  // api
  L.push('api:');
  L.push(`${I(1)}encryption:`);
  L.push(`${I(2)}key: ${val(cfg.api.useSecrets, 'api_encryption_key', cfg.api.key)}`);
  L.push('');

  // ota
  L.push('ota:');
  L.push(`${I(1)}- platform: esphome`);
  L.push(`${I(2)}password: ${val(cfg.ota.useSecrets, 'ota_password', cfg.ota.password)}`);
  L.push(`${I(2)}allow_partition_access: true`);
  L.push('');

  // uart
  L.push('uart:');
  L.push(`${I(1)}id: tigo_uart`);
  L.push(`${I(1)}tx_pin: ${cfg.uart.tx_pin}`);
  L.push(`${I(1)}rx_pin: ${cfg.uart.rx_pin}`);
  L.push(`${I(1)}baud_rate: 38400`);
  L.push(`${I(1)}rx_buffer_size: ${cfg.uart.rx_buffer_size}`);
  L.push('');

  // tigo_monitor
  L.push('tigo_monitor:');
  L.push(`${I(1)}id: tigo_hub`);
  L.push(`${I(1)}uart_id: tigo_uart`);
  L.push(`${I(1)}number_of_devices: ${cfg.tigoMonitor.numberOfDevices}`);
  L.push(`${I(1)}update_interval: ${cfg.tigoMonitor.updateInterval}`);
  if (cfg.tigoMonitor.ccaIp) L.push(`${I(1)}cca_ip: ${cfg.tigoMonitor.ccaIp}`);
  L.push('');

  // tigo_server
  L.push('tigo_server:');
  L.push(`${I(1)}id: tigo_web`);
  L.push(`${I(1)}tigo_monitor_id: tigo_hub`);
  L.push(`${I(1)}port: 80`);
  if (cfg.tigoServer.ccaSource) L.push(`${I(1)}cca_source: ${cfg.tigoServer.ccaSource}`);
  if (cfg.tigoServer.cloudImport) L.push(`${I(1)}cloud_import: true`);
  L.push('');

  // sensor + required empty sections
  L.push('sensor:');
  for (const nm of ['Total Output Power', 'Total Energy Out', 'Active Device Count', 'Free Internal RAM', 'Free PSRAM']) {
    L.push(`${I(1)}- platform: tigo_monitor`);
    L.push(`${I(2)}tigo_monitor_id: tigo_hub`);
    L.push(`${I(2)}name: "${nm}"`);
  }
  L.push('');
  L.push('text_sensor:');
  L.push('');
  L.push('binary_sensor:');
  L.push('');

  let out = L.join('\n');
  if (cfg.displayOverlay) out += '\n' + cfg.displayOverlay.trim() + '\n';
  return out;
}

export function toSecretsYaml(cfg) {
  if (!cfg.secrets) return null;
  return Object.entries(cfg.secrets).map(([k, v]) => `${k}: ${v}`).join('\n') + '\n';
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `node --test site/test/yaml.test.mjs`
Expected: PASS (4 tests).

- [ ] **Step 5: Run the whole suite**

Run: `node --test site/test/`
Expected: PASS (all tests from Tasks 1–6).

- [ ] **Step 6: Commit**

```bash
git add site/yaml.js site/test/yaml.test.mjs
git commit -m "feat(site): deterministic ESPHome YAML + secrets.yaml serializer"
```

---

### Task 7: Wizard UI (`index.html`, `style.css`, `wizard.js`)

**Files:**
- Create: `site/index.html`, `site/style.css`, `site/wizard.js`

**Interfaces:**
- Consumes: `getBoard`/`BOARDS` (Task 1), `assembleConfig` (Task 4), `generateApiKey` (Task 5), `toYaml`/`toSecretsYaml` (Task 6).
- Produces: no test exports — a browser page. Verified manually (steps below).

This task has no unit test (DOM/browser). Keep all logic in the tested modules; `wizard.js` only reads inputs, calls `assembleConfig` + `toYaml`, and renders. Element IDs are the contract the manual check relies on.

- [ ] **Step 1: Write `index.html`**

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Tigo Monitor — Config Wizard</title>
  <link rel="stylesheet" href="style.css" />
</head>
<body>
  <header><h1>Tigo Monitor Config Wizard</h1></header>
  <main>
    <form id="wizard">
      <section>
        <label>Board
          <select id="board"></select>
        </label>
        <p id="board-notes" class="notes"></p>
      </section>
      <section>
        <label>Device name <input id="name" value="tigo-server" /></label>
        <label>Number of devices <input id="num-devices" type="number" min="1" /></label>
        <label>Update interval <input id="update-interval" value="30s" /></label>
        <label>UART TX pin <input id="uart-tx" /></label>
        <label>UART RX pin <input id="uart-rx" /></label>
      </section>
      <section>
        <label>Wi-Fi SSID <input id="wifi-ssid" placeholder="YOUR_WIFI_SSID" /></label>
        <label>Wi-Fi password <input id="wifi-pass" placeholder="YOUR_WIFI_PASSWORD" /></label>
        <label>Static IP (optional) <input id="static-ip" /></label>
      </section>
      <section>
        <label>CCA source
          <select id="cca"><option value="none">None</option><option value="http">HTTP (older firmware)</option><option value="ble">Bluetooth (4.0.4+)</option></select>
        </label>
        <label id="cca-ip-row">CCA IP <input id="cca-ip" /></label>
        <label><input type="checkbox" id="cloud" /> Enable Tigo cloud import</label>
        <label id="display-row"><input type="checkbox" id="display" /> Include display (AtomS3R-Display)</label>
      </section>
      <section>
        <label><input type="checkbox" id="use-secrets" checked /> Generate a secrets.yaml</label>
        <label>API encryption key <input id="api-key" /> <button type="button" id="gen-key">Generate</button></label>
        <label>OTA password <input id="ota-pass" value="changeme" /></label>
      </section>
    </form>
    <div class="preview">
      <div class="actions">
        <button id="copy">Copy YAML</button>
        <button id="dl-yaml">Download config</button>
        <button id="dl-secrets">Download secrets.yaml</button>
      </div>
      <pre id="out"></pre>
      <p id="warn" class="warn"></p>
    </div>
  </main>
  <script type="module" src="wizard.js"></script>
</body>
</html>
```

- [ ] **Step 2: Write `style.css`**

```css
:root { --bg:#0f1216; --panel:#171c22; --text:#e6edf3; --dim:#8b949e; --accent:#3fb950; }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:14px/1.5 system-ui, sans-serif; }
header { padding:16px 24px; border-bottom:1px solid #222; }
h1 { margin:0; font-size:18px; }
main { display:grid; grid-template-columns:1fr 1fr; gap:16px; padding:24px; }
section { background:var(--panel); border:1px solid #222; border-radius:8px; padding:16px; margin-bottom:16px; }
label { display:block; margin:8px 0; color:var(--dim); }
input, select { width:100%; margin-top:4px; background:#0d1117; color:var(--text); border:1px solid #30363d; border-radius:6px; padding:6px 8px; }
input[type=checkbox] { width:auto; }
.preview pre { background:#0d1117; border:1px solid #30363d; border-radius:8px; padding:16px; overflow:auto; max-height:80vh; white-space:pre; }
.actions { display:flex; gap:8px; margin-bottom:12px; }
button { background:var(--accent); color:#04240d; border:0; border-radius:6px; padding:8px 12px; cursor:pointer; font-weight:600; }
.notes, .warn { color:var(--dim); font-size:12px; }
.warn { color:#d29922; }
```

- [ ] **Step 3: Write `wizard.js`**

```js
// site/wizard.js
import { BOARDS, getBoard } from './boards.js';
import { assembleConfig } from './rules.js';
import { toYaml, toSecretsYaml } from './yaml.js';
import { generateApiKey } from './lib/apikey.mjs';

const $ = (id) => document.getElementById(id);
const boardSel = $('board');
for (const b of BOARDS) {
  const o = document.createElement('option');
  o.value = b.id; o.textContent = b.label; boardSel.appendChild(o);
}
if (!$('api-key').value) $('api-key').value = generateApiKey();

function readForm() {
  const board = getBoard(boardSel.value);
  return {
    board,
    form: {
      name: $('name').value.trim(),
      numberOfDevices: Number($('num-devices').value) || board.numberOfDevices,
      updateInterval: $('update-interval').value.trim(),
      uart: { tx_pin: $('uart-tx').value.trim(), rx_pin: $('uart-rx').value.trim() },
      wifi: { ssid: $('wifi-ssid').value.trim() || 'YOUR_WIFI_SSID',
              password: $('wifi-pass').value.trim() || 'YOUR_WIFI_PASSWORD',
              staticIp: $('static-ip').value.trim() },
      useSecrets: $('use-secrets').checked,
      apiKey: $('api-key').value.trim(),
      otaPassword: $('ota-pass').value.trim() || 'changeme',
      cca: $('cca').value,
      ccaIp: $('cca-ip').value.trim(),
      cloudImport: $('cloud').checked,
      display: $('display').checked,
    },
  };
}

function syncBoardConstraints(board) {
  // prefill uart, toggle ble/display availability
  if (!$('uart-tx').value) $('uart-tx').value = board.uartDefault.tx_pin;
  if (!$('uart-rx').value) $('uart-rx').value = board.uartDefault.rx_pin;
  if (!$('num-devices').value) $('num-devices').value = board.numberOfDevices;
  const bleOpt = $('cca').querySelector('option[value=ble]');
  bleOpt.disabled = !board.supports.ble;
  if (!board.supports.ble && $('cca').value === 'ble') $('cca').value = 'none';
  $('display-row').style.display = board.supports.display ? '' : 'none';
  $('board-notes').textContent = board.notes.join(' ');
}

let lastYaml = '', lastSecrets = null;
function render() {
  const { board, form } = readForm();
  $('cca-ip-row').style.display = form.cca === 'http' ? '' : 'none';
  try {
    const cfg = assembleConfig(board, form);
    lastYaml = toYaml(cfg);
    lastSecrets = toSecretsYaml(cfg);
    $('out').textContent = lastYaml;
    $('warn').textContent = '';
    $('dl-secrets').style.display = lastSecrets ? '' : 'none';
  } catch (e) {
    $('warn').textContent = e.message;
  }
}

function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/yaml' }));
  a.download = name; a.click(); URL.revokeObjectURL(a.href);
}

boardSel.addEventListener('change', () => { syncBoardConstraints(getBoard(boardSel.value)); render(); });
$('wizard').addEventListener('input', render);
$('gen-key').addEventListener('click', () => { $('api-key').value = generateApiKey(); render(); });
$('copy').addEventListener('click', () => navigator.clipboard.writeText(lastYaml));
$('dl-yaml').addEventListener('click', () => download(`${boardSel.value}.yaml`, lastYaml));
$('dl-secrets').addEventListener('click', () => { if (lastSecrets) download('secrets.yaml', lastSecrets); });

syncBoardConstraints(getBoard(boardSel.value));
render();
```

- [ ] **Step 4: Manual verification**

Run: `python3 -m http.server -d site 8099` then open `http://localhost:8099/`.
Verify:
1. Board dropdown lists all four boards; selecting **ESP32-P4** shows the C6/PSRAM notes and the preview contains `esp32_hosted:` + `enable_idf_experimental_features: true`.
2. Selecting **AtomS3R** enables the BLE option and shows the display checkbox; **ESP32 DevKit** disables BLE and hides display.
3. Choosing **CCA = HTTP** reveals the CCA IP field; the preview shows `cca_source: http` + `cca_ip:`.
4. **Generate** produces a new 44-char API key and the preview updates live.
5. **Download config** saves `<board>.yaml`; **Download secrets.yaml** appears only with "Generate a secrets.yaml" checked.
6. Paste the AtomS3R output into a scratch file and run `esphome config <file>` (needs the repo's `boards/partitions/` alongside, or adjust the path) — it validates. (Full compile is the final manual gate, not required here.)

- [ ] **Step 5: Commit**

```bash
git add site/index.html site/style.css site/wizard.js
git commit -m "feat(site): config wizard UI with live preview + download"
```

---

### Task 8: AtomS3R display overlay + drift check

**Files:**
- Modify: `site/boards.js` (set AtomS3R `displayOverlay` + `supports.display: true`)
- Modify: `site/test/drift.test.mjs` (assert the overlay stays in sync with the board file)

**Interfaces:**
- Consumes: `boards/atoms3r-display.yaml` (verbatim source of the overlay).

- [ ] **Step 1: Populate the overlay in `boards.js`**

Set the AtomS3R board's `displayOverlay` to a template string containing the `external_components`, `i2c`, `lp5562`, `spi`, `output`, `light`, and `display` blocks **copied verbatim** from `boards/atoms3r-display.yaml`, and flip its `supports.display` to `true`. Keep a stable marker line at the top:

```js
// in the esp32s3-atoms3r entry:
supports: { ble: true, display: true },
displayOverlay: `# --- AtomS3R-Display overlay (from boards/atoms3r-display.yaml) ---
<PASTE THE external_components / i2c / lp5562 / spi / output / light / display BLOCKS VERBATIM HERE>`,
```

- [ ] **Step 2: Add a drift assertion for the overlay**

```js
// append to site/test/drift.test.mjs
import { getBoard as gb } from '../boards.js';
test('AtomS3R display overlay still matches boards/atoms3r-display.yaml', () => {
  const overlay = gb('esp32s3-atoms3r').displayOverlay;
  const src = readFileSync(join(repoRoot, 'boards/atoms3r-display.yaml'), 'utf8');
  // every non-comment, non-blank overlay line must appear in the source file
  for (const line of overlay.split('\n')) {
    const t = line.trim();
    if (!t || t.startsWith('#')) continue;
    assert.ok(src.includes(line.replace(/\s+$/, '')), `overlay line drifted: ${t}`);
  }
});
```

- [ ] **Step 3: Run the suite**

Run: `node --test site/test/`
Expected: PASS. Then re-run the Task 1 shape test — the display test now exercises a real overlay.

- [ ] **Step 4: Commit**

```bash
git add site/boards.js site/test/drift.test.mjs
git commit -m "feat(site): AtomS3R display overlay, drift-checked against board file"
```

---

### Task 9: CI — test + Pages deploy

**Files:**
- Create: `.github/workflows/pages.yml`
- Modify: `README.md` (link to the hosted wizard), `CHANGELOG.md` (`[Unreleased]` entry)

**Interfaces:** none (CI/config + docs).

- [ ] **Step 1: Write the workflow**

```yaml
# .github/workflows/pages.yml
name: Config Wizard
on:
  push:
    branches: [main]
    paths: ['site/**', '.github/workflows/pages.yml']
  pull_request:
    paths: ['site/**']
permissions:
  contents: read
  pages: write
  id-token: write
concurrency:
  group: pages
  cancel-in-progress: true
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: '20' }
      - run: node --test site/test/
  deploy:
    needs: test
    if: github.ref == 'refs/heads/main'
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deploy.outputs.page_url }}
    steps:
      - uses: actions/checkout@v4
      - uses: actions/configure-pages@v5
      - uses: actions/upload-pages-artifact@v3
        with: { path: 'site' }
      - id: deploy
        uses: actions/deploy-pages@v4
```

- [ ] **Step 2: Verify the workflow lints locally (optional)**

Run: `node --test site/test/`
Expected: PASS — confirms the exact command CI runs is green before pushing.

- [ ] **Step 3: Add README + CHANGELOG entries**

Add to `README.md` (near the top, under setup): a line linking to the wizard —
`**New to setup?** Generate a starter config for your board with the [Config Wizard](https://RAR.github.io/esphome-tigomonitor/).`

Add to `CHANGELOG.md` under `## [Unreleased]` → `### Added`:
`- **Config Wizard (GitHub Pages).** A static, client-side wizard that generates a complete, board-tailored starter YAML (AtomS3R, AtomS3, ESP32-P4 EVBoard, generic ESP32), encoding the per-board scaffolding (PSRAM mode/speed, partitions, esp_hosted, experimental-features flag) plus CCA (HTTP/BLE), cloud-import, and secrets toggles. Board data is drift-checked against boards/*.yaml in CI. Motivated by discussion #31.`

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pages.yml README.md CHANGELOG.md
git commit -m "ci(site): test + GitHub Pages deploy for the config wizard"
```

- [ ] **Step 5: One-time manual enablement (record, don't script)**

In GitHub repo Settings → Pages, set **Source = GitHub Actions**. First deploy runs on the next push to `main` touching `site/**`. Verify the published URL loads and the P4 preview contains `esp32_hosted:`.

---

## Self-Review

**Spec coverage:**
- Complete starter config → Tasks 4/6 (assembly + serializer, core sensors, empty sections). ✓
- Curated boards → Task 1 (4 base boards; AtomS3R+display via overlay in Task 8). ✓
- CCA HTTP/BLE, cloud, secrets toggles → Task 4 rules + Task 6 emission. ✓
- Per-board gotchas (P4 experimental + hosted, PSRAM, partitions, flash) → Task 1 data + Task 6 emission, verified by Task 3 drift-guard. ✓
- Static site / GitHub Pages hosting → Task 7 UI + Task 9 workflow. ✓
- CI drift-guard + YAML sanity → Tasks 3, 6, 9. ✓
- Client-side-only / no deps → Global Constraints, enforced by module design. ✓
- Non-goal (no panel builder, no custom board) → honored; not implemented. ✓

**Placeholder scan:** The only "paste verbatim" is Task 8's display overlay, which names the exact source file and lines — a concrete copy step, not a vague TODO. No other placeholders.

**Type consistency:** `Cfg` fields produced by `assembleConfig` (Task 4) — `esp32.frameworkComponents`, `esp32.hostedComponent`, `partitions`, `psram`, `hosted`, `tigoServer.ccaSource`, `displayOverlay`, `secrets` — are exactly the fields read by `toYaml`/`toSecretsYaml` (Task 6). `Board` fields (Task 1) consumed by `assembleConfig` match. `generateApiKey` (Task 5) signature matches its use in `wizard.js` (Task 7). Drift-guard field names (`extractBoardFields`, Task 2) match its consumption in Task 3.
