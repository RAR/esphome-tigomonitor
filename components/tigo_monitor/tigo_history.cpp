#include "tigo_history.h"

#ifdef TIGO_TSDB_AVAILABLE

#include "esphome/core/log.h"

#include "esp_timer.h"

#include <climits>
#include <cmath>

namespace esphome {
namespace tigo_monitor {

static const char *const TAG = "tigo_history";

// Encoded row pushed onto the writer queue. Layout matches the schema below.
struct EncodedRow {
  uint32_t timestamp;
  int16_t values[14];
};

// Encoders — clamp to int16 range to avoid silent wraparound on runaway sensors.
static int16_t enc_clamp_(float v) {
  if (std::isnan(v)) return 0;
  if (v <= INT16_MIN) return INT16_MIN;
  if (v >= INT16_MAX) return INT16_MAX;
  return static_cast<int16_t>(lroundf(v));
}
static int16_t enc_w_(float w) { return enc_clamp_(w); }
static int16_t enc_kwh_(float kwh) { return enc_clamp_(kwh * 100.0f); }
static int16_t enc_temp_(float c) { return enc_clamp_(c); }
static int16_t enc_dhz_(float hz) { return enc_clamp_(hz * 10.0f); }

// Schema for system.tsdb — system + per-inverter rollups, 5-min cadence.
// Order is fixed once data has been written; appending requires migration.
// See docs/tsdb-integration.md for unit/scaling reference.
static const char *kSystemParamNames[] = {
    "total_p",     "total_e",
    "inv1_p",      "inv1_e",
    "inv2_p",      "inv2_e",
    "inv3_p",      "inv3_e",
    "inv4_p",      "inv4_e",
    "temp_avg",    "freq",
    "frames_lost", "wifi_rssi",
};
static constexpr size_t kSystemNumParams =
    sizeof(kSystemParamNames) / sizeof(kSystemParamNames[0]);

// Cap system.tsdb at ~2 MB on the 3 MB tsdb partition; leaves ~1 MB for
// panels.tsdb (added in a later phase).
static constexpr size_t kSystemFileBytes = 2 * 1024 * 1024;

bool TigoHistory::init() {
  if (!mount_filesystem_())
    return false;
  if (!init_system_db_())
    return false;
  initialized_ = true;
  return true;
}

bool TigoHistory::mount_filesystem_() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/tsdb";
  conf.partition_label = "tsdb";
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err == ESP_ERR_INVALID_STATE) {
    // Already mounted (e.g. soft-reboot path) — treat as success.
    ESP_LOGI(TAG, "LittleFS already mounted on /tsdb");
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "LittleFS mount on /tsdb failed: %s", esp_err_to_name(err));
    return false;
  } else {
    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info("tsdb", &total, &used) == ESP_OK && total > 0) {
      ESP_LOGI(TAG, "LittleFS mounted on /tsdb: %zu KB / %zu KB used (%.1f%%)",
               used / 1024, total / 1024, 100.0f * used / total);
    } else {
      ESP_LOGI(TAG, "LittleFS mounted on /tsdb (info unavailable)");
    }
  }
  return true;
}

bool TigoHistory::init_system_db_() {
  tsdb_config_t cfg = {};
  cfg.filepath = "/tsdb/system.tsdb";
  cfg.num_params = kSystemNumParams;
  cfg.param_names = kSystemParamNames;
  cfg.max_records = TSDB_CALC_MAX_RECORDS(kSystemFileBytes, kSystemNumParams);
  cfg.index_stride = 380;
  cfg.buffer_pool_size = 10 * 1024;
  cfg.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
  cfg.use_paged_allocation = false;
  cfg.page_size = 0;

  esp_err_t err = tsdb_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "tsdb_init for system.tsdb failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "tsdb opened: system.tsdb (%zu params, capacity ~%lu records)",
           kSystemNumParams, (unsigned long) cfg.max_records);
  return true;
}

