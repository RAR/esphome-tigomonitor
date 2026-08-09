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
  'esp32p4-evboard': 'boards/esp32p4-evboard.yaml',
  'esp32-lilygo-t-can485': 'boards/esp32-lilygo-t-can485.yaml',
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
    assert.equal(Boolean(b.frameworkAdvanced.execute_from_psram), f.executeFromPsram, 'execute_from_psram flag drift');
    assert.equal(Boolean(b.hosted), f.hasHosted, 'esp32_hosted presence drift');
    assert.equal(b.frameworkAdvanced.minimum_chip_revision ?? null, f.minimumChipRevision,
      'minimum_chip_revision drift — 3.0 vs 3.1 decides whether ECO3 parts boot');
    assert.equal(Boolean(b.frameworkAdvanced.sram1_as_iram), f.sram1AsIram, 'sram1_as_iram drift');
    assert.equal(b.supportsWebServer !== false, f.hasWebServer, 'tigo_server presence drift');
  });
}

test('every checked board still exists in BOARDS', () => {
  for (const id of Object.keys(FILE)) assert.ok(getBoard(id), `missing board ${id}`);
});

// NOTE: this is a subset check (overlay lines ⊆ source). It does NOT verify the
// overlay is complete. Completeness of the generated display config (e.g. that
// every id its lambda references is defined) is covered in yaml.test.mjs.
test('AtomS3R display overlay still matches boards/atoms3r-display.yaml', () => {
  const overlay = getBoard('esp32s3-atoms3r').displayOverlay;
  const src = readFileSync(join(repoRoot, 'boards/atoms3r-display.yaml'), 'utf8');
  // every non-comment, non-blank overlay line must appear in the source file
  for (const line of overlay.split('\n')) {
    const t = line.trim();
    if (!t || t.startsWith('#')) continue;
    assert.ok(src.includes(line.replace(/\s+$/, '')), `overlay line drifted: ${t}`);
  }
});
