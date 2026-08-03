// site/test/yaml.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { BOARDS, getBoard } from '../boards.js';
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

test('P4 emits esp32_hosted + 200MHz PSRAM + experimental flag', () => {
  const p4 = toYaml(assembleConfig(getBoard('esp32p4-evboard'), { ...form, uart: { tx_pin: 'GPIO20', rx_pin: 'GPIO21' } }));
  assert.ok(p4.includes('esp32_hosted:'));
  assert.ok(p4.includes('speed: 200MHz'), 'P4 default is 200MHz (valid P4 speeds: 20/100/200)');
  assert.ok(p4.includes('enable_idf_experimental_features: true'), 'P4 emits the experimental flag');
  assert.ok(p4.includes('execute_from_psram: true'), 'P4 emits execute_from_psram (XIP) — fixes #31 boot crash');
  assert.ok(!p4.includes('80MHz'), 'P4 must not emit 80MHz — invalid for P4 (cv.one_of 20/100/200)');
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

test('HTTP CCA emits cca_source and cca_ip', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, cca: 'http', ccaIp: '10.0.0.9' }));
  assert.ok(y.includes('cca_source: http'), 'cca_source: http missing');
  assert.ok(y.includes('cca_ip: 10.0.0.9'), 'cca_ip missing');
});

test('static IP emits a manual_ip block', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, wifi: { ssid: 'net', password: 'pw', staticIp: '10.0.0.50' } }));
  assert.ok(y.includes('manual_ip:'), 'manual_ip block missing');
  assert.ok(y.includes('static_ip: 10.0.0.50'), 'static_ip missing');
});

test('displayOverlay is appended verbatim at the end', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), form);
  cfg.displayOverlay = '# OVERLAY_MARKER\ndisplay:\n  - platform: st7789v';
  const y = toYaml(cfg);
  assert.ok(y.includes('# OVERLAY_MARKER'), 'overlay marker missing');
  assert.ok(y.trimEnd().endsWith('platform: st7789v'), 'overlay not appended at end');
});

test('generated config sources the tigo components via external_components', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), form));
  assert.ok(y.includes('external_components:'), 'no external_components block');
  assert.ok(y.includes('url: https://github.com/RAR/esphome-tigomonitor'), 'tigo url missing');
  assert.ok(y.includes('ref: next'), 'ref missing');
  assert.ok(y.includes('components: [tigo_monitor, tigo_server]'), 'tigo components missing');
  assert.equal((y.match(/^external_components:/gm) || []).length, 1, 'must have exactly one external_components key');
});

test('display config merges lp5562 into the single external_components block', () => {
  const cfg = assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, display: true });
  const y = toYaml(cfg);
  assert.equal((y.match(/^external_components:/gm) || []).length, 1, 'display config must not duplicate external_components');
  assert.ok(y.includes('https://github.com/RAR/esphome-lp5562'), 'lp5562 source missing when display on');
  assert.ok(y.includes('components: [tigo_monitor, tigo_server]'), 'tigo source still present');
});

test('AtomS3R+display config defines every id its lambda references', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, display: true }));
  const referenced = [...new Set([...y.matchAll(/id\((\w+)\)/g)].map((m) => m[1]))];
  const defined = new Set([...y.matchAll(/^\s*id:\s*(\w+)/gm)].map((m) => m[1]));
  const missing = referenced.filter((r) => !defined.has(r));
  assert.deepEqual(missing, [], `display config references undefined ids: ${missing.join(', ')}`);
});

test('display config wires the backlight into tigo_server', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, display: true }));
  assert.ok(y.includes('backlight: lcd_backlight'), 'backlight wiring missing');
});

test('every board emits a psram block and a Free PSRAM sensor', () => {
  for (const b of BOARDS) {
    const y = toYaml(assembleConfig(b, { ...form, uart: b.uartDefault }));
    assert.ok(y.includes('\npsram:'), `${b.id} must emit psram — PSRAM is required`);
    assert.ok(y.includes('Free PSRAM'), `${b.id} should emit the Free PSRAM sensor`);
  }
});

test('cca:ble emits the full BLE stack + ble_client_id', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, cca: 'ble', ccaMac: '04:C0:5B:A1:A9:4B' }));
  assert.ok(y.includes('esp32_ble:'), 'esp32_ble block missing');
  assert.ok(y.includes('esp32_ble_tracker:'), 'esp32_ble_tracker missing');
  assert.ok(y.includes('ble_client:'), 'ble_client missing');
  assert.ok(y.includes('mac_address: "04:C0:5B:A1:A9:4B"'), 'CCA MAC missing');
  assert.ok(y.includes('id: tigo_cca_ble'), 'ble_client id missing');
  assert.ok(y.includes('ble_client_id: tigo_cca_ble'), 'tigo_server ble_client_id missing');
  assert.ok(y.includes('use_psram: true'), 'esp32_ble use_psram missing (BLE board has PSRAM)');
});

test('cca:ble falls back to a placeholder MAC when none given', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), { ...form, cca: 'ble' }));
  assert.ok(y.includes('mac_address: "04:C0:5B:00:00:00"'), 'placeholder MAC missing');
});

test('no BLE blocks or ble_client_id when cca is not ble', () => {
  const y = toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), form));
  assert.ok(!y.includes('esp32_ble:'), 'esp32_ble emitted without cca:ble');
  assert.ok(!y.includes('ble_client_id'), 'ble_client_id emitted without cca:ble');
});

test('board sdkconfigBle is emitted only when the config selects BLE', () => {
  // The P4 needs CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n for repeatable CCA-over-BLE:
  // its C6 companion refuses LE_Extended_Create_Connection (0x2043), which Bluedroid
  // switches to after the first session. Without it only one connect per boot works.
  const SYM = 'CONFIG_BT_BLE_50_FEATURES_SUPPORTED: "n"';
  const bleForm = { ...form, cca: 'ble', ccaMac: '04:C0:5B:A1:A9:4B' };

  assert.ok(toYaml(assembleConfig(getBoard('esp32p4-evboard'), bleForm)).includes(SYM),
    'P4 + BLE must emit the BLE 5.0 opt-out');
  assert.ok(!toYaml(assembleConfig(getBoard('esp32p4-evboard'), form)).includes(SYM),
    'P4 without BLE must not carry the inert symbol');
  assert.ok(!toYaml(assembleConfig(getBoard('esp32s3-atoms3r'), bleForm)).includes(SYM),
    'boards without sdkconfigBle must be unaffected');
});
