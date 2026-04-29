# esp_tsdb Integration Plan

Persistent on-flash time-series storage for Tigo Monitor, replacing the
volatile in-RAM history that resets on every reboot.

Component: [`zakery292/esp_tsdb` v2.0.2](https://components.espressif.com/components/zakery292/esp_tsdb/versions/2.0.2/readme).

---

## Goals

- History survives reboots and OTA updates.
- Power & energy charts for day / week / month / year ranges.
- Per-panel history at useful resolution without burning flash.
- Query latency <500ms for dashboard ranges (week view).
- No regression in UART frame throughput (currently ~14 fps, 0.003% loss).

## Non-goals

- Full per-panel V/I/T at 5-min resolution (too many series — see schema).
- Real-time streaming of historical data; UI polls.
- Replacing `/api/energy-history` immediately — run side-by-side first.

---

## Constraints from esp_tsdb

| Constraint | Implication |
|---|---|
| 64 params max per DB | Need to tier by aggregation level; can't store all per-panel metrics in one DB |
| `int16_t` values only | Scale at write, descale at read; document scaling per param |
| LittleFS-backed | Need a flash partition; pick size up front (resizing erases) |
| ~290ms write avg, 412ms p95 | Write off the UART task; never under the state mutex |
| Sequential overflow on schema add | Design param list up front; migrations leave dead flash |
| 5-min × 1 week query ≈ 130ms | Fast enough for week view; pre-aggregate for month/year |

---

## Schema design

Two databases. The split keeps system rollups fast and per-panel data lazy.

### `system.tsdb` — system + per-inverter rollups (5-min resolution)

Sized for a 4-inverter system; 14 base params, room for growth.

| # | Name | Unit | Scale | Range covered |
|---|---|---|---|---|
| 0 | `total_p` | W | ×1 | 0–32 kW |
| 1 | `total_e` | kWh ×100 | ×100 | 0–327 kWh per period (resets daily; cumulative kept elsewhere) |
| 2 | `inv1_p` | W | ×1 | 0–32 kW per inverter |
| 3 | `inv1_e` | kWh ×100 | ×100 | |
| 4 | `inv2_p` | W | ×1 | |
| 5 | `inv2_e` | kWh ×100 | ×100 | |
| 6 | `inv3_p` | W | ×1 | |
| 7 | `inv3_e` | kWh ×100 | ×100 | |
| 8 | `inv4_p` | W | ×1 | |
| 9 | `inv4_e` | kWh ×100 | ×100 | |
| 10 | `temp_avg` | °C | ×1 | -40 to 100 |
| 11 | `freq` | dHz | ×10 | 0–6553 dHz (= 0–655 Hz) |
| 12 | `frames_lost` | count | ×1 | per 5-min window |
| 13 | `wifi_rssi` | dBm | ×1 | -100 to 0 |

**Why these:** drives every dashboard chart, every "today/week/month" stat, every inverter-level alert. ~55 params reserved (extras) for future per-string rollups when we add them.

**Storage estimate:** 14 params × 2 bytes + 4-byte timestamp = 32 bytes/record. At 5-min cadence: 288 records/day × 32 bytes = ~9 KB/day, ~3.3 MB/year. **Sized at 8 MB → ~2.4 years** of 5-min data with index overhead.

### `panels.tsdb` — per-panel power (5-min resolution)

Power only. V/I/T at 5-min would triple writes for marginal benefit; if a panel is misbehaving the dashboard's live view shows V/I/T already.

| # | Name | Unit | Scale | Range |
|---|---|---|---|---|
| 0..63 | `p00`..`p63` | W | ×1 | 0–500 W per panel |

**Capacity:** 64 panels max in one DB. Most installs are <48; for larger systems, instantiate `panels2.tsdb`. We don't need to pay for that complexity until someone hits the limit.

**Storage estimate:** 64 × 2 bytes + 4-byte ts = 132 bytes/record. 288/day × 132 = ~38 KB/day, ~14 MB/year. **Sized at 16 MB → ~14 months** at full panel count.

**Mapping:** stable index assigned per barcode at first sight, persisted alongside the DB in a small JSON file (`panel_map.json` on LittleFS). New panels get the next free slot. Removed panels keep their slot (don't shuffle indices — would invalidate history).

### `daily.tsdb` — pre-aggregated daily rollup

Hand-rolled rollup written nightly (00:01 local), so month/year queries don't scan 5-min data.

| # | Name | Unit | Scale |
|---|---|---|---|
| 0 | `energy` | Wh | ×1 (range 0–32767 Wh = 32.7 kWh — *insufficient*; use kWh ×100 instead) |
| 0 | `energy` | kWh ×100 | ×100 (range 0–327 kWh/day) |
| 1 | `peak_p` | W | ×1 |
| 2 | `runtime_min` | min | ×1 |
| 3 | `frames_lost` | count | ×1 |
| 4..N | per-inverter daily energy | kWh ×100 | ×100 |

365 records/year, trivially small.

---

## Scaling reference

Stored as `int16_t`. Always apply scaling at write & read:

```c
// Write helpers
static inline int16_t enc_w(float watts) { return (int16_t)lroundf(watts); }
static inline int16_t enc_kwh(float kwh)  { return (int16_t)lroundf(kwh * 100.0f); }
static inline int16_t enc_v(float volts)  { return (int16_t)lroundf(volts * 10.0f); }

// Read helpers
static inline float dec_kwh(int16_t v)  { return v / 100.0f; }
```

Bounds-check before encoding — clamp to `INT16_MIN/MAX` to avoid silent wraparound on a runaway sensor.

---

## Partition table

Add a LittleFS partition. Current AtomS3R has 8 MB flash; existing partition table is ESPHome default. esp_tsdb wants ~24 MB total for our schema (8 + 16 + small daily). **The 8 MB AtomS3R can't fit this without compromise.**

Options:

**A. Reduced retention (fits 8 MB AtomS3R):**
- `system.tsdb` 4 MB → ~14 months of 5-min data
- `panels.tsdb` 3 MB → ~80 days of per-panel data
- `daily.tsdb` ~50 KB
- Leaves ~1 MB headroom for LittleFS overhead

**B. Recommend 16 MB board variant** (e.g., AtomS3R-M12 or generic ESP32-S3-N16R8) for full retention.

**Decision needed from user:** A is the safer default (works on existing hardware); B becomes the recommended config in `boards/` for long-term users. I'd ship both — code reads partition size at boot and adjusts `max_records` on `tsdb_init`.

Partition table snippet (option A):

```csv
# Name,    Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x300000,
ota_0,    app,  ota_0,   0x310000, 0x300000,
ota_1,    app,  ota_1,   0x610000, 0x180000,
tsdb,     data, littlefs,0x790000, 0x870000,
```

---

## Write path

### Task

Dedicated FreeRTOS task `tsdb_writer_task` (priority below UART, stack 4 KB).

Inbox: a small ring buffer (PSRAM) of pending samples, populated by a 5-min timer in the main app loop.

```cpp
// Pseudocode in tigo_monitor.cpp
void TigoMonitor::flush_to_tsdb_() {
  // Called every 5 minutes from a software timer.
  TsdbSample s;
  {
    StateLock lock(state_mutex_);   // brief — just snapshot rollups
    s.ts = now();
    s.total_p = enc_w(this->total_power_);
    s.total_e = enc_kwh(this->energy_5min_);
    for (size_t i = 0; i < inverters_.size() && i < 4; ++i) {
      s.inv_p[i] = enc_w(inverters_[i].power);
      s.inv_e[i] = enc_kwh(inverters_[i].energy_5min);
    }
    // ... per-panel snapshot too
  }  // lock released
  xQueueSend(tsdb_queue_, &s, 0);   // non-blocking; drop if full + log warn
}

// In tsdb_writer_task:
TsdbSample s;
while (xQueueReceive(tsdb_queue_, &s, portMAX_DELAY)) {
  int16_t row[14] = { s.total_p, s.total_e, s.inv_p[0], s.inv_e[0], ... };
  esp_err_t err = tsdb_write(s.ts, row);
  if (err != ESP_OK) ESP_LOGW(TAG, "tsdb write failed: %s", esp_err_to_name(err));
}
```

**Critical:** the 290ms write happens on the writer task, not the UART task or the main loop. The state mutex is only held during the snapshot (microseconds), not during the flash write.

### Cadence

- **5-min** for `system.tsdb` and `panels.tsdb` — one queue, two writes back-to-back.
- **Daily at 00:01** for `daily.tsdb` — rolled up from `system.tsdb` via `tsdb_aggregate(start_of_day, end_of_day, idx, TSDB_AGG_SUM, &out)`.
- **Synchronous flush** before OTA + before reboot (already a graceful-shutdown path in `tigo_monitor.cpp`; add a hook).

---

## Query path

New endpoints on `tigo_web_server`:

| Endpoint | Source DB | Resolution | Notes |
|---|---|---|---|
| `/api/history/power?range=day` | `system.tsdb` | 5-min | ~288 points |
| `/api/history/power?range=week` | `system.tsdb` | 5-min | ~2k points; dashboard week chart |
| `/api/history/power?range=month` | `system.tsdb` aggregated to hourly | hourly | ~720 points |
| `/api/history/power?range=year` | `daily.tsdb` | daily | ~365 points |
| `/api/history/energy?range=…` | `daily.tsdb` mostly | daily | bar chart |
| `/api/history/panel?id=P18&range=week` | `panels.tsdb` | 5-min | one column read |
| `/api/history/panels?range=week` | `panels.tsdb` | 5-min, all panels | for the per-panel bar chart |

Implementation pattern (handler runs on http_server task, NOT app task — already true today):

```cpp
// Inside the http handler. No state mutex needed; tsdb has its own internal locking.
tsdb_query_t q;
tsdb_query_init(&q, start, end, NULL, 0);
PSRAMString json;
json.reserve(64 * 1024);
json += "[";
uint32_t ts; int16_t row[14];
bool first = true;
while (tsdb_query_next(&q, &ts, row) == ESP_OK) {
  if (!first) json += ",";
  first = false;
  // emit {"t":1714410600,"p":7840,"e":326}
  ...
}
json += "]";
tsdb_query_close(&q);
```

**Cap response size** the same way we cap CCA responses — hard limit at 1 MB to prevent runaway queries.

---

## Bootstrapping & migration

- On first boot after upgrade: tsdb files don't exist → `tsdb_init` creates them. No data yet.
- The existing in-RAM `daily_energy_history_` (from `/api/energy-history`) keeps working in parallel for the first month so users don't lose visible history during the transition.
- After 30 days of tsdb data accumulated, `/api/energy-history` becomes a passthrough to `/api/history/energy?range=month` and the in-RAM ring buffer is removed.

---

## Risks & open questions

1. **Flash wear.** LittleFS does its own wear leveling. At 5-min cadence × 2 writes = 576 writes/day × ~3 KB each = ~1.7 MB/day. Flash endurance is ~100k erase cycles per sector; LittleFS spreads them. Should last >10 years at this rate, but worth measuring with `esp_partition_get_sha256` over time and surfacing in diagnostics.
2. **Concurrent flash I/O during OTA.** OTA writes flash; tsdb writes flash. The readme calls out 750ms latency spikes under concurrent load. Solution: pause the writer task during OTA (we already have an `ota` event hook in ESPHome).
3. **Component is one author, recent.** v2.0.2; backward-compatible with v1 according to the readme. Worth pinning to an exact version and reading the source before merging.
4. **8 MB board limit.** Real decision the user needs to make: do we ship Option A (reduced retention everywhere) or Option B (recommend 16 MB hardware, keep A as fallback)?
5. **Panel-index stability.** If a panel is replaced (new barcode, same physical slot), how do we handle history? Probably: leave the old slot's history in place (read-only), assign a new slot for the new panel. UI surfaces both with a "replaced on YYYY-MM-DD" pill.

---

## Implementation phases

1. **Add the dependency + partition.** New `tsdb` partition in `boards/*.yaml` partition tables, add `idf.py` dependency, verify it links and `tsdb_init` succeeds. No data written yet.
2. **System rollup writer.** Add the 5-min timer, the writer task, and `system.tsdb` schema. Add `/api/history/power?range=day|week`. Verify on dashboard.
3. **Daily rollup.** Cron at 00:01 to aggregate into `daily.tsdb`. Add `/api/history/energy`.
4. **Per-panel.** `panels.tsdb` + barcode→index mapping + `/api/history/panel`. Tools-view button to view a panel's last week.
5. **Diagnostics.** Surface TSDB stats in the diagnostics view: file sizes, write latency p95, last write age, slot map.
6. **Retire volatile history.** Once stable for ~30 days, remove the in-RAM history and point legacy endpoints at TSDB.

Each phase is its own branch off `main`, mergeable independently.

---

## Open question for the user

**Option A** (8 MB board, ~14 months system data, ~80 days panel data) or **Option B** (recommend 16 MB board for new installs, keep A as the small-flash variant)?

Both are easy code-wise; the answer just changes default partition sizes shipped in `boards/`.
