#pragma once

// Bus state: decodes frames with the shared tigo_protocol.h, tracks the latest
// reading per panel, and keeps the node table (short address ↔ barcode) with
// the shared node_identity rules, so labels follow the barcode exactly as on
// the ESP32 (#74). Thread-safe: the serial thread feeds it, the publisher and
// web server read snapshots.

#include "link_stats.h"
#include "tigo_layout.h"
#include "tigo_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <set>
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
  // When it last reported a reading, and when the TAP's roster (a Frame 27)
  // last listed it: unix s, saved to the hour. 0: not yet.
  int64_t last_heard_unix = 0;
  int64_t last_roster_unix = 0;
  // The owner marked it known: listed by the TAP but not reporting, and
  // expected not to (one that never joined, say). Not missing, not counted.
  // Cleared by its first reading.
  bool known = false;
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
  // Each optimizer's link: its TAP, reports lost today, RSSI over a day.
  esphome::tigo_monitor::link::LinkStats links() const;
  // A new optimizer: a barcode first heard (in a Frame 27) more than a day
  // after this monitor first heard any, so a fresh install's own panels
  // aren't news. When each was first heard, by barcode (unix s).
  std::map<std::string, int64_t> first_heard() const;
  // Those heard in the last NEW_FOR_S that no string holds yet: the ones to
  // place in the layout. A replacement shows up here.
  int new_optimizers(int64_t now_unix) const;
  static const int64_t NEW_AFTER_S = 86400;
  static const int64_t NEW_FOR_S = 7 * 86400;
  // Missing: a node with a barcode that hasn't reported for MISSING_AFTER_S,
  // or that the roster stopped listing OFF_ROSTER_AFTER_S before its newest
  // entry. A dead or removed optimizer shows up here.
  int missing_optimizers(int64_t now_unix) const;
  // For automatic discovery (discovery_policy.h): when each optimizer placed
  // in the layout (a string label, not marked known) last reported, and the
  // newest report from any panel (unix s, 0 = none).
  std::vector<int64_t> expected_last_heard() const;
  int64_t last_report_unix() const;
  // Marks the node with this barcode known (or not). False: no such node.
  bool set_known(const std::string &barcode, bool known);
  static const int64_t MISSING_AFTER_S = 3 * 86400;
  static const int64_t OFF_ROSTER_AFTER_S = 86400;
  static const int64_t SEEN_SAVE_EVERY_S = 3600;  // how stale a saved last_heard/last_roster may be
  // Added to the new optimizer log line (main.cpp's new_optimizer_note).
  void set_new_optimizer_note(std::string note) {
    std::lock_guard<std::mutex> lock(mu_);
    new_note_ = std::move(note);
  }
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
  // Where apply_layout would put each panel it matches, without writing.
  std::vector<esphome::tigo_monitor::layout::Assignment> resolve_layout(const std::vector<LayoutPanel> &panels) const;
  // The Topology editor's: places nodes by exact barcode. With clear_unlisted,
  // a node the list leaves out loses its panel name, string and MPPT.
  // Returns how many nodes matched.
  int set_assignments(const std::vector<esphome::tigo_monitor::layout::Assignment> &as, bool clear_unlisted);
  // Every node that has a barcode, as the labels it carries now.
  std::vector<esphome::tigo_monitor::layout::Assignment> current_assignments() const;
  // Drops one node (and its panel), as the Nodes page's delete does.
  bool delete_node(const std::string &addr);

 private:
  void on_frame_(const std::string &raw);
  void on_power_packet_(const std::string &packet, uint16_t tap);
  void on_netdiag_(const std::string &packet);
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
  esphome::tigo_monitor::link::LinkStats links_;
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
  // Every barcode this monitor has heard, since seen_since_; and when each
  // one heard after the first day was first heard.
  std::set<std::string> seen_barcodes_;
  int64_t seen_since_ = 0;
  std::map<std::string, int64_t> first_heard_;
  std::string new_note_;
  void note_barcode_(const std::string &barcode, const std::string &addr);

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
