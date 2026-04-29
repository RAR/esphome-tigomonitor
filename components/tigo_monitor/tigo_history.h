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

namespace esphome {
namespace tigo_monitor {

class TigoHistory {
 public:
  // Mounts LittleFS on the `tsdb` partition and opens the system rollup db.
  // Returns true on success. Logs (E) on any failure.
  bool init();

  bool initialized() const { return initialized_; }

 private:
  bool mount_filesystem_();
  bool init_system_db_();

  bool initialized_{false};
};

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // __has_include
#endif  // USE_ESP_IDF
