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
