#include "tigo_history.h"

#ifdef TIGO_TSDB_AVAILABLE

#include "esphome/core/log.h"

#include "esp_timer.h"

#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>  // opendir, readdir
#include <errno.h>
#include <sys/stat.h>  // stat
#include <unistd.h>  // fsync, fileno

namespace esphome {
namespace tigo_monitor {

static const char *const TAG = "tigo_history";

// Encoded row pushed onto the writer queue. Layout matches the schemas below:
// — system_values mirror kSystemParamNames (14 entries)
// — panel_values is laid out as [panel_db_0 first 16][panel_db_1 next 16]
struct EncodedRow {
  uint32_t timestamp;
  int16_t system_values[14];
  int16_t panel_values[kMaxPanelSlots];
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

// Per-panel schema — same 16 names in each DB. The slot map (panel_map.json)
// resolves slot -> barcode at the application layer; the DB just sees columns.
static const char *kPanelParamNames[kPanelsPerDb] = {
    "p00", "p01", "p02", "p03", "p04", "p05", "p06", "p07",
    "p08", "p09", "p10", "p11", "p12", "p13", "p14", "p15",
};

// Cap system.tsdb at ~2 MB on the 3 MB tsdb partition; leaves ~1 MB shared
// across the panel DBs and LittleFS overhead.
static constexpr size_t kSystemFileBytes = 2 * 1024 * 1024;

// 256 KB per panel DB × 3 DBs = 768 KB; plus 2 MB system + ~200 KB LittleFS
// overhead fits the 3 MB partition with ~60 KB headroom. At 16 panels × 2
// bytes + 4-byte ts = 36 B/record, this gives ~7200 records per DB ≈ 25 days
// of 5-min data per panel — short of the design doc's 30-day target but
// acceptable until a 16 MB hardware variant exists.
static constexpr size_t kPanelFileBytes = 256 * 1024;

static constexpr const char *kPanelMapPath = "/tsdb/panel_map.json";

bool TigoHistory::init() {
  if (!mount_filesystem_())
    return false;

  // Diagnostic: dump pre-init dir listing for /tsdb. Ground truth for
  // "did the previous session's writes actually reach flash and survive".
  // Run AFTER mount but BEFORE tsdb_open (tsdb_open will unlink+recreate
  // a 0-byte file, hiding the evidence).
  {
    DIR *dp = opendir("/tsdb");
    if (dp == nullptr) {
      ESP_LOGW(TAG, "boot dir: opendir(/tsdb) failed errno=%d", errno);
    } else {
      int n = 0;
      struct dirent *ent;
      while ((ent = readdir(dp)) != nullptr) {
        char full[64];
        snprintf(full, sizeof(full), "/tsdb/%s", ent->d_name);
        struct stat st = {};
        if (stat(full, &st) == 0) {
          ESP_LOGI(TAG, "boot dir: %s (%ld bytes)", ent->d_name,
                   (long) st.st_size);
        } else {
          ESP_LOGI(TAG, "boot dir: %s (stat failed errno=%d)", ent->d_name,
                   errno);
        }
        ++n;
      }
      closedir(dp);
      ESP_LOGI(TAG, "boot dir: %d entry(ies) total", n);
    }

    // Additional smoke test: does the previous session's shutdown marker
    // (written via plain open-write-close pattern) survive? If yes, plain
    // file persistence works on this FS and the bug is specific to the
    // long-lived r+b handles. If no, we have a deeper FS issue.
    FILE *m = fopen("/tsdb/shutdown_marker.json", "rb");
    if (m == nullptr) {
      ESP_LOGW(TAG, "boot marker: shutdown_marker.json absent (errno=%d)", errno);
    } else {
      char buf[64] = {};
      size_t r = fread(buf, 1, sizeof(buf) - 1, m);
      fclose(m);
      ESP_LOGI(TAG, "boot marker: shutdown_marker.json present, %zu bytes: %s",
               r, buf);
    }
  }

  if (!init_system_db_())
    return false;
  // Panel DBs are opened lazily — load_slot_map_() will open any DBs
  // referenced by previously-saved slot assignments, and get_or_assign_slot()
  // opens new ones as panels appear. Empty installs commit zero panel-DB
  // flash; large installs grow into 1, 2, or 3 DBs naturally.
  if (!load_slot_map_()) {
    ESP_LOGW(TAG, "Slot map load failed — starting with empty mapping");
    slot_map_.clear();
    for (auto &b : slot_to_barcode_) b.clear();
    next_free_slot_ = 0;
  }
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

  system_db_ = tsdb_open(&cfg);
  if (system_db_ == nullptr) {
    ESP_LOGE(TAG, "tsdb_open for system.tsdb failed");
    return false;
  }
  ESP_LOGI(TAG, "tsdb opened: system.tsdb (%zu params, capacity ~%lu records)",
           kSystemNumParams, (unsigned long) cfg.max_records);
  return true;
}

bool TigoHistory::open_panel_db_(size_t idx) {
  if (idx >= kNumPanelDbs) return false;
  if (panel_db_[idx] != nullptr) return true;  // already open

  char path[32];
  std::snprintf(path, sizeof(path), "/tsdb/panels%zu.tsdb", idx);

  tsdb_config_t cfg = {};
  cfg.filepath = path;
  cfg.num_params = kPanelsPerDb;
  cfg.param_names = kPanelParamNames;
  cfg.max_records = TSDB_CALC_MAX_RECORDS(kPanelFileBytes, kPanelsPerDb);
  cfg.index_stride = 380;
  // Smaller pool than system db — each panel DB is ~3.5x smaller and reads
  // are rare (one column at a time). 6 KB covers read+write+query buffers.
  cfg.buffer_pool_size = 6 * 1024;
  cfg.alloc_strategy = TSDB_ALLOC_INTERNAL_RAM;
  cfg.use_paged_allocation = false;
  cfg.page_size = 0;

  panel_db_[idx] = tsdb_open(&cfg);
  if (panel_db_[idx] == nullptr) {
    ESP_LOGE(TAG, "tsdb_open for %s failed", path);
    return false;
  }
  ESP_LOGI(TAG, "tsdb opened: %s (%zu panels, capacity ~%lu records)", path,
           kPanelsPerDb, (unsigned long) cfg.max_records);
  return true;
}

// Tiny hand-rolled JSON for the slot map — avoids pulling in a parser for
// what's ultimately a flat list of 6-char keys to uint8 values. Format:
//   {"slots":[{"b":"abc123","s":0},{"b":"def456","s":1}]}
// Reads tolerate trailing commas and whitespace between tokens.
bool TigoHistory::load_slot_map_() {
  slot_map_.clear();
  for (auto &b : slot_to_barcode_) b.clear();
  next_free_slot_ = 0;

  FILE *f = fopen(kPanelMapPath, "rb");
  if (f == nullptr) {
    ESP_LOGI(TAG, "panel_map.json absent — starting empty");
    return true;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 8192) {
    ESP_LOGW(TAG, "panel_map.json size out of bounds: %ld", size);
    fclose(f);
    return true;  // Fresh start
  }

  std::string buf;
  buf.resize((size_t) size);
  size_t read = fread(buf.data(), 1, (size_t) size, f);
  fclose(f);
  if (read != (size_t) size) {
    ESP_LOGW(TAG, "panel_map.json short read");
    return true;
  }

  // Parse {"b":"xxxxxx","s":N} pairs.
  size_t pos = 0;
  while (pos < buf.size()) {
    size_t b_open = buf.find("\"b\"", pos);
    if (b_open == std::string::npos) break;
    size_t q1 = buf.find('"', b_open + 3);
    if (q1 == std::string::npos) break;
    size_t q2 = buf.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    std::string barcode = buf.substr(q1 + 1, q2 - q1 - 1);

    size_t s_open = buf.find("\"s\"", q2);
    if (s_open == std::string::npos) break;
    size_t colon = buf.find(':', s_open);
    if (colon == std::string::npos) break;
    int slot_int = -1;
    if (std::sscanf(buf.c_str() + colon + 1, " %d", &slot_int) != 1) break;

    if (!barcode.empty() && slot_int >= 0 && slot_int < (int) kMaxPanelSlots) {
      uint8_t slot = (uint8_t) slot_int;
      slot_map_[barcode] = slot;
      slot_to_barcode_[slot] = barcode;
      if (slot >= next_free_slot_) next_free_slot_ = slot + 1;
    }
    pos = colon + 1;
  }

  ESP_LOGI(TAG, "Loaded %zu panel slot mappings from %s (next_free=%u)",
           slot_map_.size(), kPanelMapPath, (unsigned) next_free_slot_);

  // Re-open any panel DBs that previously-saved slots map into. Without this
  // a query for an existing slot would race the next snapshot's lazy-open and
  // briefly fail with -1.
  for (uint8_t s = 0; s < next_free_slot_; ++s) {
    if (slot_to_barcode_[s].empty()) continue;
    open_panel_db_(s / kPanelsPerDb);
  }
  return true;
}

bool TigoHistory::save_slot_map_() {
  // Write directly to the destination. We tried temp-file + rename, but the
  // joltwallet LittleFS port we depend on returns EEXIST on rename when the
  // destination already exists (POSIX semantics expect clobber). Direct
  // write is good enough here:
  //  - LittleFS metadata updates are atomic at the block level, so a
  //    concurrent reader sees either the old or the new file, never a mix.
  //  - On power loss mid-write the file may end up truncated; the load path
  //    already tolerates partial JSON and falls back to the entries it could
  //    parse (worst case: a few panels get re-assigned to fresh slots,
  //    splitting their history — much better than no persistence at all).
  FILE *f = fopen(kPanelMapPath, "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Failed to open %s for write", kPanelMapPath);
    return false;
  }

