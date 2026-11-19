#pragma once

// The decoded bus log and the reconfiguration alarm (bus_watch.h), fed with
// every byte read off the bus and, in the master role, every byte sent.
//
// The alarm latches: once a frame that reconfigures a TAP is heard, it stays
// on through restarts (kept in <state_dir>/bus-alarm.json: when, and which
// frame by name, never its bytes) until the owner clears it.
//
// It also keeps the firmware versions heard (the optimizers' answers to
// ^00Version, the TAP's to VERSION) in <state_dir>/versions.json.

#include "bus_book.h"
#include "bus_watch.h"
#include "tigo_protocol.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <string>

namespace tigod {

class BusLog {
 public:
  static const size_t KEEP_ALARMS = 20;

  explicit BusLog(std::string alarm_path, std::string versions_path = "");
  void load();
  void feed_rx(const char *data, size_t n);  // read off the bus
  // This monitor's master sent it; its last byte left late_ms before now.
  void feed_tx(const char *data, size_t n, uint32_t late_ms = 0);
  // Writes the alarm and versions files if they changed since the last
  // save. It can wait on the SD card: call it off the bus master's loop.
  void save_if_changed();
  bool needs_save() const;
  void clear_alarm();

  bool alarm_active() const;
  // "GatewayCfg write (…), 3 frames, last 2026-11-07 01:02:03 UTC" for health.
  std::string alarm_summary() const;
  // /api/bus: the alarm, the last 5 minutes in numbers, then up to limit of
  // the newest exchanges.
  nlohmann::json to_json(size_t limit) const;
  // /api/versions: {taps: [...], optimizers: [...], radios: [...], ids: [...],
  // settings: [...]}, each by address; radios are the TAPs' channel, SLT and
  // SLm as last read, ids each TAP's MAC, settings each optimizer's answer to
  // ^00Smrt, mppt its four MPPT parameters from its answer to ^00Mppt,
  // ratings its rated volts and watts from its answer to ^00Cal.
  nlohmann::json versions_json() const;
  // Each TAP's radio settings heard, PAN included (for choosing a new TAP's).
  std::vector<esphome::tigo_monitor::master::TapRadio> radios() const;
  // The address of the TAP that gave this MAC (16 hex digits), or 0.
  uint16_t tap_of_mac(const std::string &mac) const;
  // Each TAP address's MAC (16 hex digits), as last heard there.
  std::map<uint16_t, std::string> tap_macs() const;
  // How many optimizer settings, MPPT or firmware changes were heard since.
  int changes_since(int64_t since_ms) const;
  // The barcode at an address in the node table, or "": an answer from an
  // address another optimizer has taken since (a gateway renumbering) then
  // starts that address's record afresh instead of logging a change. Called
  // with this log's lock held, so it must not call back in.
  void set_barcode_of(std::function<std::string(uint16_t)> barcode_of) { book_.set_barcode_of(std::move(barcode_of)); }
  // Whether this monitor runs the bus now. While another master runs it,
  // what an optimizer reports changed is that master's doing: the new value
  // is kept, but not listed as a change or warned of; and a reconfiguration
  // it sends the TAP is marked in the log but raises no alarm.
  void set_runs_bus(bool on) { runs_bus_ = on; }

 private:
  struct Alarm {
    int64_t unix_ms;
    std::string what;
  };
  void on_frame_(const esphome::tigo_monitor::master::Frame &f, bool ours, int64_t now);
  void load_versions_();
  std::string path_, versions_path_;
  // The TAPs' and optimizers' firmware, settings, ratings and radios, shared
  // with the ESP32 (bus_book.h). Its dirty flag: versions.json to write.
  esphome::tigo_monitor::master::BusBook book_;
  nlohmann::json versions_json_(bool with_pans) const;
  std::atomic<bool> runs_bus_{false};
  mutable std::mutex mu_;
  esphome::tigo_monitor::master::BusWatch watch_;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> rx_, tx_;
  std::deque<std::string> sent_;  // our last few request bodies, to drop an adapter's echo of them
  std::deque<Alarm> alarms_;      // newest last
  uint64_t alarm_count_ = 0;
  int64_t first_alarm_ms_ = 0;
  std::string last_logged_;
  int64_t last_logged_ms_ = 0;
  bool dirty_ = false;
};

}  // namespace tigod