bool TigoHistory::start_writer_task() {
  if (!initialized_) {
    ESP_LOGE(TAG, "start_writer_task() called before init()");
    return false;
  }
  if (task_ != nullptr) {
    return true;  // already running
  }
  // Queue depth 4 — at 5-min cadence we should never be more than 1 deep.
  // Extra headroom absorbs transient flash slowdowns without dropping samples.
  queue_ = xQueueCreate(4, sizeof(EncodedRow));
  if (queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create tsdb writer queue");
    return false;
  }
  // Stack 8 KB — tsdb_write + LittleFS ops + esp_log printf overflowed 4 KB
  // in practice. Priority 1 matches the main app task, well below UART.
  BaseType_t ok = xTaskCreate(&TigoHistory::writer_task_entry_, "tsdb_writer",
                              8192, this, 1, &task_);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create tsdb writer task");
    vQueueDelete(queue_);
    queue_ = nullptr;
    return false;
  }
  ESP_LOGI(TAG, "tsdb writer task started (queue depth 4, stack 8 KB)");
  return true;
}

void TigoHistory::enqueue_snapshot(const SystemSnapshot &snap) {
  if (queue_ == nullptr) return;
  if (snap.timestamp == 0) {
    ESP_LOGD(TAG, "Skipping snapshot — timestamp not yet valid");
    return;
  }

  EncodedRow row;
  row.timestamp = snap.timestamp;
  // Order must match kSystemParamNames in init_system_db_.
  row.values[0] = enc_w_(snap.total_p_w);
  row.values[1] = enc_kwh_(snap.period_e_kwh);
  row.values[2] = enc_w_(snap.inv_p_w[0]);
  row.values[3] = enc_kwh_(snap.inv_e_kwh[0]);
  row.values[4] = enc_w_(snap.inv_p_w[1]);
  row.values[5] = enc_kwh_(snap.inv_e_kwh[1]);
  row.values[6] = enc_w_(snap.inv_p_w[2]);
  row.values[7] = enc_kwh_(snap.inv_e_kwh[2]);
  row.values[8] = enc_w_(snap.inv_p_w[3]);
  row.values[9] = enc_kwh_(snap.inv_e_kwh[3]);
  row.values[10] = enc_temp_(snap.temp_avg_c);
  row.values[11] = enc_dhz_(snap.freq_hz);
  row.values[12] = enc_clamp_((float) snap.frames_lost);
  row.values[13] = enc_clamp_((float) snap.wifi_rssi_dbm);

  // Non-blocking. If the queue is full, drop the sample with a warning.
  if (xQueueSend(queue_, &row, 0) != pdTRUE) {
    ESP_LOGW(TAG, "tsdb queue full — dropping snapshot @ %lu",
             (unsigned long) snap.timestamp);
  }
}

int TigoHistory::iterate_power(uint32_t start_ts, uint32_t end_ts,
                               const PowerRowCb &cb) {
  if (!initialized_)
    return -1;
  if (end_ts < start_ts)
    return 0;

  // Read only columns 0 (total_p) and 1 (total_e). Cuts flash IO roughly 7x
  // versus reading the full 14-param row.
  uint8_t cols[] = {0, 1};
  tsdb_query_t q;
  esp_err_t err = tsdb_query_init(&q, start_ts, end_ts, cols, 2);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "tsdb_query_init failed: %s", esp_err_to_name(err));
    return -1;
  }

  int count = 0;
  uint32_t ts = 0;
  // Defensively size the values buffer to the full schema in case the query
  // engine ignores the column selector and writes back all params.
  int16_t values[kSystemNumParams] = {0};
  while (tsdb_query_next(&q, &ts, values) == ESP_OK) {
    cb(ts, values[0], values[1]);
    ++count;
  }
  tsdb_query_close(&q);
  return count;
}

void TigoHistory::writer_task_entry_(void *arg) {
  static_cast<TigoHistory *>(arg)->writer_task_loop_();
}

void TigoHistory::writer_task_loop_() {
  EncodedRow row;
  for (;;) {
    if (xQueueReceive(queue_, &row, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    uint32_t t0 = (uint32_t) (esp_timer_get_time() / 1000);
    esp_err_t err = tsdb_write(row.timestamp, row.values);
    uint32_t dt = (uint32_t) (esp_timer_get_time() / 1000) - t0;
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "tsdb_write @ %lu failed after %u ms: %s (stack hwm %u B)",
               (unsigned long) row.timestamp, (unsigned) dt,
               esp_err_to_name(err), (unsigned) (hwm * sizeof(StackType_t)));
    } else {
      ESP_LOGD(TAG, "tsdb_write @ %lu ok in %u ms (stack hwm %u B free)",
               (unsigned long) row.timestamp, (unsigned) dt,
               (unsigned) (hwm * sizeof(StackType_t)));
    }
  }
}

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // TIGO_TSDB_AVAILABLE
