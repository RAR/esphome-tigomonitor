// Synthetic API responses for the docs screenshots.
//
// ============================================================================
// EVERY VALUE IN THIS FILE IS INVENTED. Nothing is copied from a real install.
// ============================================================================
//
// These payloads are published on a public docs site, so they must never carry
// a real serial, barcode, MAC, SSID, IP, account or site ID. Identifiers use
// the ranges the IETF reserves for documentation, which makes "this is fake"
// checkable rather than a promise:
//
//   IPs   192.0.2.0/24          RFC 5737
//   MACs  00:00:5E:00:53:xx     RFC 7042
//
// Panel barcodes keep Tigo's real `04C05B` OUI — a public IEEE registration
// shared by every Tigo device — so the Node Table screenshot still looks like a
// Tigo system. Crucially they then carry a `0000` block that no real unit has,
// giving fake serials a shape the test can verify. Matching the OUI alone is
// NOT enough: the CCA MAC that leaked into the first draft began `04C05B` too.
//
// site/test/fixtures-privacy.test.mjs enforces all of the above in CI.
//
// ---------------------------------------------------------------------------
// Field names and nesting mirror the firmware's real JSON. They were checked
// against a live device once; hand-guessed names rendered "undefined" across
// the whole dashboard. If the firmware's JSON changes shape, this file goes
// stale SILENTLY — the screenshots still build, they just render blanks. That
// is the standing maintenance cost of generating docs images this way.

const now = Math.floor(Date.now() / 1000);

const STRINGS = [
  { label: 'String A', mppt: 'MPPT 1', inv: 0, base: 372 },
  { label: 'String B', mppt: 'MPPT 2', inv: 0, base: 366 },
  { label: 'String C', mppt: 'MPPT 3', inv: 1, base: 358 },
  { label: 'String D', mppt: 'MPPT 4', inv: 1, base: 349 },
];
const INVERTERS = [
  { name: 'Inverter 1', display_name: 'South Roof' },
  { name: 'Inverter 2', display_name: 'North Roof' },
];
const RATING = 400;

// Deterministic pseudo-jitter. No Math.random: identical input must give
// identical pixels, or every CI run produces a pointless image diff.
const wob = (i) => ((Math.sin(i * 12.9898) * 43758.5453) % 1 + 1) % 1;

export const devices = [];
let n = 0;
for (const [si, s] of STRINGS.entries()) {
  for (let p = 1; p <= 6; p++) {
    const i = n++;
    // One shaded panel, so the heatmap in the docs shows the thing the heatmap
    // is FOR. Twenty-four identical green tiles demonstrate nothing.
    const shaded = si === 1 && p === 3;
    const pin = shaded ? 121 : Math.round(s.base * (0.94 + wob(i) * 0.1));
    const vin = +(38.1 + wob(i + 40) * 2.2).toFixed(2);
    const eff = shaded ? 91.4 : +(98.9 + wob(i + 160) * 1.2).toFixed(2);
    devices.push({
      addr: (2 + i * 3).toString().padStart(4, '0'),
      barcode: `04C05B0000${(0x100000 + i * 4919).toString(16).toUpperCase()}`,
      name: `${s.label.slice(-1)}${p}`,
      string_label: s.label,
      voltage_in: vin,
      voltage_out: +(vin - 0.25).toFixed(2),
      current: +(pin / vin).toFixed(2),
      current_out: +((pin * eff / 100) / vin).toFixed(2),
      power_in: pin,
      power: +(pin * eff / 100).toFixed(1),
      power_out: +(pin * eff / 100).toFixed(1),
      peak_power: +(RATING * (0.92 + wob(i + 200) * 0.08)).toFixed(1),
      temperature: shaded ? 31.2 : +(41.6 + wob(i + 120) * 7).toFixed(1),
      rssi: Math.round(46 + wob(i + 280) * 18),
      duty_cycle: shaded ? 88.0 : 100.0,
      efficiency: eff,
      data_age_ms: Math.round(1800 + wob(i + 320) * 7000),
      stale: false,
    });
  }
}

const of = (label) => devices.filter((d) => d.string_label === label);
const sum = (a, k) => +a.reduce((t, d) => t + d[k], 0).toFixed(1);
const avg = (a, k) => +(a.reduce((t, d) => t + d[k], 0) / a.length).toFixed(2);
const mpptOf = (label) => STRINGS.find((s) => s.label === label).mppt;

