#pragma once

// On-flash time-series history via esp_tsdb (zakery292/esp_tsdb).
//
// Compiled in only when both esp_tsdb.h and esp_littlefs.h are reachable on
// the include path — controlled by the YAML `framework: components:` list.
// Builds without those deps (e.g. dev boards on the default partition table)
// silently skip this code, leaving runtime behaviour unchanged.

#ifdef USE_ESP_IDF
#if __has_include("esp_tsdb.h") && __has_include("esp_littlefs.h")
#define TIGO_TSDB_AVAILABLE 1

#include "esp_tsdb.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace esphome {
namespace tigo_monitor {

// How often a snapshot is written to flash — user-settable as `history_interval`
// in YAML. This is now a resolution/retention decision, but it spent most of
// this project's life as a crash-exposure dial, which is worth knowing before
// you turn it down.
//
// Every flash write forces the ESP-IDF cross-core cache-disable/CPU-stall
// (spi_flash_disable_interrupts_caches_and_other_cpu), which intermittently
// faulted under WiFi/BLE coex — the "Fault - Unknown" class documented in
// docs/tsdb-flash-crash-issue.md. Exposure scaled with how often this fired, so
// the cadence kept being coarsened to buy stability: 5 min -> 30 (2026-07-23)
// -> 60 (2026-07-28). execute_from_psram (see tigo_monitor/__init__.py) removes
// the mechanism rather than shrinking its window — with instructions and rodata
// in PSRAM, IDF compiles the cache-disable out entirely — which is what makes
// this safe to expose as a knob at all.
//
// Two costs still scale with it, and neither was fixed by that flag:
//
//   * A commit holds the flash lock ~21 s (sys ~5 s + panels ~16 s, measured
//     with the panel rings full). Readers wait kFsLockReaderWaitMs, so a
//     history request landing in a commit fails. Shorter interval, worse odds.
//   * Each commit rewrites every file to EOF, because esp_tsdb rewrites the
//     header at offset 0 and littlefs byte-copies from there. That is ~841 KB
//     per commit into a 3 MB partition, so flash wear scales linearly with
//     1/interval.
//
// Both have the same real fix — a deferred-sync write path in esp_tsdb — not a
// number here.
//
// Retention scales with the interval too, since each DB holds a fixed record
// count: the 5404-record panel rings span ~112 days at 30 min, ~37.5 at 10;
// system.tsdb spans ~2.2 yr at 30 min. period_e_kwh is energy-since-last-
// snapshot, so it is interval-agnostic and lifetime totals stay correct across
// a change — you can retune this without invalidating stored history.
//
// Default only — the effective value is `history_interval` in YAML, held on
// TigoMonitorComponent and readable via get_snapshot_interval_min(). 30 min is
// a deliberate middle: twice the detail of the old hourly cadence at half the
// flash wear of 10 min, which matters because we ship to unknown flash parts.
static constexpr uint32_t kDefaultSnapshotIntervalMin = 30;

// Bounds enforced by the Python schema; restated here because the retention and
// duty-cycle maths below depend on them. A commit currently holds the flash
// ~21 s (see tigo_history.cpp write path), so 5 min is already ~7% duty and
// anything shorter approaches a writer that is never idle.
static constexpr uint32_t kMinSnapshotIntervalMin = 5;
static constexpr uint32_t kMaxSnapshotIntervalMin = 1440;

// esp_tsdb caps base params per DB at 16. We cover up to 48 panels by
// striping across three DB instances (panels0.tsdb, panels1.tsdb, panels2.tsdb).
// Sized for the user's 36->40 panel rig with 8 slots of growth headroom; bump
// kNumPanelDbs to 4 if a 64-panel install ever shows up (will require larger
// flash — see kPanelFileBytes math in tigo_history.cpp).
static constexpr size_t kPanelsPerDb = 16;
static constexpr size_t kNumPanelDbs = 3;
static constexpr size_t kMaxPanelSlots = kPanelsPerDb * kNumPanelDbs;

// One raw snapshot. The history layer encodes these to int16_t and writes.
// Caller fills this under the TigoMonitorComponent state lock.
struct SystemSnapshot {
  uint32_t timestamp;          // unix epoch seconds (0 = invalid, will be dropped)
  float total_p_w;             // system power in watts
  float period_e_kwh;          // energy produced since the last snapshot (kWh)
  float inv_p_w[4];            // per-inverter power
  float inv_e_kwh[4];          // per-inverter energy since the last snapshot
  float temp_avg_c;            // average device temperature
  float freq_hz;               // 0 if unavailable
  uint16_t frames_lost;        // missed frames in this window
  int16_t wifi_rssi_dbm;       // 0 if unavailable

