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

test('P4 defines a hosted radio and defaults to no experimental flag', () => {
  const p4 = getBoard('esp32p4-evboard');
  assert.ok(p4.hosted, 'P4 must define an esp32_hosted companion');
  // Defaults to 80MHz PSRAM, which needs no experimental gate. 200MHz on the
  // Function-EV-Board is an explicit opt-in, not the wizard's default.
  assert.equal(p4.frameworkAdvanced.enable_idf_experimental_features, false);
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
