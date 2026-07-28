// The screenshot fixtures get rendered into images published on a public docs
// site. Nothing in them may come from a real install.
//
// This is not paranoia — the first draft of these fixtures was written by
// mirroring a live device's /api responses to get the field names right, and it
// silently carried that device's CCA MAC and inverter model names along with
// them. Shape-correctness and privacy pull in opposite directions here, so the
// rule needs a test rather than a comment.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { routes, patterns, devices } from '../screenshots/fixtures.mjs';

const blob = JSON.stringify({
  routes,
  devices,
  patterns: patterns.map(([re, fn]) => [String(re), fn('/api/history/power?range=day')]),
});

test('IPs are inside the RFC 5737 documentation range', () => {
  const ips = blob.match(/\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/g) ?? [];
  assert.ok(ips.length > 0, 'expected at least one IP, so this test can fail if one leaks');
  for (const ip of ips) {
    assert.match(ip, /^192\.0\.2\.\d{1,3}$/,
      `${ip} is not in 192.0.2.0/24 (RFC 5737 documentation range)`);
  }
});

test('MACs are inside the RFC 7042 documentation range', () => {
  const macs = blob.match(/\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b/g) ?? [];
  assert.ok(macs.length > 0, 'expected at least one MAC');
  for (const mac of macs) {
    assert.match(mac, /^00:00:5E:00:53:[0-9A-Fa-f]{2}$/,
      `${mac} is not in 00:00:5E:00:53:00-FF (RFC 7042 documentation range)`);
  }
});

test('bare-hex identifiers use the reserved fake-serial shape', () => {
  // Prefix alone is NOT sufficient. The CCA MAC that leaked into the first
  // draft of these fixtures began with the public Tigo OUI itself, so an
  // OUI-only check waves real hardware straight through. Fake serials must
  // also carry the 0000 block, which no real unit has. (Deliberately not
  // quoting the leaked value here — that would put it back in the repo.)
  // Scan hex runs inside quoted values only. A bare /[0-9A-F]{10,}/ over the
  // whole blob also matches unquoted epoch timestamps — 1771136965 is ten
  // characters all of which are valid hex digits.
  const hex = [...blob.matchAll(/"([^"]*)"/g)]
    .flatMap((m) => m[1].match(/[0-9A-F]{10,}/g) ?? []);
  assert.ok(hex.length > 0, 'expected barcodes to be present');
  for (const h of hex) {
    assert.match(h, /^04C05B0000[0-9A-F]*$/,
      `${h} looks like a real serial: expected the 04C05B0000 fake-serial prefix`);
  }
});

test('no real-world SSIDs, hostnames or vendor model names', () => {
  // Generic-by-construction. A fixture naming an actual inverter model tells a
  // reader what hardware the author owns, which is exactly what we are avoiding.
  const banned = [
    /\.local\b/i,          // mDNS hostnames
    /flexboss/i,           // inverter model that leaked into the first draft
    /\bhamkins/i,          // SSID that leaked into the first draft
  ];
  for (const re of banned) {
    assert.doesNotMatch(blob, re, `fixture matched banned pattern ${re}`);
  }

  // Allowlist, not a denylist: a banned-substring list only catches leaks you
  // already thought of. NOTE the key is `wifi_ssid`, so an anchored /\bssid/
  // never matches — that exact mistake let a real SSID through the first
  // version of this test.
  const ssids = [...blob.matchAll(/"[a-z_]*ssid"\s*:\s*"([^"]*)"/gi)].map((m) => m[1]);
  assert.ok(ssids.length > 0, 'expected an SSID field to be present');
  for (const s of ssids) {
    assert.equal(s, 'example-wifi', `SSID ${JSON.stringify(s)} is not the placeholder`);
  }
});

test('fixtures still describe a complete, healthy system', () => {
  // Guards the docs against showing a install that looks broken.
  assert.equal(routes['/api/overview'].active_devices, devices.length);
  assert.equal(routes['/api/overview'].max_devices, devices.length,
    'active must equal max, or the dashboard reads "75% online"');
  assert.ok(devices.every((d) => d.name && d.addr), 'every panel needs a name and addr');
  assert.equal(devices.filter((d) => d.power_in < 200).length, 1,
    'exactly one underperforming panel, so the heatmap demonstrates something');
});

test('capture output is deterministic across calls', () => {
  // Math.random in the fixtures would make every CI run produce a new image.
  const a = JSON.stringify(routes['/api/devices']);
  const b = JSON.stringify(routes['/api/devices']);
  assert.equal(a, b);
});