  // Per-panel power indexed by stable slot. Unused slots stay at 0.0f and
  // encode to int16_t 0 — distinguishable from valid panels in queries
  // because the slot map only ever points at really-seen barcodes.
  float panel_p_w[kMaxPanelSlots];
};

// One entry of the persistent slot map. `barcode_last6` is the matching key
// (matches CCA fuzzy match in match_barcode()); `slot` is the absolute
// 0..kMaxPanelSlots-1 index into panel time series.
struct PanelSlot {
  std::string barcode_last6;
  uint8_t slot;
};

class TigoHistory {
 public:
  // RAII guard over the component-wide flash lock (see fs_mutex_ below).
  // ANY code that touches littlefs must hold this for the whole operation —
  // including callers outside this class that reach flash directly, such as
  // /api/tsdb/stats (esp_littlefs_info -> lfs_fs_size traverses block
  // metadata, which is a flash read like any other).
  //
  // timeout_ms == 0 means wait forever. Always check held() before proceeding
  // when you passed a timeout; a false return means "do not touch flash".
  class FlashLock {
   public:
    // Default mirrors kFsLockReaderWaitMs in the .cpp — must stay above a full
    // commit (~21 s), or readers fail instead of waiting.
    explicit FlashLock(TigoHistory *hist, uint32_t timeout_ms = 30000);
    ~FlashLock();
    FlashLock(const FlashLock &) = delete;
    FlashLock &operator=(const FlashLock &) = delete;
    bool held() const { return held_; }

   private:
    SemaphoreHandle_t mutex_;
    bool held_;
  };

  // Mounts LittleFS on the `tsdb` partition and opens system + panel DBs.
  // Loads /tsdb/panel_map.json if present. Returns true on success.
  bool init();

  // Spawns the dedicated FreeRTOS writer task. Must be called after init().
  bool start_writer_task();

  // Look up (or assign) the slot for a panel barcode. Idempotent: subsequent
  // calls with the same key return the same slot. New assignments persist
  // panel_map.json synchronously. Returns 0xFF if the table is full.
  uint8_t get_or_assign_slot(const std::string &barcode_last6);

  // Read-only snapshot of current slot assignments (for the JSON API).
  std::vector<PanelSlot> snapshot_slot_map() const;

  // Encode + push a snapshot onto the writer queue. Non-blocking; drops the
  // sample (with a (W) log) if the queue is full.
  void enqueue_snapshot(const SystemSnapshot &snap);

  // Iterates rows in [start_ts, end_ts] (inclusive). For each row the callback
  // receives (timestamp, total_p in watts, total_e_kwh × 100 — divide by 100).
  // Returns number of rows yielded, or -1 on error.
  // Runs synchronously on the caller's task — fine to invoke from an HTTP
  // handler since esp_http_server runs on its own task.
  using PowerRowCb = std::function<void(uint32_t /*ts*/, int16_t /*total_p_w*/,
                                        int16_t /*total_e_kwh_x100*/)>;
  int iterate_power(uint32_t start_ts, uint32_t end_ts, const PowerRowCb &cb);

  // Iterates a single panel's power series. Picks the right DB based on slot.
  using PanelRowCb = std::function<void(uint32_t /*ts*/, int16_t /*power_w*/)>;
  int iterate_panel(uint8_t slot, uint32_t start_ts, uint32_t end_ts,
                    const PanelRowCb &cb);

  bool initialized() const { return initialized_; }

  // Pause/resume the writer's flash writes around an OTA. Set true on OTA start
  // so the writer skips its littlefs writes (which otherwise collide with the
  // OTA image write on the same flash chip and fault); cleared on OTA abort.
  // Written from the OTA task, read from the writer task — hence atomic.
  void set_ota_active(bool active) { ota_active_.store(active, std::memory_order_relaxed); }

  // Drains the writer queue (best-effort, with timeout) and closes every open
  // tsdb_t handle. Called from TigoMonitorComponent::on_shutdown() so that
  // user-initiated reboots (incl. /api/restart) commit pending writes to
  // flash. esp_littlefs's lfs_file_close issues the metadata commit that
  // bare fsync apparently doesn't on long-lived r+b file handles — without
  // this hook, every reboot wipes the in-progress tsdb files even though
  // each tsdb_write_h fflushes + fsyncs along the way.
  void flush_and_close();

  // Direct handle access for diagnostic endpoints (e.g. /api/tsdb/stats).
  // Caller must not close these — TigoHistory owns the lifecycle.
  tsdb_t *system_db() const { return system_db_; }
  tsdb_t *panel_db(size_t idx) const {
    return idx < kNumPanelDbs ? panel_db_[idx] : nullptr;
  }
  size_t panel_db_count() const { return kNumPanelDbs; }
  size_t slot_count() const { return slot_map_.size(); }
  uint8_t next_free_slot() const { return next_free_slot_; }