const strings = STRINGS.map((s) => {
  const ds = of(s.label);
  return {
    label: s.label, display_label: '', inverter: s.mppt, mppt: s.mppt,
    panel_rating_w: RATING,
    total_power: sum(ds, 'power_in'), peak_power: sum(ds, 'peak_power'),
    total_current: +(avg(ds, 'current') * ds.length).toFixed(2),
    avg_voltage_in: avg(ds, 'voltage_in'), avg_voltage_out: avg(ds, 'voltage_out'),
    avg_temperature: avg(ds, 'temperature'), avg_efficiency: avg(ds, 'efficiency'),
    min_efficiency: Math.min(...ds.map((d) => d.efficiency)),
    max_efficiency: Math.max(...ds.map((d) => d.efficiency)),
    active_devices: ds.length, total_devices: ds.length,
  };
});

const inverters = INVERTERS.map((inv, ii) => {
  const ss = strings.filter((_, si) => STRINGS[si].inv === ii);
  return {
    name: inv.name, display_name: inv.display_name,
    mppts: ss.map((s) => s.mppt),
    total_power: +ss.reduce((a, s) => a + s.total_power, 0).toFixed(1),
    peak_power: +ss.reduce((a, s) => a + s.peak_power, 0).toFixed(1),
    active_devices: ss.reduce((a, s) => a + s.active_devices, 0),
    total_devices: ss.reduce((a, s) => a + s.total_devices, 0),
    strings: ss,
  };
});

// 30-minute cadence, matching what the firmware actually writes. Spans a full
// week so the History view's default Week tab has something to draw.
function series(hours, peak) {
  const out = [];
  const c = Math.round(hours * 2);
  for (let i = 0; i < c; i++) {
    const t = now - (c - i) * 1800;
    const d = new Date(t * 1000);
    const h = d.getHours() + d.getMinutes() / 60;
    const day = Math.max(0, Math.sin(((h - 6.2) / 12.4) * Math.PI));
    const cloud = 1 - 0.34 * Math.max(0, Math.sin(i * 0.31) * Math.sin(i * 0.11));
    const p = Math.round(peak * day * cloud);
    out.push({ t, p, e: +((p / 1000) * 0.5).toFixed(2) });
  }
  return out;
}

// The Topology editor's tree (/api/layout): the same site as /api/inverters,
// CCA-derived (not yet saved by hand), plus one panel the bus has found that
// no string holds yet. That panel is not in /api/devices: it hasn't reported.
// The second inverter numbers its own MPPTs: labels stay unique across the
// site (panels carry them), names need only be unique in their inverter.
const MPPT_NAMES = { 'MPPT 3': 'MPPT 1', 'MPPT 4': 'MPPT 2' };

const layout = {
  manual: false,
  inverters: INVERTERS.map((inv, ii) => ({
    name: inv.name,
    display_name: inv.display_name,
    mppts: STRINGS.filter((s) => s.inv === ii).map((s) => ({
      label: s.mppt,
      display_label: MPPT_NAMES[s.mppt] || '',
      strings: [{
        label: s.label, display_label: '', rating_w: RATING,
        panels: of(s.label).map((d) => ({ barcode: d.barcode, addr: d.addr, name: d.name, first_heard_unix: 0, known: false })),
      }],
    })),
  })),
  unplaced_mppts: [],
  unassigned: [{ barcode: '04C05B0000200001', addr: '0080', name: '', first_heard_unix: 0, known: false }],
  pending: null,
};

