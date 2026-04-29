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

namespace esphome {
namespace tigo_monitor {

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
};

class TigoHistory {
 public:
  // Mounts LittleFS on the `tsdb` partition and opens the system rollup db.
  // Returns true on success. Logs (E) on any failure.
  bool init();

  // Spawns the dedicated FreeRTOS writer task. Must be called after init().
  bool start_writer_task();

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

  bool initialized() const { return initialized_; }

 private:
  static void writer_task_entry_(void *arg);
  void writer_task_loop_();

  bool mount_filesystem_();
  bool init_system_db_();

  bool initialized_{false};
  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};
};

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // __has_include
#endif  // USE_ESP_IDF
