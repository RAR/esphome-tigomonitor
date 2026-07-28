# ESP32-S3 faults inside the LittleFS writer, despite a mutex that should make it impossible

## One-line statement

On an ESP32-S3, a FreeRTOS task writing to LittleFS faults (`Fault - Unknown`,
core 1) *while holding* the only mutex that guards flash access — and the fault
rate is strongly driven by how often a **different** task performs flash reads,
even though those reads take the same mutex and therefore cannot be executing
concurrently.

Either that mutual exclusion is not real, or the fault does not require
concurrency at all. We do not know which, and that is the question.

## Hardware / build

| | |
|---|---|
| Board | M5Stack AtomS3R, ESP32-S3 rev0.2, 2 cores, 8 MB PSRAM |
| Framework | ESP-IDF 5.5.5 via ESPHome 2026.7.2 |
| Filesystem | LittleFS (joltwallet port) on a 3 MB `tsdb` partition |
| TSDB | `zakery292/esp_tsdb` 2.3.0 (registry, not a fork) |
| Radios | WiFi up; BLE controller up (`cca_source: ble`), scanning stopped at boot |
| Relevant sdkconfig | `CONFIG_SPI_FLASH_AUTO_SUSPEND` **off**; `CONFIG_UART_ISR_IN_IRAM=y`; `CONFIG_ESP_WIFI_IRAM_OPT=y`; `CONFIG_ESP_WIFI_RX_IRAM_OPT=y`; `power_save_mode: none` |

## The tasks involved

- **Writer task** — `TigoHistory::writer_task_loop_`, `xTaskCreatePinnedToCore`
  on **core 1**. Wakes every 30 min, writes one record to each of four TSDB
  files (`system`, `panels0/1/2`), then rewrites an 8-byte marker file to force
  a LittleFS journal commit.
- **HTTP task** — ESP-IDF `httpd`, left at `tskNO_AFFINITY` (the default;
  `core_id` is never set), so it floats — typically core 0.
- **ESPHome loop task** — assigns panel slots, writes `panel_map.json`. Rare.

## The lock

`TigoHistory::FlashLock` is an RAII guard over `fs_mutex_`, a FreeRTOS
**recursive** mutex (`xSemaphoreCreateRecursiveMutex`), created once in `init()`.

```cpp
TigoHistory::FlashLock::FlashLock(TigoHistory *hist, uint32_t timeout_ms)
    : mutex_(hist != nullptr ? hist->fs_mutex_ : nullptr) {
  TickType_t wait = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  held_ = (mutex_ == nullptr) || (xSemaphoreTakeRecursive(mutex_, wait) == pdTRUE);
}
TigoHistory::FlashLock::~FlashLock() {
  if (mutex_ != nullptr && held_) xSemaphoreGiveRecursive(mutex_);
}
```

Every known flash toucher takes it:

- writer: `FlashLock lock(this, 0)` (portMAX_DELAY) held across all four
  `tsdb_write_h` calls **and** the journal commit (`tigo_history.cpp:611`)
- history queries (`iterate_power`/`iterate_panel`): 15 s timeout
- slot assignment: recursive, across `open_panel_db_` + `save_slot_map_`
- `/api/tsdb/stats` (before the fix below): took it before `esp_littlefs_info`

**Note `held_ = (mutex_ == nullptr) || ...` — a null mutex reports success.
That is intended for the pre-init single-threaded window. Whether it can be
null later is one thing worth checking.**

## Crash signature

Always the writer task, always core 1, always `Fault - Unknown`, always inside
LittleFS beneath `tsdb_write_h`. Three elf-matched decodes from 2026-07-28:

```
11:18  PC 0x42092029  lfs_file_flushedwrite  lfs.c:3598
12:17  PC 0x42092289  lfs_file_flush         lfs.c:3380
13:29  (same class, at the commit instant under load)

common tail:
  lfs_file_sync_ ← lfs_file_sync ← esp_littlefs_file_sync ← vfs_littlefs_fsync
  ← esp_vfs_fsync ← tsdb_write_block (tsdb_write.c:57) ← tsdb_write_h
  ← TigoHistory::writer_task_loop_ (tigo_history.cpp:625)
```

`tigo_history.cpp:625` is the panel-DB write inside the batch loop — i.e.
**inside** the `FlashLock` scope opened at line 611.

An earlier decode (2026-07-23) caught the fault inside IDF's own cross-core
stall coordination: `vPortClearInterruptMaskFromISR ← xTaskResumeAll ←
spi_flash_disable_interrupts_caches_and_other_cpu` during an `esp_flash_read`.

## The experiments (all on one rig, same binary, 4 concurrent curl loops each)

| Load | Flash access on that path | Result |
|---|---|---|
| `/api/tsdb/stats` reading flash directly | `esp_littlefs_info` + 4× `stat()` | **crash in ~11 s** |
| `/api/status` | none | survived 190 s at a *higher* request rate |
| `/api/tsdb/stats` with 60 s response cache, idle | ~1 flash read/min | survived 4 min |
| same, across a writer commit | ~1 flash read/min | **crashed at the commit instant, to the second** |
| no HTTP load at all | writer only | crashes on its own, MTTF ~14 h (samples: 13.5, 42.0, 14.0 h) |

