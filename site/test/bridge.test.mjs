// site/test/bridge.test.mjs
// The Config Builder's network bridge: a board that forwards the raw RS485
// bus over TCP and decodes nothing. What keeps a bridge off the roof bus is
// hardware (no tx_pin, and the transceiver's driver held off by a switch), so
// the generated config is checked block by block against the example files in
// boards/, which are the ones that were built and, for the AtomS3, run on a bus.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { BRIDGE_BOARDS, getBridgeBoard, assembleBridge, toBridgeYaml } from '../bridge.js';
import { toSecretsYaml } from '../yaml.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

const form = {
  name: '', wifi: { ssid: 'net', password: 'pw', staticIp: '' },
  useSecrets: false, apiKey: 'KEY==', otaPassword: 'ota',
};

// Top-level YAML blocks, comments and blank lines dropped, so a block compares
// on what ESPHome reads rather than on how it is annotated.
function blocks(yaml) {
  const out = {};
  let key = null;
  for (const raw of yaml.split('\n')) {
    const line = raw.replace(/\s+#.*$/, '').replace(/^\s*#.*$/, '').trimEnd();
    if (!line) continue;
    const top = /^([a-z_0-9]+):/.exec(line);
    if (top) { key = top[1]; out[key] = []; }
    out[key].push(line);
  }
  return Object.fromEntries(Object.entries(out).map(([k, v]) => [k, v.join('\n')]));
}

// Everything that touches the bus, the chip or the stream. wifi:, api: and ota:
// differ on purpose: the builder fills in the user's network and a real key.
const HARDWARE_BLOCKS = ['esp32', 'external_components', 'switch', 'uart', 'stream_server',
  'logger', 'binary_sensor', 'sensor'];

test('there is a bridge for each example bridge file, and each file exists', () => {
  assert.deepEqual(BRIDGE_BOARDS.map((b) => b.example).sort(),
    ['boards/example-atoms3-bridge.yaml', 'boards/example-t-can485-bridge.yaml']);
  for (const b of BRIDGE_BOARDS) assert.equal(getBridgeBoard(b.id), b);
});

for (const b of BRIDGE_BOARDS) {
  test(`${b.id}: the bus, chip and stream blocks match ${b.example}`, () => {
    const want = blocks(readFileSync(resolve(ROOT, b.example), 'utf8'));
    const got = blocks(toBridgeYaml(assembleBridge(b, form)));
    for (const k of HARDWARE_BLOCKS) {
      assert.ok(want[k], `${b.example} has no ${k}: block`);
      assert.equal(got[k], want[k], `${k}: differs from ${b.example}`);
    }
  });

  test(`${b.id}: the UART has no transmit pin, and every pin the board drives stays on`, () => {
    const y = toBridgeYaml(assembleBridge(b, { ...form, uart: { tx_pin: 'GPIO6' } }));
    assert.doesNotMatch(y, /^\s*tx_pin:/m);
    assert.ok(b.holdPins.length > 0, 'a bridge must hold its transceiver driver off');
    for (const p of b.holdPins) {
      assert.match(y, new RegExp(`pin: ${p.pin}\\b[^\\n]*\\n\\s+restore_mode: ALWAYS_ON`));
    }
  });

  test(`${b.id}: Wi-Fi never sleeps, and the network and secrets come from the form`, () => {
    const cfg = assembleBridge(b, { ...form, useSecrets: true,
      wifi: { ssid: 'net', password: 'pw', staticIp: '10.0.0.46' } });
    const y = toBridgeYaml(cfg);
    assert.match(y, /^  power_save_mode: none$/m);
    assert.match(y, /^  ssid: !secret wifi_ssid$/m);
    assert.match(y, /^    key: !secret api_encryption_key$/m);
    assert.match(y, /^    password: !secret ota_password$/m);
    assert.match(y, /static_ip: 10\.0\.0\.46\n[\s\S]*gateway: 10\.0\.0\.1\n[\s\S]*dns1: 10\.0\.0\.1/);
    assert.equal(toSecretsYaml(cfg),
      'wifi_ssid: net\nwifi_password: pw\napi_encryption_key: KEY==\nota_password: ota\n');
  });
}

test('a bridge is named tigo-bridge unless the form names it', () => {
  const b = BRIDGE_BOARDS[0];
  assert.match(toBridgeYaml(assembleBridge(b, form)), /^  name: tigo-bridge$/m);
  assert.match(toBridgeYaml(assembleBridge(b, { ...form, name: 'cabinet' })), /^  name: cabinet$/m);
});

test('a bridge config pulls in no TigoMonitor component', () => {
  for (const b of BRIDGE_BOARDS) {
    const y = toBridgeYaml(assembleBridge(b, form));
    assert.doesNotMatch(y, /tigo_monitor|tigo_server|psram:/);
  }
});
