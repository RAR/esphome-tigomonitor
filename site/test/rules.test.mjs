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

// Every shipping board happens to support BLE, so this guards the rule itself
// against a future board that doesn't.
test('BLE on an unsupported board throws', () => {
  const noBle = { ...getBoard('esp32s3-atoms3r'), id: 'no-ble', supports: { ble: false, display: false } };
  assert.throws(() => assembleConfig(noBle, { ...baseForm, cca: 'ble' }), /BLE/);
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
