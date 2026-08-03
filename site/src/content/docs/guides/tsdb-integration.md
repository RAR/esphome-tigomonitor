---
title: Saving History to Flash
description: Turn on long-term history so your charts survive a reboot — what to add, and how much it stores.
---

By default the device shows you what's happening *now*. Turn this on and it also
keeps a **permanent record on the device itself** — about two years of system
history and four months per panel — so the History charts still work after a
power cut, a reboot, or a firmware update.

It's off by default because it needs a couple of extra lines in your setup file
and a change to how the device's storage is divided up. The
[Config Builder](/esphome-tigomonitor/config-builder/) can do that part for you.

**To turn it on:** copy the block under
[Required configuration](#required-configuration) into your setup file and
reflash. That's the whole job — everything after that section is background for
the curious and for people working on the firmware.

Under the hood this is [`zakery292/esp_tsdb`](https://github.com/zakery292/esp_tsdb),
a small time-series database that writes to a dedicated area of the ESP32's flash
memory.

:::note[Two things to expect]
**A reading every 30 minutes,** adjustable. Not every few seconds — flash memory
wears out if you write to it constantly. Set `history_interval` (in minutes) to
trade detail against flash life; see [Choosing an
interval](#choosing-an-interval). Your energy totals are unaffected either way,
only how finely the charts are drawn.

**About four months of per-panel detail** at the default. Each panel keeps its
most recent 5,404 readings and then starts overwriting the oldest, so the span
shrinks proportionally if you ask for finer resolution. System-wide history keeps
longer — roughly 2 years.

**Up to 48 panels.** Beyond that, extra panels still show live readings but don't
get their own saved history.
:::

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
  board: m5stack-atoms3   # board-specific — set this to YOUR board
  variant: esp32s3
  framework:
    type: esp-idf
    components:
      # Upstream 2.1.0+ has everything this project needs (handle-based
      # multi-instance API, wrapped-ring query fix). ESP32-P4 only: use the
      # RAR/esp_tsdb fork's `tigomonitor` branch instead (source/ref pin) —
      # 2.1.0's manifest doesn't list the esp32p4 target yet.
      - zakery292/esp_tsdb^2.1.0
      - joltwallet/littlefs^1.16
    sdkconfig_options:
      CONFIG_PARTITION_TABLE_CUSTOM: "y"
      CONFIG_PARTITION_TABLE_FILENAME: "boards/partitions/tigo-8mb.csv"
      CONFIG_LITTLEFS_FOR_IDF_3_2: "n"
```

### Choosing an interval

`history_interval` accepts 5 to 1440 minutes and defaults to 30. Everything
scales linearly with it, in both directions:

| Interval | Per-panel history | System history | Device time spent committing | Chart resolution |
|----------|-------------------|----------------|------------------------------|------------------|
| 10 min | ~5 weeks | ~7 months | ~1.8% | finest |
| **30 min (default)** | **~4 months** | **~2 years** | **~0.6%** | balanced |
| 60 min | ~7.5 months | ~3.7 years | ~0.3% | coarse |

The low end costs more than the resolution alone suggests. Writing a snapshot
takes about 10 seconds — four databases, each needing a filesystem sync that
LittleFS implements as a full journal commit — and the device holds the history
lock for all of it, so chart requests arriving mid-commit have to wait it out
(they allow 30 s before giving up). Flash wear rises by the same factor. Values
under 15 minutes log a build-time warning.

That cost is per commit rather than per byte: a 1 MB database and a 192 KB one
were measured taking the same time, whether appending or overwriting. So it
scales with how *often* you write, not how much you store — which is exactly why
the interval is the lever that matters.

Changing the interval later is safe: `period_e_*` stores energy since the
previous snapshot rather than a running total, so lifetime figures stay correct
across a change and existing history is not invalidated.

> **Board note:** the `board:` value above (`m5stack-atoms3`) is an example. The reference rig for this project is the **AtomS3R** — set `board:` to whatever board you actually run so you don't flash the wrong target.

The TSDB code is conditionally compiled — without those two dependencies on the include path, `tigo_history.h` short-circuits and the History / TSDB-stats endpoints don't exist. You can run the rest of the component without TSDB; you just lose persistent history.

---

# Reference

Internals for firmware developers: what gets persisted, how it's sized, the write/query paths, and implementation notes. You don't need any of this to enable history.

## Status and cadence

Phases 1–3 shipped. Per-snapshot system rollups + per-panel power are persisted at a user-settable cadence (`history_interval`, default 30 min); up to 48 panels supported across three lazy-opened panel DBs. Daily-rollup phase (Phase 4) and the volatile-history retirement (Phase 6) are tracked separately and not on the critical path.

**Where the interval lives.** `kSnapshotIntervalMin` in `tigo_history.h` is the single source of truth — `tigo_monitor.cpp` arms its timer from it, and `/api/history/*` reports it as `interval_min` so the UI labels charts without hardcoding a number. Change it in one place.

**Why it moved around.** The interval was originally 5 min. Every flash write briefly disables the CPU instruction cache on both cores, and at 5-min cadence those windows collided often enough with WiFi/BLE-coex radio ISRs to crash the device. Coarsening to 30 min (and core-pinning the writer) cut the crash rate ~40×, and 60 min halved the exposure again — but both were probability reductions that left the fault in place, with mean time-to-failure still only 13.5–42 h.

`execute_from_psram` addressed the mechanism instead. With instructions and rodata relocated to PSRAM, ESP-IDF satisfies `SPI_FLASH_CACHE_NO_DISABLE` and compiles the cache-disable out entirely, so a flash write no longer stalls the other core at all — see [the crash investigation](https://github.com/RAR/esphome-tigomonitor/blob/main/docs/tsdb-flash-crash-issue.md). A build with that flag ran 142 h with no fault and no movement in any memory watermark, against a 42 h best beforehand.

Cadence is therefore a resolution/retention decision again rather than a stability lever, and 10 min is deliberately 6× the write rate of that 142 h run — it tests the fix rather than trusting it. `period_e_*` energy deltas are interval-agnostic, so totals stay correct across any of these changes; only time-resolution and retention move.

## What gets persisted

Two logical schemas, three on-disk DBs (panel DB is striped because esp_tsdb caps at 16 base params per file).

### `system.tsdb` — system + per-inverter rollups (14 params)

| # | Name | Unit | Scale |
|---|------|------|-------|
| 0 | `total_p` | W | ×1 |
| 1 | `total_e` | kWh | ×100 (period delta — energy produced since the previous snapshot) |
| 2–9 | `inv1_p` … `inv4_e` | W or kWh ×100 | per-inverter power and per-period energy delta |
| 10 | `temp_avg` | °C | ×1 |
| 11 | `freq` | dHz | ×10 (currently 0 — wired but not extracted from telemetry) |
| 12 | `frames_lost` | count | ×1 |
| 13 | `wifi_rssi` | dBm | ×1 |

### `panels{0,1,2}.tsdb` — per-panel power (16 params each)

Each DB covers 16 panel slots. Up to 48 panels total. DBs are opened lazily — `panels1.tsdb` doesn't exist on flash until a 17th slot is assigned.

Slots are stable: a barcode is mapped to a slot on first sight and that mapping persists in `/tsdb/panel_map.json` (small JSON file written via fopen("wb")+fclose for crash safety). Replaced panels keep their slot history; new barcodes get the next free slot. Removed panels are not garbage-collected — their history stays in place.

If 48 slots fill up, additional panels are skipped silently (with a `(W)` log). Bumping `kNumPanelDbs` in `tigo_history.h` raises the cap, but requires bigger flash sizing per below.

---

## Sizing

`tigo-8mb.csv` partition layout reserves 3 MB for the LittleFS partition (label `tsdb`):

```text
otadata    8 KB
phy_init   4 KB
app0       1.75 MB   (OTA slot A)
app1       1.75 MB   (OTA slot B)
nvs        448 KB
tsdb       3 MB      (LittleFS — system.tsdb + 3× panels<N>.tsdb)
```

Per-DB allocations in `tigo_history.cpp` (record count = `(file_bytes − 2048) / (4 + params×2)`):

| DB | File size | Records | At the 30-min default | Buffer pool |
|----|-----------|---------|-------------------|-------------|
| `system.tsdb` | 1 MB | ~32,700 records | ~1.9 yr | 10 KB (PSRAM) |
| `panels{0,1,2}.tsdb` | 192 KB each | ~5,400 records | ~112 days each | 6 KB each (PSRAM) |

The three panel DBs total 576 KB; with the 1 MB system DB that's ~1.6 MB of the 3 MB partition (~52% used). The rest is deliberate headroom — LittleFS needs free blocks for metadata, copy-on-write scratch, and garbage collection. (An earlier 2 MB + 3×256 KB layout ran the partition ~98% full, which starved LittleFS and wiped history on every reboot — see commit `00366d7`.)

Buffer pools live in PSRAM (`TSDB_ALLOC_PSRAM`) so they don't pressure internal heap; the AtomS3R reference rig reclaimed ~28 KB internal heap by moving them out.

---

## Write path

A dedicated FreeRTOS task (`tsdb_writer`, priority 1, 8 KB stack, **pinned to core 1** via `xTaskCreatePinnedToCore`) drains a queue of encoded snapshots. Pinning matters: it puts the writer on the same core as the ESPHome main loop and the UART read, so the writer's flash op can never run *concurrently* with the UART ring-buffer read on the other core — one of the flash-vs-cache crash victims (see the cadence note at the top). Snapshots are produced by a `kSnapshotIntervalMin` `set_interval` timer on the main app task:

1. Take the state lock briefly to gather aggregates (system, per-inverter, per-panel power).
2. Encode floats to int16 with the appropriate scale.
3. `xQueueSend` non-blocking — if the queue is full (4-deep), drop the sample with a log warning. Even at the 5-min floor the queue should never be more than 1 deep in steady state.

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
| `/api/history/power?range=day` | `system.tsdb` | `history_interval` | ~48 |
| `/api/history/power?range=week` | `system.tsdb` | `history_interval` | ~336 |
| `/api/history/power?range=month` | `system.tsdb` | `history_interval` | ~1,440 |
| `/api/history/power?range=year` | `system.tsdb` | `history_interval` | ~17,500 (fits at the default; at 10 min the DB caps out around 227 days, so a year query returns ~7.5 months) |
| `/api/history/panel?slot=N&range=…` | `panels{slot/16}.tsdb` | `history_interval` | one column read (~112 days available at the default) |
| `/api/panels` | `panel_map.json` | — | full slot map |
| `/api/tsdb/stats` | live handles | — | per-DB record counts, oldest/newest, evictions, file sizes |

The Diagnostics view consumes `/api/tsdb/stats` to render the database table (records / max records / writes / evictions / size / range).

Two things to know about that endpoint:

- **File sizes need `CONFIG_VFS_SUPPORT_DIR: "y"`.** `esp_tsdb` derives `storage_bytes` from `stat()`, which IDF compiles out by default — without the flag every size reads 0. The shipped boards set it.
- **A database can report `available: false`.** `tsdb_get_stats_h()` takes the per-DB mutex with a 5-second timeout, and the writer task holds that mutex across `tsdb_sync_h()`'s `fclose`/`fopen`. A stats poll landing on a flush logs `tsdb_get_stats_h: lock timeout` and returns `ESP_ERR_TIMEOUT`. The row is still emitted, with `available: false`, an `error` string, and `null` numeric fields; the Diagnostics table renders those as `—` rather than dropping the database from the list. A database that is simply not open yet (lazily-created panel DBs) gets no row at all.

---

## Implementation notes

### Persistence bug fix (upstream PR `zakery292/esp_tsdb#1`)

Empirical: history was being wiped on every reboot even though `tsdb_write_h` does fflush + fsync after every record. Root cause was in `tsdb_open`'s file-existence detection: it called `stat(filepath)` and went down the create-new path on failure — `stat()` failed for files that `fopen("rb")` immediately read bytes back from, so every boot took the create-new path and `fopen("w+b")` truncated the existing data.

> **Corrected 2026-07-24.** This was originally attributed to a quirk of joltwallet's `esp_littlefs` (1.21.1). It was almost certainly not the filesystem: ESP-IDF leaves **`CONFIG_VFS_SUPPORT_DIR` off by default**, which compiles the VFS directory syscalls — `stat`, `unlink`, `rename`, `opendir`, `mkdir` — out of the build entirely. They then fail at runtime for *every* path, while `fopen`/`fread`/`fwrite` (under `VFS_SUPPORT_IO`) keep working. That matches the observed "`stat()` says ENOENT for a file I can read" behaviour exactly, and explains why the fopen-based probe fixed it. The boards now set `CONFIG_VFS_SUPPORT_DIR: "y"`, which also restores `esp_tsdb`'s size reporting and its corruption-recovery `unlink()` paths.

Fix: probe existence by trying `fopen(path, "r+b")` first; fall through to `fopen("w+b")` only if that fails. The slot map (`panel_map.json`) was unaffected because it uses an `open(wb)+write+close` cycle every save, which never hits the bad code path.

### Multi-DB striping

`esp_tsdb` caps at 16 base params per DB. To cover 48 panels, three panel DBs are opened lazily as slots fill. The fork's `tsdb_t *` handle-based API (also from PR #1) makes this straightforward — each DB has its own handle, mutex, and buffer pool.

### PSRAM placement

Buffer pools default to internal RAM in upstream esp_tsdb. We override via `cfg.alloc_strategy = TSDB_ALLOC_PSRAM` so the ~28 KB total stays out of the constrained internal heap on the AtomS3R.

---

## Out of scope (for now)

- **Full per-panel V/I/T at 5-min** — too many series for the int16-only schema; the live UI shows V/I/T already.
- **Real-time streaming** — UI polls.
- **Daily/monthly pre-aggregation** (`daily.tsdb` from the original plan) — month/year queries currently scan the system DB directly. Fine at the 30-min default, where a year fits. At shorter `history_interval` values `range=year` becomes capacity-bound (~227 days at 10 min); pre-aggregation or larger flash would be the fix if a true multi-year view is wanted at fine resolution.
- **Per-string rollups in `system.tsdb`** — there's headroom in the param layout for future strings, but not wired.

---

## Where to look in the source

- `components/tigo_monitor/tigo_history.{h,cpp}` — schema, encoders, writer task, slot map.
- `components/tigo_monitor/tigo_monitor.cpp` `snapshot_to_history_()` — gathers and enqueues snapshots.
- `components/tigo_server/tigo_web_server.cpp` `api_history_*_handler`, `api_panels_handler`, `api_tsdb_stats_handler` — query/stats endpoints.
- `components/tigo_server/web/app.html` — History view (charts) + Diagnostics view (TSDB stats table).

---

**See also:** [Configuration](/esphome-tigomonitor/guides/configuration/) · [Web Server & API](/esphome-tigomonitor/guides/web-server/) · [← Back to README](https://github.com/RAR/esphome-tigomonitor)