  // Everything /api/tsdb/stats reports, held in RAM.
  //
  // This exists because serving those numbers used to make the HTTP task read
  // flash, and on the ESP32-S3 that is what crashes us: reproduced 2026-07-28,
  // four concurrent callers of /api/tsdb/stats killed the device in ~11 s, and
  // caching the response for 60 s only made it rarer — it still died the moment
  // a cache rebuild landed on a writer commit. FlashLock does not prevent the
  // collision (why, is still unknown), so the fix is to remove one of the two
  // colliding parties: the writer refreshes this from inside its own flash
  // batch, and HTTP handlers only ever read the copy.
  struct StatsSnapshot {
    struct Db {
      bool present{false};    // handle exists (a lazily-unopened panel DB does not)
      bool available{false};  // the stats read succeeded
      esp_err_t error{ESP_OK};
      tsdb_stats_t stats{};
    };
    bool valid{false};        // false until the first refresh
    uint32_t updated_ms{0};   // esp_timer millis at capture; drives snapshot_age_ms
    size_t fs_total{0};
    size_t fs_used{0};
    size_t slot_count{0};
    uint8_t next_free_slot{0};
    Db system;
    Db panels[kNumPanelDbs];
  };

  // Thread-safe copy for HTTP handlers. Touches NO flash — that is the point.
  void copy_stats_snapshot(StatsSnapshot &out);

 private:
  static void writer_task_entry_(void *arg);
  void writer_task_loop_();

  bool mount_filesystem_();
  // Force LittleFS to commit its block-allocation journal. tsdb files reuse one
  // inode for life, so per-record writes never trigger a dir-tree op — the only
  // thing that commits the journal — and a reboot without a clean unmount then
  // wipes them. Called after each snapshot so data survives any reboot.
  void commit_journal_();
  // Recomputes stats_snapshot_ from flash. CALLER MUST HOLD FlashLock. Only
  // ever called from a context that is already inside a flash batch, so it
  // adds no new flash access to the system — see StatsSnapshot above.
  void refresh_stats_snapshot_();
  bool init_system_db_();
  // Opens panel_db_[idx] if not already open. Lazy: panel DBs only land on
  // flash when the rig actually has a panel mapped into that 16-slot range.
  bool open_panel_db_(size_t idx);
  bool load_slot_map_();
  bool save_slot_map_();

  bool initialized_{false};
  // True while an OTA is running — makes the writer task skip flash writes.
  std::atomic<bool> ota_active_{false};
  // Per-instance handles from the v2.1 multi-DB API. system_db_ holds the
  // 14-param rollups; panel_db_[i] each hold 16 panel powers. Striping across
  // multiple panel DBs sidesteps esp_tsdb's 16-base-param limit.
  tsdb_t *system_db_{nullptr};
  tsdb_t *panel_db_[kNumPanelDbs] = {};

  // Written by refresh_stats_snapshot_() (writer/loop task, under FlashLock),
  // read by HTTP handlers via copy_stats_snapshot(). stats_mutex_ guards only
  // the struct copy — it is deliberately NOT the flash lock, so a reader never
  // waits on, or interleaves with, anything touching flash.
  StatsSnapshot stats_snapshot_;
  std::mutex stats_mutex_;

  // barcode_last6 -> slot index. Persisted as panel_map.json on LittleFS.
  std::unordered_map<std::string, uint8_t> slot_map_;
  // Reverse: slot -> barcode (sized to kMaxPanelSlots). Empty string = unassigned.
  std::string slot_to_barcode_[kMaxPanelSlots];
  // Next free slot to assign. Slots are never recycled — replacements keep
  // their position in history forever.
  uint8_t next_free_slot_{0};

  // Serializes EVERY littlefs/SPI-flash access this component makes, across all
  // three tasks that reach flash: the writer task (tsdb_write_h + journal
  // commit), the ESPHome loop task (slot assignment -> tsdb_open +
  // panel_map.json), and the esp_http_server task (history queries).
  //
  // Why a component-level lock and not esp_tsdb's own: esp_tsdb's mutex is
  // per-handle (tsdb_internal.h:137), so a read of panels<N> and a write of
  // system are never serialized against each other — they are different
  // handles on the same flash chip. And the httpd task runs at
  // tskNO_AFFINITY, so it floats to core 0 and escapes the writer's core-1
  // pin entirely.
  //
  // What goes wrong without it (reproduced on the rig 2026-07-27, first try):
  // with CONFIG_SPI_FLASH_AUTO_SUSPEND off, every SPI1 op — read as well as
  // write — calls spi_flash_disable_interrupts_caches_and_other_cpu, which
  // cuts I-cache on BOTH cores and parks the other one. Two tasks entering
  // that path concurrently fault inside the stall coordination itself:
  //   spi_flash_disable_interrupts_caches_and_other_cpu (cache_utils.c:176)
  //     <- cache_disable <- spi1_start <- esp_flash_read <- lfs_bd_read
  //     <- lfs_file_flush <- vfs fsync <- tsdb_write_block  == "Fault - Unknown"
  // A month-range history query holds the bus for ~2.3 s, so an open web UI
  // reliably overlaps the snapshot commit.
  SemaphoreHandle_t fs_mutex_{nullptr};

  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};
  // Given by the writer task right before it self-deletes. flush_and_close
  // sends a sentinel snapshot then takes this; once we have it, the writer
  // is guaranteed to no longer be touching any tsdb_t or its FILE*, so
  // tsdb_close_h's fclose can't race the writer's fwrite.
  SemaphoreHandle_t writer_done_{nullptr};
};

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // __has_include
#endif  // USE_ESP_IDF