  fputs("{\"slots\":[", f);
  bool first = true;
  for (uint8_t s = 0; s < kMaxPanelSlots; ++s) {
    if (slot_to_barcode_[s].empty()) continue;
    if (!first) fputc(',', f);
    first = false;
    std::fprintf(f, "{\"b\":\"%s\",\"s\":%u}", slot_to_barcode_[s].c_str(),
                 (unsigned) s);
  }
  fputs("]}\n", f);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  return true;
}

uint8_t TigoHistory::get_or_assign_slot(const std::string &barcode_last6) {
  if (barcode_last6.empty()) return 0xFF;

  auto it = slot_map_.find(barcode_last6);
  if (it != slot_map_.end()) return it->second;

  if (next_free_slot_ >= kMaxPanelSlots) {
    // Capacity exhausted. Caller should drop this panel's series silently.
    return 0xFF;
  }

  uint8_t slot = next_free_slot_++;
  slot_map_[barcode_last6] = slot;
  slot_to_barcode_[slot] = barcode_last6;

  // Lazy DB open: only commit panels<idx>.tsdb to flash when this slot's
  // bucket actually has its first panel. Pays the ~290ms tsdb_open cost on
  // the slot-assignment path (typically once per ~16 panels at install
  // time), not on every snapshot.
  open_panel_db_(slot / kPanelsPerDb);

  ESP_LOGI(TAG, "Assigned panel slot %u to %s", (unsigned) slot,
           barcode_last6.c_str());

  // Synchronous save — slot assignments are infrequent (only on first sight
  // of a barcode) and the cost of losing one is "panel rejoins as a new
  // slot, history splits in two", which we want to avoid.
  save_slot_map_();
  return slot;
}

std::vector<PanelSlot> TigoHistory::snapshot_slot_map() const {
  std::vector<PanelSlot> out;
  out.reserve(slot_map_.size());
  for (uint8_t s = 0; s < kMaxPanelSlots; ++s) {
    if (slot_to_barcode_[s].empty()) continue;
    out.push_back({slot_to_barcode_[s], s});
  }
  return out;
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
  // in practice. Three back-to-back writes per drain (system + 2x panels)
  // adds peak depth but stays well under 8 KB; soak shows ~3.5 KB hwm.
  // Priority 1 matches the main app task, well below UART.
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
  row.system_values[0] = enc_w_(snap.total_p_w);
  row.system_values[1] = enc_kwh_(snap.period_e_kwh);
  row.system_values[2] = enc_w_(snap.inv_p_w[0]);
  row.system_values[3] = enc_kwh_(snap.inv_e_kwh[0]);
  row.system_values[4] = enc_w_(snap.inv_p_w[1]);
  row.system_values[5] = enc_kwh_(snap.inv_e_kwh[1]);
  row.system_values[6] = enc_w_(snap.inv_p_w[2]);
  row.system_values[7] = enc_kwh_(snap.inv_e_kwh[2]);
  row.system_values[8] = enc_w_(snap.inv_p_w[3]);
  row.system_values[9] = enc_kwh_(snap.inv_e_kwh[3]);
  row.system_values[10] = enc_temp_(snap.temp_avg_c);
  row.system_values[11] = enc_dhz_(snap.freq_hz);
  row.system_values[12] = enc_clamp_((float) snap.frames_lost);
  row.system_values[13] = enc_clamp_((float) snap.wifi_rssi_dbm);

  for (size_t i = 0; i < kMaxPanelSlots; ++i) {
    row.panel_values[i] = enc_w_(snap.panel_p_w[i]);
  }

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
  esp_err_t err = tsdb_query_init_h(system_db_, &q, start_ts, end_ts, cols, 2);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "tsdb_query_init_h failed: %s", esp_err_to_name(err));
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

int TigoHistory::iterate_panel(uint8_t slot, uint32_t start_ts, uint32_t end_ts,
                               const PanelRowCb &cb) {
  if (!initialized_) return -1;
  if (slot >= kMaxPanelSlots) return -1;
  if (end_ts < start_ts) return 0;

  size_t db_idx = slot / kPanelsPerDb;
  uint8_t col = slot % kPanelsPerDb;
  if (panel_db_[db_idx] == nullptr) return -1;

  tsdb_query_t q;
  uint8_t cols[] = {col};
  esp_err_t err =
      tsdb_query_init_h(panel_db_[db_idx], &q, start_ts, end_ts, cols, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "panel %u tsdb_query_init_h failed: %s", (unsigned) slot,
             esp_err_to_name(err));
    return -1;
  }

