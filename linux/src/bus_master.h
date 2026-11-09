#pragma once

// The daemon as the TAP's bus master, in the CCA's place. The logic is
// shared with the ESP32 (components/tigo_master/master_core.h); this is the
// serial port it runs on here, and the daemon's names for it.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the TAP's discovery window. That is no fail-safe: on the roof's
// TS4-A-O optimizers the array kept producing with no master and the window
// closed, RSS on or off (measured 2026-11-03). Rapid shutdown is the RSD
// cutting the TAP's power.

#include "../../components/tigo_master/master_core.h"

#include <string>

namespace tigod {

using esphome::tigo_master::Fault;
using esphome::tigo_master::fault_name;
using esphome::tigo_master::MasterStatus;
using esphome::tigo_master::Phase;
using esphome::tigo_master::phase_name;
using esphome::tigo_master::Standby;
using esphome::tigo_master::standby_name;

// A serial port by name, as the listen role opens it: "auto" and by-id paths
// resolved, and no network bridge (the master needs the bus itself).
class SerialTransport : public esphome::tigo_master::Transport {
 public:
  void set_device(const std::string &device, int baud) {
    device_ = device;
    baud_ = baud;
  }
  bool configured() const override { return !device_.empty(); }
  bool open(std::string &error) override;
  void close() override;
  bool is_open() const override { return fd_ >= 0; }
  bool write(const uint8_t *data, size_t n, std::string &error) override;
  void drain() override;
  int read(char *buf, size_t n, int timeout_ms) override;
  int baud() const override { return baud_; }
  std::string name() const override { return path_; }

 private:
  std::string device_, path_;
  int baud_ = 38400;
  int fd_ = -1;
};

class BusMaster {
 public:
  using Sink = esphome::tigo_master::MasterCore::Sink;
  using SentSink = esphome::tigo_master::MasterCore::SentSink;
  using Targets = esphome::tigo_master::MasterCore::Targets;
  using Learned = esphome::tigo_master::MasterCore::Learned;
  BusMaster(Sink on_rx, SentSink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping = false,
            std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned = {}, Targets targets = nullptr,
            bool queries = false, bool writes = false);

  void set_standby(uint32_t takeover_s, Sink on_listen, Learned learned) {
    core_.set_standby(takeover_s, std::move(on_listen), std::move(learned));
  }
  bool open(const std::string &device, int baud) {
    port_.set_device(device, baud);
    return core_.open();
  }
  void set_pv(const esphome::tigo_master::PvState &state, esphome::tigo_master::MasterCore::PvSaved saved,
              bool silent = false) {
    core_.set_pv(state, std::move(saved), silent);
  }
  bool request_pv(bool off) { return core_.request_pv(off); }
  bool request_discovery(bool renew) { return core_.request_discovery(renew); }
  void set_discovery(bool renew, esphome::tigo_master::MasterCore::DiscoverySaved saved) {
    core_.set_discovery(renew, std::move(saved));
  }
  void set_read_tap_config(bool on) { core_.set_read_tap_config(on); }
  void set_table_writes(bool on) { core_.set_table_writes(on); }
  void set_table_saved(esphome::tigo_master::MasterCore::TableSaved saved, const esphome::tigo_monitor::master::Bytes &mac,
                       const std::vector<esphome::tigo_monitor::master::TableRecord> *resume) {
    core_.set_table_saved(std::move(saved), mac, resume);
  }
  bool request_table_remove(const esphome::tigo_monitor::master::Bytes &mac, bool dry_run, std::string &why) {
    return core_.request_table_remove(mac, dry_run, why);
  }
  bool request_table_replace(const esphome::tigo_monitor::master::Bytes &old_mac,
                             const esphome::tigo_monitor::master::Bytes &new_mac, bool dry_run, std::string &why) {
    return core_.request_table_replace(old_mac, new_mac, dry_run, why);
  }
  void set_on_replaced(esphome::tigo_master::MasterCore::TableReplaced cb) { core_.set_on_replaced(std::move(cb)); }
  bool request_table_stop_waiting(const esphome::tigo_monitor::master::Bytes &mac) {
    return core_.request_table_stop_waiting(mac);
  }
  bool request_table_add(const esphome::tigo_monitor::master::Bytes &mac, uint16_t id, bool dry_run, std::string &why) {
    return core_.request_table_add(mac, id, dry_run, why);
  }
  void set_window_wanted(bool wanted, int missing) { core_.set_window_wanted(wanted, missing); }
  std::map<uint16_t, esphome::tigo_monitor::master::Queries::Setup> setups() const { return core_.setups(); }
  void tick(uint32_t now_ms) { core_.tick(now_ms); }
  void close() { core_.close(); }
  MasterStatus status() const { return core_.status(); }

 private:
  SerialTransport port_;  // before core_, which uses it from its first tick to its destructor
  esphome::tigo_master::MasterCore core_;
};

}  // namespace tigod
