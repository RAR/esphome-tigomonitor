#include "tigo_history.h"

#ifdef TIGO_TSDB_AVAILABLE

#include "esphome/core/log.h"

namespace esphome {
namespace tigo_monitor {

static const char *const TAG = "tigo_history";

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

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // TIGO_TSDB_AVAILABLE