Control: a writer commit on an idle device completes cleanly and repeatedly.

## What has been ruled out (with evidence — do not re-propose)

1. **esp_tsdb being a fork** — recurs on the registry build.
2. ~~**`execute_from_psram`** — recurs without it.~~ **RETRACTED 2026-07-28.** That
   experiment only established it was not the *cause*. It was never enabled and
   measured as a *cure*, and it is the only candidate found so far that removes
   the mechanism rather than shrinking the window:

   ```c
   // IDF 5.5.5 spi_flash_os_func_app.c:29
   #define SPI_FLASH_CACHE_NO_DISABLE (CONFIG_SPI_FLASH_AUTO_SUSPEND \
     || (CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA) \
     || CONFIG_APP_BUILD_TYPE_RAM)

   #if !SPI_FLASH_CACHE_NO_DISABLE      // <- cache_disable is not compiled at all
   IRAM_ATTR static void cache_disable(void* arg) {
       spi_flash_disable_interrupts_caches_and_other_cpu();
   }
   #endif
   ```

   `execute_from_psram` sets that middle pair. Verified in the built
   `sdkconfig.h`: `CONFIG_SPIRAM_FETCH_INSTRUCTIONS 1`, `CONFIG_SPIRAM_RODATA 1`,
   and `CONFIG_SPI_FLASH_SHARE_SPI1_BUS` undefined so no earlier branch wins —
   `spi1_start` takes `_lock_acquire(&s_spi1_flash_mutex)` instead. That removes
   frames BT2/BT3/BT4 of the 14:25 crash backtrace by construction.

   "Harmful on S3" appeared in the working notes with no supporting evidence;
   ESPHome's own `tests/components/esp32/test.esp32-s3-idf.yaml:6` sets this flag
   on an S3, and this repo's P4 notes describe it fixing the same failure class
   (a PSRAM-resident stack unreachable during the cache-disable window, #31).
   Enabled on the rig 2026-07-28; **soak result not yet in.**
3. **OOM / heap exhaustion** — `min_free_heap` flat for hours across crashes.
4. **Caller-side locking of esp_tsdb** — it has its own per-handle recursive mutex.
5. **`CONFIG_SPI_FLASH_AUTO_SUSPEND`** — the textbook fix; causes a <20 s
   early-boot watchdog reset on this hardware, and Espressif cautions against
   it under BT/WiFi coex.
6. **OTA collision** — crashes happen with no OTA in flight.
7. **Per-victim IRAM whack-a-mole** — fixing one victim relocates the fault
   (WiFi ISR → UART ringbuf → the writer's own flush).
8. **A hidden second flash writer** — audited: no `globals:`, no
   `total_daily_energy`/`integration`/`utility_meter`, no periodic NVS flush.
   The only writers are the TSDB batch, the journal marker, and `panel_map.json`.
9. **Shared `FILE*` corruption from the stats path** — `tsdb_get_stats_h`
   (`tsdb_core.c:901`) reads only the in-RAM header plus a path-based `stat()`;
   it never touches `db->file`.

## What was just changed (and what it does not explain)

`/api/tsdb/stats` now serves from a RAM snapshot that the writer refreshes
inside its own flash batch, so the HTTP path performs no flash access at all.
That removes the induced crash. It does **not** explain why the lock failed to
prevent it, and it does not touch the ~14 h background crash.

## The questions

1. **Why did `FlashLock` not prevent the collision?** Both sides take the same
   recursive mutex; the writer holds it with `portMAX_DELAY` across the whole
   batch. Under what concrete mechanism can an HTTP-task flash read still
   affect the writer's flash write? Candidates worth testing include: the
   mutex not being the object we think it is at that moment; priority
   inversion or `httpd` worker threads bypassing it; the fault not requiring
   concurrency at all (i.e. the HTTP load matters via some other coupling —
   interrupt load, cache pressure, PSRAM bus contention, TCP/lwIP work on the
   other core during the writer's `esp_ipc_isr_stall_other_cpu` window).
2. **Is the background ~14 h crash the same bug or a different one?** Same
   backtrace, but no HTTP load is required.
3. **What is the highest-leverage fix**, given the ruled-out list? Under
   consideration: snapshot interval 30 → 60 min; a single `fsync` per file per
   write (upstream esp_tsdb change, halves sync count and improves crash
   atomicity); journal commit every Nth snapshot (trades against the data-loss
   it exists to prevent); BT controller IRAM; quiescing the radios around the
   flush; pinning `httpd` to core 1.
4. **Is this an upstream ESP-IDF defect on this silicon**, and if so is there a
   known erratum, issue, or workaround for ESP32-S3 rev0.2?

## Ground rules for anyone analysing this

- Read the actual code before theorising. Two conclusions today were confidently
  argued from code reading and then falsified on hardware within the hour.
- Any claim about flash access must name the file and line that performs it.
- Distinguish "reduces the probability" from "removes the mechanism". Several
  fixes already shipped were mitigations sold as cures.
- The device is reachable at `http://tigomonitor.local`; crashes are recoverable
  and lossless, so on-hardware experiments are cheap and are the tiebreaker.
