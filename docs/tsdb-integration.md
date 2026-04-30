# TSDB Integration

On-flash time-series storage for Tigo Monitor, backed by [`zakery292/esp_tsdb`](https://github.com/zakery292/esp_tsdb). Survives reboots and OTA updates; queryable from the SPA's History view and from the JSON API.

> **Status:** Phases 1–3 shipped. Per-snapshot system rollups + per-panel power are persisted at 5-min cadence; up to 48 panels supported across three lazy-opened panel DBs. Daily-rollup phase (Phase 4) and the volatile-history retirement (Phase 6) are tracked separately and not on the critical path.

---

## What gets persisted

Two logical schemas, three on-disk DBs (panel DB is striped because esp_tsdb caps at 16 base params per file).

### `system.tsdb` — system + per-inverter rollups (5-min cadence, 14 params)

| # | Name | Unit | Scale |
|---|------|------|-------|
| 0 | `total_p` | W | ×1 |
| 1 | `total_e` | kWh | ×100 (period delta — energy produced in the 5-min window) |
| 2–9 | `inv1_p` … `inv4_e` | W or kWh ×100 | per-inverter power and 5-min energy delta |
| 10 | `temp_avg` | °C | ×1 |
| 11 | `freq` | dHz | ×10 (currently 0 — wired but not extracted from telemetry) |
| 12 | `frames_lost` | count | ×1 |
| 13 | `wifi_rssi` | dBm | ×1 |

### `panels{0,1,2}.tsdb` — per-panel power (5-min cadence, 16 params each)

Each DB covers 16 panel slots. Up to 48 panels total. DBs are opened lazily — `panels1.tsdb` doesn't exist on flash until a 17th slot is assigned.

Slots are stable: a barcode is mapped to a slot on first sight and that mapping persists in `/tsdb/panel_map.json` (small JSON file written via fopen("wb")+fclose for crash safety). Replaced panels keep their slot history; new barcodes get the next free slot. Removed panels are not garbage-collected — their history stays in place.

If 48 slots fill up, additional panels are skipped silently (with a `(W)` log). Bumping `kNumPanelDbs` in `tigo_history.h` raises the cap, but requires bigger flash sizing per below.

---

## Sizing

`tigo-8mb.csv` partition layout reserves 3 MB for the LittleFS partition (label `tsdb`):

```
otadata    8 KB
phy_init   4 KB
app0       1.75 MB   (OTA slot A)
app1       1.75 MB   (OTA slot B)
nvs        448 KB
tsdb       3 MB      (LittleFS — system.tsdb + 3× panels<N>.tsdb)
```

Per-DB allocations in `tigo_history.cpp`:

| DB | File size | Records | At 5-min cadence | Buffer pool |
|----|-----------|---------|------------------|-------------|
| `system.tsdb` | 2 MB | ~65k records | ~227 days at full cadence | 10 KB (PSRAM) |
| `panels{0,1,2}.tsdb` | 256 KB each | ~7,200 records | ~25 days each | 6 KB each (PSRAM) |

Buffer pools live in PSRAM (`TSDB_ALLOC_PSRAM`) so they don't pressure internal heap; the AtomS3R reference rig reclaimed ~28 KB internal heap by moving them out.

---

## Write path

A dedicated FreeRTOS task (`tsdb_writer`, priority 1, 8 KB stack) drains a queue of encoded snapshots. Snapshots are produced by a 5-min `set_interval` timer on the main app task:

1. Take the state lock briefly to gather aggregates (system, per-inverter, per-panel power).
2. Encode floats to int16 with the appropriate scale.
3. `xQueueSend` non-blocking — if the queue is full (4-deep), drop the sample with a log warning. With 5-min cadence the queue should never be more than 1 deep in steady state.

The writer task pops snapshots and calls `tsdb_write_h(system_db_, …)` followed by `tsdb_write_h(panel_db_[i], …)` for every open panel DB. Each `tsdb_write_h` does fflush + fsync internally.

On `App.safe_reboot()`, `TigoMonitorComponent::on_shutdown()`:

1. Drains the writer queue (best effort, 800 ms cap).
2. `tsdb_close_h`s every open handle (releases buffer pool, fcloses underlying FILE*).
3. `esp_vfs_littlefs_unregister("tsdb")` — final journal commit before the partition is unmounted.

This pairing matters: without the explicit close, the next `tsdb_open` in some scenarios would re-create the file from scratch (see "Implementation notes" below).

---

## Query path

The SPA's History view and the JSON API both pull from `/api/history/power` (system) and `/api/history/panel?slot=N` (single panel). Queries run on the http_server task using `tsdb_query_*_h` and stream results into a `PSRAMString`.

| Endpoint | Source | Resolution | Typical points |
|----------|--------|------------|----------------|
| `/api/history/power?range=day` | `system.tsdb` | 5-min | ~288 |
| `/api/history/power?range=week` | `system.tsdb` | 5-min | ~2,000 |
| `/api/history/power?range=month` | `system.tsdb` | 5-min | ~8,640 (capped at oldest record on disk) |
| `/api/history/power?range=year` | `system.tsdb` | 5-min | (covers what's on disk, ≤ ~227 days) |
| `/api/history/panel?slot=N&range=…` | `panels{slot/16}.tsdb` | 5-min | one column read |
| `/api/panels` | `panel_map.json` | — | full slot map |
| `/api/tsdb/stats` | live handles | — | per-DB record counts, oldest/newest, evictions, file sizes |

The Diagnostics view consumes `/api/tsdb/stats` to render the database table (records / max records / writes / evictions / size / range).

---

## Required configuration

Add the `esp_tsdb` and `joltwallet/littlefs` dependencies plus a `tsdb` partition.

`tigo-8mb.csv` (lives under `boards/partitions/`):

```csv
otadata,   data, ota,      ,       0x2000,
phy_init,  data, phy,      ,       0x1000,
app0,      app,  ota_0,    ,       0x1C0000,
app1,      app,  ota_1,    ,       0x1C0000,
nvs,       data, nvs,      ,       0x70000,
tsdb,      data, littlefs, ,       0x300000,
```

YAML:

```yaml
esp32:
  board: m5stack-atoms3
  variant: esp32s3
  framework:
    type: esp-idf
    components:
      # Until zakery292/esp_tsdb#1 lands and tags a release, the path: form
      # below points at the local handle-based-API fork. Once the upstream
      # release ships, swap this for a registry pin.
      - name: zakery292/esp_tsdb
        path: /path/to/esp_tsdb
      - joltwallet/littlefs^1.16
    sdkconfig_options:
      CONFIG_PARTITION_TABLE_CUSTOM: "y"
      CONFIG_PARTITION_TABLE_FILENAME: "boards/partitions/tigo-8mb.csv"
      CONFIG_LITTLEFS_FOR_IDF_3_2: "n"
```

The TSDB code is conditionally compiled — without those two dependencies on the include path, `tigo_history.h` short-circuits and the History / TSDB-stats endpoints don't exist. You can run the rest of the component without TSDB; you just lose persistent history.

---

## Implementation notes

### Persistence bug fix (upstream PR `zakery292/esp_tsdb#1`)

Empirical: history was being wiped on every reboot even though `tsdb_write_h` does fflush + fsync after every record. Root cause was in `tsdb_open`'s file-existence detection: it called `stat(filepath)` and went down the create-new path on failure. On joltwallet's `esp_littlefs` (1.21.1), `stat()` returns `ENOENT` for files that `fopen("rb")` immediately reads bytes back from — so every boot took the create-new path and `fopen("w+b")` truncated the existing data.

Fix: probe existence by trying `fopen(path, "r+b")` first; fall through to `fopen("w+b")` only if that fails. The slot map (`panel_map.json`) was unaffected because it uses an `open(wb)+write+close` cycle every save, which never hits the bad code path.

### Multi-DB striping

`esp_tsdb` caps at 16 base params per DB. To cover 48 panels, three panel DBs are opened lazily as slots fill. The fork's `tsdb_t *` handle-based API (also from PR #1) makes this straightforward — each DB has its own handle, mutex, and buffer pool.

### PSRAM placement

Buffer pools default to internal RAM in upstream esp_tsdb. We override via `cfg.alloc_strategy = TSDB_ALLOC_PSRAM` so the ~28 KB total stays out of the constrained internal heap on the AtomS3R.

---

## Out of scope (for now)

- **Full per-panel V/I/T at 5-min** — too many series for the int16-only schema; the live UI shows V/I/T already.
- **Real-time streaming** — UI polls.
- **Daily/monthly pre-aggregation** (`daily.tsdb` from the original plan) — month/year queries currently scan the system DB directly. Fine for ~227 days of 5-min data; add aggregation if larger flash is fitted.
- **Per-string rollups in `system.tsdb`** — there's headroom in the param layout for future strings, but not wired.

---

## Where to look in the source

- `components/tigo_monitor/tigo_history.{h,cpp}` — schema, encoders, writer task, slot map.
- `components/tigo_monitor/tigo_monitor.cpp` `snapshot_to_history_()` — gathers and enqueues snapshots.
- `components/tigo_server/tigo_web_server.cpp` `api_history_*_handler`, `api_panels_handler`, `api_tsdb_stats_handler` — query/stats endpoints.
- `components/tigo_server/web/app.html` — History view (charts) + Diagnostics view (TSDB stats table).
