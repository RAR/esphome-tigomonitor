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
