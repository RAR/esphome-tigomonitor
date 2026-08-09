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

// PSRAM used to be required of every board. PR #44 added a supported no-PSRAM
// tier (sensors to Home Assistant, no web server), so the invariant is no longer
// "everything has PSRAM" — it is that PSRAM and the web server travel together.
// tigo_server builds whole HTML pages and JSON responses in memory from the
// httpd task; without PSRAM that fragments the internal heap to OOM.
test('PSRAM and the web server imply each other', () => {
  for (const b of BOARDS) {
    const hasPsram = Boolean(b.psram?.mode && b.psram?.speed);
    const webServer = b.supportsWebServer !== false;
    assert.equal(webServer, hasPsram,
      `${b.id}: web server ${webServer ? 'offered' : 'withheld'} but PSRAM ${hasPsram ? 'present' : 'absent'}`);
  }
});

// Everything in the wizard's "CCA & extras" card is a tigo_server feature, so a
// board without the web server must not advertise any of them.
test('a board without the web server offers no server-only features', () => {
  for (const b of BOARDS) {
    if (b.supportsWebServer !== false) continue;
    assert.equal(b.supports.ble, false, `${b.id} offers BLE without a web server`);
    assert.equal(b.supports.display, false, `${b.id} offers a display without a web server`);
    assert.equal(b.partitions, null, `${b.id} has tsdb partitions but no web server to serve history`);
    assert.deepEqual(b.frameworkComponents, [], `${b.id} pulls in tsdb components it cannot use`);
  }
});

test('board ids are unique and getBoard resolves them', () => {
  const ids = BOARDS.map((b) => b.id);
  assert.equal(new Set(ids).size, ids.length, 'duplicate board id');
  for (const id of ids) assert.equal(getBoard(id)?.id, id);
});

test('P4 defines a hosted radio + 200MHz PSRAM with the experimental flag', () => {
  const p4 = getBoard('esp32p4-evboard');
  assert.ok(p4.hosted, 'P4 must define an esp32_hosted companion');
  assert.equal(p4.psram.speed, '200MHz', 'P4 valid speeds are 20/100/200 — 200 default');
  assert.equal(p4.frameworkAdvanced.enable_idf_experimental_features, true);
  assert.equal(p4.frameworkAdvanced.execute_from_psram, true, 'P4 needs XIP-from-PSRAM to boot (#31)');
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
