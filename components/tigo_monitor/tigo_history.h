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
#include "freertos/task.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace esphome {
namespace tigo_monitor {

// esp_tsdb caps base params per DB at 16. We cover up to 48 panels by
// striping across three DB instances (panels0.tsdb, panels1.tsdb, panels2.tsdb).
// Sized for the user's 36->40 panel rig with 8 slots of growth headroom; bump
// kNumPanelDbs to 4 if a 64-panel install ever shows up (will require larger
// flash — see kPanelFileBytes math in tigo_history.cpp).
static constexpr size_t kPanelsPerDb = 16;
static constexpr size_t kNumPanelDbs = 3;
static constexpr size_t kMaxPanelSlots = kPanelsPerDb * kNumPanelDbs;

// Raw 5-min snapshot. The history layer encodes these to int16_t and writes.
// Caller fills this under the TigoMonitorComponent state lock.
struct SystemSnapshot {
  uint32_t timestamp;          // unix epoch seconds (0 = invalid, will be dropped)
  float total_p_w;             // system power in watts
  float period_e_kwh;          // energy produced in this 5-min window (kWh)
  float inv_p_w[4];            // per-inverter power
  float inv_e_kwh[4];          // per-inverter 5-min energy
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

  // Direct handle access for diagnostic endpoints (e.g. /api/tsdb/stats).
  // Caller must not close these — TigoHistory owns the lifecycle.
  tsdb_t *system_db() const { return system_db_; }
  tsdb_t *panel_db(size_t idx) const {
    return idx < kNumPanelDbs ? panel_db_[idx] : nullptr;
  }
  size_t panel_db_count() const { return kNumPanelDbs; }
  size_t slot_count() const { return slot_map_.size(); }
  uint8_t next_free_slot() const { return next_free_slot_; }

 private:
  static void writer_task_entry_(void *arg);
  void writer_task_loop_();

  bool mount_filesystem_();
  bool init_system_db_();
  // Opens panel_db_[idx] if not already open. Lazy: panel DBs only land on
  // flash when the rig actually has a panel mapped into that 16-slot range.
  bool open_panel_db_(size_t idx);
  bool load_slot_map_();
  bool save_slot_map_();

  bool initialized_{false};
  // Per-instance handles from the v2.1 multi-DB API. system_db_ holds the
  // 14-param rollups; panel_db_[i] each hold 16 panel powers. Striping across
  // multiple panel DBs sidesteps esp_tsdb's 16-base-param limit.
  tsdb_t *system_db_{nullptr};
  tsdb_t *panel_db_[kNumPanelDbs] = {};

  // barcode_last6 -> slot index. Persisted as panel_map.json on LittleFS.
  std::unordered_map<std::string, uint8_t> slot_map_;
  // Reverse: slot -> barcode (sized to kMaxPanelSlots). Empty string = unassigned.
  std::string slot_to_barcode_[kMaxPanelSlots];
  // Next free slot to assign. Slots are never recycled — replacements keep
  // their position in history forever.
  uint8_t next_free_slot_{0};

  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};
};

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // __has_include
#endif  // USE_ESP_IDF