  int count = 0;
  uint32_t ts = 0;
  int16_t values[kPanelsPerDb] = {0};
  while (tsdb_query_next(&q, &ts, values) == ESP_OK) {
    cb(ts, values[0]);
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

    esp_err_t err = tsdb_write_h(system_db_, row.timestamp, row.system_values);
    uint32_t t_sys = (uint32_t) (esp_timer_get_time() / 1000) - t0;

    // Panel writes happen back-to-back. A single failure on one DB shouldn't
    // skip the others — keep going so we lose at most one DB's data per row.
    uint32_t panel_total_ms = 0;
    for (size_t i = 0; i < kNumPanelDbs; ++i) {
      if (panel_db_[i] == nullptr) continue;
      uint32_t ti = (uint32_t) (esp_timer_get_time() / 1000);
      esp_err_t perr = tsdb_write_h(panel_db_[i], row.timestamp,
                                    row.panel_values + i * kPanelsPerDb);
      uint32_t dt = (uint32_t) (esp_timer_get_time() / 1000) - ti;
      panel_total_ms += dt;
      if (perr != ESP_OK) {
        ESP_LOGW(TAG, "panels%zu write @ %lu failed after %u ms: %s", i,
                 (unsigned long) row.timestamp, (unsigned) dt,
                 esp_err_to_name(perr));
      }
    }

    // Force LittleFS to commit each tsdb file's dir entry. Without this,
    // fopen("r+b") + fwrite + fflush + fsync leaves the file in an
    // un-published state — every reboot starts with system.tsdb missing
    // and the previous session's data unreadable. tsdb_sync_h does
    // fclose+fopen("r+b") so the dir-entry commit fires the way
    // panel_map.json's open-write-close pattern does.
    uint32_t t_sync_start = (uint32_t) (esp_timer_get_time() / 1000);
    if (system_db_ != nullptr) tsdb_sync_h(system_db_);
    for (size_t i = 0; i < kNumPanelDbs; ++i) {
      if (panel_db_[i] != nullptr) tsdb_sync_h(panel_db_[i]);
    }
    uint32_t t_sync_total = (uint32_t) (esp_timer_get_time() / 1000) - t_sync_start;

    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "tsdb_write @ %lu failed after %u ms: %s (stack hwm %u B)",
               (unsigned long) row.timestamp, (unsigned) t_sys,
               esp_err_to_name(err), (unsigned) (hwm * sizeof(StackType_t)));
    } else {
      ESP_LOGI(TAG,
               "tsdb_write @ %lu ok (sys %u ms, panels %u ms, sync %u ms, stack hwm %u B)",
               (unsigned long) row.timestamp, (unsigned) t_sys,
               (unsigned) panel_total_ms, (unsigned) t_sync_total,
               (unsigned) (hwm * sizeof(StackType_t)));
    }
  }
}

