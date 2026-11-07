#pragma once

// The decoded bus log and the reconfiguration alarm (bus_watch.h), fed with
// every byte read off the bus and, in the master role, every byte sent.
//
// The alarm latches: once a frame that reconfigures a TAP is heard, it stays
// on through restarts (kept in <state_dir>/bus-alarm.json: when, and which
// frame by name, never its bytes) until the owner clears it.

#include "bus_watch.h"
#include "tigo_protocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace tigod {

class BusLog {
 public:
  static const size_t KEEP_ALARMS = 20;

  explicit BusLog(std::string alarm_path) : path_(std::move(alarm_path)) {}
  void load();
  void feed_rx(const char *data, size_t n);  // read off the bus
  void feed_tx(const char *data, size_t n);  // this monitor's master sent it
  // Writes the alarm file if it changed since the last save.
  void save_if_changed();
  void clear_alarm();

  bool alarm_active() const;
  // "GatewayCfg write (…), 3 frames, last 2026-11-07 01:02:03 UTC" for health.
  std::string alarm_summary() const;
  // /api/bus: the alarm, then up to limit of the newest events.
  nlohmann::json to_json(size_t limit) const;

 private:
  struct Alarm {
    int64_t unix_ms;
    std::string what;
  };
  void on_frame_(const esphome::tigo_monitor::master::Frame &f, bool ours, int64_t now);

  std::string path_;
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