export const routes = {
  '/api/overview': {
    total_power: sum(devices, 'power_in'),
    total_current: +sum(devices, 'current').toFixed(2),
    avg_efficiency: avg(devices, 'efficiency'),
    avg_temperature: avg(devices, 'temperature'),
    active_devices: devices.length,
    max_devices: devices.length,          // all panels reporting: docs should
                                          // not show a system that looks broken
    total_energy: 41.82, total_energy_in: 43.06, total_energy_out: 41.82,
    today_energy: 41.82, month_energy: 604.3,
  },
  '/api/devices': { devices, count: devices.length },
  '/api/strings': { strings },
  '/api/inverters': { inverters, mppt_names: MPPT_NAMES },
  '/api/layout': layout,
  '/api/layout/pending': { pending: null },
  '/api/nodes': {
    nodes: devices.map((d, i) => ({
      addr: d.addr, first_heard_unix: 0, long_address: d.barcode, sensor_index: i, checksum: '',
      cca_validated: true, cca_label: d.name, cca_string: d.string_label,
      cca_mppt: mpptOf(d.string_label),
      cca_channel: `04C05B000000.${10 + i}`,
      last_heard_unix: 0, last_roster_unix: 0, known: false, setup: null,
    })),
    inverters: [],
    strings: strings.map((s) => ({ label: s.label, panel_rating_w: RATING })),
  },
  // Pi build only: firmware heard on the bus. One optimizer was updated.
  '/api/versions': {
    taps: [{ addr: '1201', model: 'Mgate', version: 'G8.59 Jan  1 2026', previous: '',
             since_unix_ms: 1767225600000, last_unix_ms: 1767225600000 }],
    optimizers: devices.map((d, i) => ({ addr: d.addr, model: 'Mnode', version: i === 4 ? 'K8.0130 (2D)' : 'K8.0120 (2D)',
      previous: i === 4 ? 'K8.0120 (2D)' : '', since_unix_ms: 1767225600000 + i * 60000, last_unix_ms: 1767225600000 + i * 60000 })),
  },
  '/api/status': {
    free_heap: 98740, total_heap: 310652,
    free_psram: 8289756, total_psram: 8388608,
    min_free_heap: 88440, min_free_psram: 8001336,
    uptime_sec: 187245, uptime_days: 2, uptime_hours: 4, uptime_mins: 0,
    esphome_version: '2026.7.2', compilation_time: 'Jan 01 2026 00:00:00',
    task_count: 16, internal_temp: 50.9,
    invalid_checksum: 37, missed_frames: 606, total_frames: 8629414,
    command_frames: 9653, frame_27_count: 510,
    network_connected: true, wifi_rssi: -53, wifi_ssid: 'example-wifi',
    ip_address: '192.0.2.24', mac_address: '00:00:5E:00:53:24',
    active_sockets: 4, max_sockets: 16,
    reset_reason: 'poweron',
  },
  '/api/health': { status: 'ok', uptime: 187245, heap_free: 98740, heap_min_free: 88440 },
  '/api/panels': {
    slots: devices.map((d, i) => ({
      slot: i, barcode: d.barcode.slice(-6), label: d.name,
      mppt: mpptOf(d.string_label), string: d.string_label,
    })),
    count: devices.length, max_slots: 48,
  },
  '/api/config': {
    power_calibration: { value: 1.0, default: 1.0, overridden: false },
    night_mode_timeout: { value: 60, default: 60, overridden: false },
    reset_at_midnight: { value: true, default: false, overridden: true },
    sync_cca_on_startup: { value: true, default: true, overridden: false },
    cca_ip: { value: '192.0.2.100', default: '192.0.2.100', overridden: false },
  },
  '/api/tsdb/stats': {
    littlefs: { total: 3145728, used: 884736 },
    // Age of the writer's RAM snapshot these figures came from; the page turns
    // it into "sampled Nm ago". Fixed value so screenshots stay reproducible.
    snapshot_age_ms: 240000,
    slots: { used: devices.length, next_free: devices.length, max: 48 },
    databases: [
      { label: 'system', available: true, records: 7840, max_records: 65472,
        writes: 7840, evictions: 0, oldest_ts: now - 7840 * 1800, newest_ts: now,
        size_bytes: 260968, params: 14 },
      { label: 'panels0', available: true, records: 7840, max_records: 10912,
        writes: 7840, evictions: 0, oldest_ts: now - 7840 * 1800, newest_ts: now,
        size_bytes: 282240, params: 16 },
      { label: 'panels1', available: true, records: 7840, max_records: 10912,
        writes: 7840, evictions: 0, oldest_ts: now - 7840 * 1800, newest_ts: now,
        size_bytes: 282240, params: 16 },
    ],
  },
  '/api/energy/history': {
    days: Array.from({ length: 14 }, (_, i) => ({
      date: new Date((now - (13 - i) * 86400) * 1000).toISOString().slice(0, 10),
      energy: +(28 + wob(i) * 22).toFixed(1),
    })),
  },
  '/api/cloud/status': { configured: false, email: '', expires: 0, system_id: 0 },
  '/api/cca': {
    connected: true, source: 'ble',
    device_info: JSON.stringify({
      unit_id: 'CCA-000000', serial: 'CCA-000000', firmware: '4.0.5-ct',
      hardware: 'CCA-3', uptime: '6d 4h', panels: 24, taps: 2,
      ip: '192.0.2.100', mac: '00:00:5E:00:53:64',
    }),
    last_sync_epoch: now - 412,
  },
  // Shape is {yaml: "..."} — NOT a plain-text body. Returning a bare string
  // renders an "Unexpected token '#'" banner across the Tools screenshot, and
  // capture.mjs will not catch it: the SPA catches the parse error itself, so
  // there is no page error to trip on. Only looking at the image found it.
  '/api/yaml': {
    yaml: [
      'sensor:',
      '  # Hub-level sensors (system-wide, no address required)',
      '  - platform: tigo_monitor',
      '    tigo_monitor_id: tigo_hub',
      '    name: "Total System Power"',
      '',
      '  # A1 (discovered - CCA: MPPT 1 / String A)',
      '  - platform: tigo_monitor',
      '    tigo_monitor_id: tigo_hub',
      '    address: "0002"',
      '    name: "A1"',
      '    power: {}',
      '    voltage_in: {}',
      '    temperature: {}',
      '',
      '  # A2 (discovered - CCA: MPPT 1 / String A)',
      '  - platform: tigo_monitor',
      '    tigo_monitor_id: tigo_hub',
      '    address: "0005"',
      '    name: "A2"',
      '    power: {}',
      '    voltage_in: {}',
      '    temperature: {}',
    ].join('\n'),
  },
};

