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

#include "bus_watch.h"
#include "tigo_protocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <map>
#include <string>

namespace tigod {

class BusLog {
 public:
  static const size_t KEEP_ALARMS = 20;

  explicit BusLog(std::string alarm_path, std::string versions_path = "")
      : path_(std::move(alarm_path)), versions_path_(std::move(versions_path)) {}
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
  // /api/versions: {taps: [...], optimizers: [...], radios: [...]}, each by
  // address; radios are the TAPs' channel, SLT and SLm as last read.
  nlohmann::json versions_json() const;

 private:
  struct Alarm {
    int64_t unix_ms;
    std::string what;
  };
  void on_frame_(const esphome::tigo_monitor::master::Frame &f, bool ours, int64_t now);
  void load_versions_();

  std::string path_, versions_path_;
  esphome::tigo_monitor::master::VersionBook versions_;
  // Each TAP's radio settings, as last read (by anyone), and when.
  std::map<uint16_t, std::pair<esphome::tigo_monitor::master::TapRadio, int64_t>> radios_;
  bool versions_dirty_ = false;
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
