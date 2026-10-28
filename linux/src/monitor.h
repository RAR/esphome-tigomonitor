#pragma once

// Bus state: decodes frames with the shared tigo_protocol.h, tracks the latest
// reading per panel, and keeps the node table (short address ↔ barcode) with
// the shared node_identity rules, so labels follow the barcode exactly as on
// the ESP32 (#74). Thread-safe: the serial thread feeds it, the publisher and
// web server read snapshots.

#include "tigo_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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

// One string's live totals, grouped by the nodes' cca_string label — the
// firmware's StringData, aggregated the same way (output power, online panels).
struct StringTotals {
  std::string label;  // "All panels" for panels no string label claims
  std::string mppt;   // the nodes' cca_mppt label, if any
  double total_power = 0, total_current = 0;
  double avg_voltage_in = 0, avg_voltage_out = 0, avg_temperature = 0;
  double avg_efficiency = 0, min_efficiency = 0, max_efficiency = 0;
  double peak_power = 0;  // highest total today
  int active_devices = 0, total_devices = 0;
};

// One panel of a site layout (the Tigo cloud's). mppt is numbered across the
// whole system (MPPT 1, 2, 3, ...), as the inverters list expects.
struct LayoutPanel {
  std::string serial, label, string_label, mppt, object_id, channel;
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
  ~Monitor();  // writes any state still waiting

  void feed(const char *data, size_t len);

  // A panel is online while it reported within stale_timeout (the firmware's
  // stale_timeout, default 10 minutes; 0 keeps it online forever). Past that
  // its production reads 0, as on the ESP32.
  Clock::duration online_window() const;
  void set_stale_timeout_min(int minutes);
  // No frame from the bus for night_mode_timeout minutes: the panels have
  // gone dark for the night. Production reads 0, as on the ESP32.
  bool night_mode() const;
  void set_night_mode_timeout_min(int minutes);
  // reset_at_midnight: also clear the peaks when the day rolls over.
  void set_reset_at_midnight(bool on);

  std::vector<Panel> panels() const;
  std::vector<Node> nodes() const;
  BusStats stats() const;
  double energy_kwh() const;    // lifetime, panel output
  double energy_in_kwh() const;  // lifetime, panel input
  double today_kwh() const;
  double today_in_kwh() const;
  // Completed days, oldest first, as {YYYY-MM-DD, kWh}; up to HISTORY_DAYS.
  std::vector<std::pair<std::string, double>> daily_history() const;
  static constexpr size_t HISTORY_DAYS = 90;
  float total_power() const;     // output power of online panels, W
  float total_power_in() const;  // input power of online panels, W
  std::vector<StringTotals> strings() const;
  // Panels with no string label are grouped under this one.
  static constexpr const char *ALL_PANELS = "All panels";

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

  // Labels nodes from a layout; returns how many panels matched a node.
  // A cloud serial embeds the last 6 hex of the panel's barcode (barcode
  // 04C05B4000BBCC02 -> serial "4-BBCC02K"), which is the only link between
  // the two, so that is the match — as in the firmware.
  int apply_layout(const std::vector<LayoutPanel> &panels);
  // Drops one node (and its panel), as the Nodes page's delete does.
  bool delete_node(const std::string &addr);

 private:
  void on_frame_(const std::string &raw);
  void on_power_packet_(const std::string &packet);
  void on_frame27_(const std::string &hex);
  Panel *find_panel_(const std::string &addr);
  float total_power_locked_(bool input = false) const;
  std::vector<StringTotals> strings_locked_() const;
  bool night_locked_() const;
  bool save_locked_();
  void writer_loop_();

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
  double energy_in_kwh_ = 0, today_in_kwh_ = 0;
  Clock::duration online_window_ = std::chrono::minutes(10);
  Clock::duration night_timeout_ = std::chrono::minutes(60);
  Clock::time_point last_frame_{};
  double today_kwh_ = 0;
  std::string today_;  // local date today_kwh_ belongs to
  std::vector<std::pair<std::string, double>> history_;
  mutable std::map<std::string, double> string_peaks_;  // label -> W
  std::map<std::string, double> saved_peaks_;  // barcode -> W, from state.json until the panel reports
  bool reset_peaks_at_midnight_ = false;
  Clock::time_point last_tick_{};
  Clock::time_point last_save_{};
  bool dirty_ = false;

  // state.json is written on its own thread, outside mu_: an fsync on a slow
  // SD card took 0.9 s, and in the master role feed() and tick() run on the
  // loop that drives the bus. save_locked_() hands over the text; the newest
  // waiting copy wins.
  std::mutex write_mu_;
  std::condition_variable write_cv_;
  std::string pending_;
  bool has_pending_ = false, writing_ = false, stop_writer_ = false;
  std::atomic<bool> write_failed_{false};
  std::thread writer_;
};

}  // namespace tigod