void TigoHistory::flush_and_close() {
  ESP_LOGI(TAG, "flush_and_close: ENTRY (initialized=%d)", initialized_ ? 1 : 0);
  if (!initialized_) {
    ESP_LOGW(TAG, "flush_and_close: not initialized — bailing out");
    return;
  }

  // Give the writer a brief window to drain whatever's already enqueued. We
  // don't pull the task down — its queue read is a portMAX_DELAY block, but
  // anything already received will have run tsdb_write_h before we get here.
  // 200 ms covers the common case (one row, ~120 ms total flash IO).
  if (queue_ != nullptr) {
    UBaseType_t pending = uxQueueMessagesWaiting(queue_);
    if (pending > 0) {
      ESP_LOGI(TAG, "flush_and_close: waiting on %u queued snapshot(s)",
               (unsigned) pending);
      uint32_t deadline = (uint32_t) (esp_timer_get_time() / 1000) + 800;
      while (uxQueueMessagesWaiting(queue_) > 0 &&
             (uint32_t) (esp_timer_get_time() / 1000) < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
  }

  // Close each open tsdb_t. tsdb_close_h calls fclose on the underlying
  // FILE*, which is what triggers esp_littlefs's per-file commit. After
  // tsdb_close_h the handle is freed — null the pointer so any racing
  // /api/tsdb/stats during shutdown returns "no DB" instead of UAF.
  if (system_db_ != nullptr) {
    tsdb_close_h(system_db_);
    system_db_ = nullptr;
    ESP_LOGI(TAG, "flush_and_close: system_db closed");
  }
  for (size_t i = 0; i < kNumPanelDbs; ++i) {
    if (panel_db_[i] == nullptr) continue;
    tsdb_close_h(panel_db_[i]);
    panel_db_[i] = nullptr;
    ESP_LOGI(TAG, "flush_and_close: panels%zu closed", i);
  }

  // Unmount LittleFS. Per-file fclose commits inode metadata, but the
  // **filesystem journal** (block allocation map, dir-entry table) only
  // commits on operations that touch the directory tree — and the tsdb
  // files re-use one inode for their entire lifetime, so the journal can
  // sit uncommitted across many writes. esp_vfs_littlefs_unregister()
  // calls lfs_unmount which does the final journal commit. Without this
  // every restart wipes the tsdb files; panel_map.json survives only
  // because save_slot_map_ creates a fresh file each save (the dir-entry
  // op forces a journal commit as a side effect).
  // Smoke test: write a marker file using the simple open(wb)+write+fclose
  // pattern that save_slot_map_ uses. We know that pattern works on the
  // live unit (panel_map.json persists). If THIS marker survives reboot
  // but the tsdb files don't, the issue is uniquely about long-lived r+b
  // handles. If even THIS doesn't survive, the FS or partition itself is
  // broken on this unit.
  {
    FILE *m = fopen("/tsdb/shutdown_marker.json", "wb");
    if (m == nullptr) {
      ESP_LOGW(TAG, "marker: fopen failed errno=%d", errno);
    } else {
      uint32_t now = (uint32_t) (esp_timer_get_time() / 1000);
      char buf[64];
      int n = snprintf(buf, sizeof(buf), "{\"ms\":%lu}\n", (unsigned long) now);
      size_t w = fwrite(buf, 1, (size_t) n, m);
      fflush(m);
      fsync(fileno(m));
      fclose(m);
      ESP_LOGI(TAG, "marker: wrote shutdown_marker.json (%zu bytes)", w);
    }
  }

  // Dir listing right before unmount. Names + sizes for everything LittleFS
  // currently sees. Compared against the boot dir listing on next boot,
  // this tells us exactly what does and doesn't survive a reboot.
  {
    DIR *dp = opendir("/tsdb");
    if (dp == nullptr) {
      ESP_LOGW(TAG, "pre-unmount dir: opendir failed errno=%d", errno);
    } else {
      int n = 0;
      struct dirent *ent;
      while ((ent = readdir(dp)) != nullptr) {
        char full[64];
        snprintf(full, sizeof(full), "/tsdb/%s", ent->d_name);
        struct stat st = {};
        if (stat(full, &st) == 0) {
          ESP_LOGI(TAG, "pre-unmount dir: %s (%ld bytes)", ent->d_name,
                   (long) st.st_size);
        } else {
          ESP_LOGI(TAG, "pre-unmount dir: %s (stat failed errno=%d)",
                   ent->d_name, errno);
        }
        ++n;
      }
      closedir(dp);
      ESP_LOGI(TAG, "pre-unmount dir: %d entry(ies) total", n);
    }
  }

  esp_err_t uerr = esp_vfs_littlefs_unregister("tsdb");
  if (uerr == ESP_OK) {
    ESP_LOGI(TAG, "flush_and_close: LittleFS /tsdb unmounted (journal committed)");
  } else {
    ESP_LOGW(TAG, "flush_and_close: LittleFS unmount failed: %s",
             esp_err_to_name(uerr));
  }

  initialized_ = false;
  ESP_LOGI(TAG, "flush_and_close: EXIT");
}

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // TIGO_TSDB_AVAILABLE
