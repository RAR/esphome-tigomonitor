#pragma once

// Bus state: decodes frames with the shared tigo_protocol.h, tracks the latest
// reading per panel, and keeps the node table (short address ↔ barcode) with
// the shared node_identity rules, so labels follow the barcode exactly as on
// the ESP32 (#74). Thread-safe: the serial thread feeds it, the publisher and
// web server read snapshots.

#include "tigo_protocol.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tigod {

using Clock = std::chrono::steady_clock;

// Field names match NodeTableData; node_identity's templates use them.
struct Node {
  std::string addr;          // short address, 4 hex chars
  std::string long_address;  // barcode, 16 hex chars
  std::string checksum;      // Tigo check character for addr
  int sensor_index = -1;
  bool is_persistent = true;
  std::string cca_label, cca_string_label, cca_mppt_label, cca_channel, cca_object_id;
  bool cca_validated = false;
  bool seen_in_frame27 = false;  // runtime only
};

struct Panel {
  std::string addr;
  std::string barcode;  // empty until a Frame 27 names this address
  std::string label;    // CCA label, if any
  esphome::tigo_monitor::protocol::PowerReading reading;
  float peak_power = 0;  // today's highest output power, W
  Clock::time_point updated;
  int64_t updated_unix_ms = 0;
  uint32_t readings = 0;
};

struct BusStats {
  uint64_t bytes = 0;
  uint32_t frames = 0;
  uint32_t bad_checksums = 0;
  uint32_t ends_without_start = 0;
  uint32_t overflows = 0;
  uint32_t power_packets = 0;
  uint32_t frame27 = 0;
  int64_t last_frame_unix_ms = 0;
};

class Monitor {
 public:
  Monitor(std::string state_path, float power_calibration, size_t max_nodes);

  void feed(const char *data, size_t len);

  // A panel is online if it reported within this window.
  static constexpr auto ONLINE_WINDOW = std::chrono::minutes(2);

  std::vector<Panel> panels() const;
  std::vector<Node> nodes() const;
  BusStats stats() const;
  double energy_kwh() const;    // lifetime
  double today_kwh() const;
  // Completed days, oldest first, as {YYYY-MM-DD, kWh}; up to HISTORY_DAYS.
  std::vector<std::pair<std::string, double>> daily_history() const;
  static constexpr size_t HISTORY_DAYS = 90;
  float total_power() const;  // output power of online panels, W

  // Integrates total_power into energy_kwh; call periodically.
  void tick();

  bool load();
  bool save();
  void set_power_calibration(float c);
  void reset_peaks();
  // Forgets every node and panel; they come back with the next Frame 27.
  void reset_nodes();

  // Replaces the node table with a backup (the /api/nodes export), the way
  // the firmware's import_node_table() does: an entry whose barcode a Frame 27
  // has since placed at another address moves there, one whose address now
  // belongs to another panel waits for its own Frame 27, and entries sharing
  // a barcode fold into one.
  struct ImportResult {
    size_t imported = 0;
    int remapped = 0, parked = 0;
  };
  ImportResult import_nodes(const std::vector<Node> &in);
  size_t max_nodes() const { return max_nodes_; }

 private:
  void on_frame_(const std::string &raw);
  void on_power_packet_(const std::string &packet);
  void on_frame27_(const std::string &hex);
  Panel *find_panel_(const std::string &addr);
  float total_power_locked_() const;
  bool save_locked_();

  std::string state_path_;
  float power_calibration_;
  size_t max_nodes_;

  mutable std::mutex mu_;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
  std::vector<Panel> panels_;
  std::vector<Node> nodes_;
  std::vector<Node> parked_cca_;
  BusStats stats_;
  double energy_kwh_ = 0;
  double today_kwh_ = 0;
  std::string today_;  // local date today_kwh_ belongs to
  std::vector<std::pair<std::string, double>> history_;
  Clock::time_point last_tick_{};
  Clock::time_point last_save_{};
  bool dirty_ = false;
};

}  // namespace tigod