export const patterns = [
  [/\/api\/history\/power/, (u) => {
    const range = new URL(u, 'http://x').searchParams.get('range') || 'day';
    const hours = { day: 24, week: 168, month: 720, year: 8760 }[range] ?? 24;
    return { range, interval_min: 60, records: series(Math.min(hours, 336), 9000), count: 48, query_ms: 118 };
  }],
  [/\/api\/history\/panel/, () => ({ slot: 0, range: 'day', interval_min: 60, records: series(24, 392), count: 48, query_ms: 31 })],
];

// ---------------------------------------------------------------------------
// The Raspberry Pi build's pages (Bus › Control, Bus › Traffic, Devices with
// the TAP), served on top of `routes` when capture.mjs runs in Pi mode.
// Invented, like everything here: gateway 1201, the panels' own node numbers.
// Fixed times, so the pixels are the same on every run.
const PI_T = 1767268800000;  // 2026-01-01 12:00 UTC
const node = (i) => devices[i].addr;
let exId = 1;
const exch = (dt_ms, o) => ({
  id: exId++, unix_ms: PI_T + dt_ms, ours: true, other_master: false, addr: '1201', cmd: '0B0F', name: 'Tunnel',
  sub_name: '', node: null, category: 'other', result: 'answered', late: false, rt_ms: 4,
  summary: '', answer: '', answer_after_ms: null, request_hex: '', reply_hex: '', reconfig: '', ...o,
});
const disc = (dt) => exch(dt, {
  sub_name: 'DiscState', category: 'discovery', rt_ms: 5,
  summary: `Open, ${612 - Math.round(dt / 1000)} s left · 24 of the 24 in the table have joined`,
  request_hex: '00 00 00 2E 05', reply_hex: '00 0E 00 2F 05 01 01 9C 00 18 00 18 00 18 00',
});
const pgw = (dt, i) => exch(dt, {
  sub_name: 'SetPgw', category: 'housekeeping', node: node(i), rt_ms: 4,
  summary: `Preferred gateway for node ${node(i)} · stream 01`,
  request_hex: `00 00 00 41 0A 00 ${node(i).slice(0, 2)} ${node(i).slice(2)} 00 00 00 01 00`, reply_hex: '00 0E 00 42 0A',
});
const query = (dt, i, q, answer) => exch(dt, {
  sub_name: 'EncapString', category: 'queries', node: node(i), result: 'delivered', rt_ms: 6,
  summary: q, answer, answer_after_ms: answer ? 1840 : null,
  request_hex: `00 00 00 06 0B 00 ${node(i).slice(2)} 5E 30 30`, reply_hex: '00 0D 00 07 0B',
});
const page = (dt, from, n) => exch(dt, {
  sub_name: 'GetTable', category: 'housekeeping', rt_ms: n ? 31 : 8,
  summary: n ? `${n} entries from index ${from}` : `The end of the table, from index ${from}`,
  request_hex: `00 00 00 26 0C 00 ${from.slice(2)}`, reply_hex: '00 0E 00 27 0C',
});
const piExchanges = [
  disc(0),
  pgw(2100, 3),
  query(4300, 5, '^00Version', 'Mnode Version K8.0130 (2D)'),
  pgw(19600, 9),
  disc(20000),
  query(22400, 11, '^00Info', '!Info 0027 12 0000'),
  page(30000, '0002', 12),
  page(30400, '000E', 12),
  page(30800, '001A', 0),
  pgw(37100, 14),
  disc(40000),
  query(41900, 17, '^00Mppt', ''),
  exch(43500, { sub_name: 'EncapString', category: 'queries', node: node(20), result: 'not_delivered', rt_ms: 5,
    summary: "^00Version · the TAP couldn't deliver it (0x0146): the optimizer may be off or rejoining",
    request_hex: `00 00 00 06 0E 00 ${node(20).slice(2)} 5E 30 30`, reply_hex: '00 02' }),
  pgw(54600, 21),
  disc(60000),
];

