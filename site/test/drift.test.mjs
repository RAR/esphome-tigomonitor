import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { getBoard } from '../boards.js';
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
