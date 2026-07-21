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
  assert.deepEqual(f.components, ['zakery292/esp_tsdb', 'joltwallet/littlefs^1.16']);
});

test('a commented-out experimental flag reads as NOT enabled', () => {
  const commented = `
esp32:
  framework:
    # advanced:
    #   enable_idf_experimental_features: yes
psram:
  mode: hex
  speed: 80MHz
`;
  const f = extractBoardFields(commented);
  assert.equal(f.experimental, false);
  assert.equal(f.psramSpeed, '80MHz');
});

test('absent fields come back null/false', () => {
  const f = extractBoardFields('esp32:\n  board: esp32dev\n');
  assert.equal(f.flash_size, null);
  assert.equal(f.partitions, null);
  assert.equal(f.psramMode, null);
  assert.equal(f.experimental, false);
  assert.equal(f.hasHosted, false);
});