export const piRoutes = {
  '/api/status': { ...routes['/api/status'], platform: 'linux' },
  '/api/settings': {
    serial_port: '/dev/serial/by-id/usb-FTDI_USB_RS485-if00-port0', bus_role: 'master', standby_takeover_s: 3,
    master_startup: true, master_housekeeping: true, master_queries: true, master_optimizer_writes: false,
    gateway_id: 0x1201, web_port: 0, web_port_active: 80, web_username: 'admin', web_password_set: true,
    tls_cert: '', tls_key: '', tls_active: false, tls_from_args: false, power_calibration: 1, publish_interval_s: 30,
    device_id: 'tigomonitor', show_cca_page: true, show_cloud_page: true,
    mqtt: { host: '192.0.2.10', port: 1883, username: 'tigomonitor', password_set: true, discovery: true,
      discovery_prefix: 'homeassistant', base_topic: 'tigomonitor', standby_panels: false, pv_switch: true },
    mqtt_connected: true, mqtt_error: '',
  },
  '/api/master': {
    role: 'master', active: true, fault: 'none', phase: 'running', detail: '', extra_taps: [],
    housekeeping: { on: true, learned_41: 24, sent_41: 1412, sent_session: 3, sent_broadcast: 6, sent_walk_pages: 96, walks_done: 32 },
    queries: { on: true, writes: false, sent_reads: 640, sent_writes: 0, answers: 598 },
    tap_version: 'Mgate Version G8.59 Jan  1 2026', tap: { lost: 0, last_lost_unix: 0, recent_lost_unix: [], back_without_discovery: 0 },
    discovery_read: true, discovery: { running: true, remaining_s: 552, node_count: 24 }, renewals: 41, discovery_renewing: true,
    polls: 712400, timeouts: 3, resyncs: 0, gap_ms: { max: 39, max_at_unix: 1767225600, recent: 12 }, last_reply_unix: PI_T / 1000,
    pv: { on: true, source: 'setting', since: 0, confirmed: true, unconfirmed: false, last_sent: '01', last_reply: '01' },
    standby: { state: 'off', takeover_s: 3, master_heard_ms_ago: -1, takeovers: 0, last_takeover_unix: 0, yields: 0 },
  },
  '/api/bus': {
    alarm: { active: false, count: 0, first_unix_ms: 0, recent: [] },
    stats: { window_s: 300, polls: 13180, polls_lost: 0, polls_empty: 12950, polls_refused: 0,
      count: { discovery: 4, queries: 4, housekeeping: 7, bringup: 0, reconfig: 0, other: 0 }, delivered: 3, not_delivered: 1,
      other_master: 0, reconfig: 0, rt_median_ms: 4, rt_max_ms: 31,
      discovery: 'Open, 552 s left · 24 of the 24 in the table have joined' },
    exchanges: piExchanges,
  },
};
