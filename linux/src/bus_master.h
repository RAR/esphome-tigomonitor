#pragma once

// The daemon as the TAP's bus master, in the CCA's place. The logic is
// shared with the ESP32 (components/tigo_master/master_core.h); this is the
// serial port it runs on here, and the daemon's names for it.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the lease and the array goes safe.

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
  using Targets = esphome::tigo_master::MasterCore::Targets;
  using Learned = esphome::tigo_master::MasterCore::Learned;
  BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping = false,
            std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned = {}, Targets targets = nullptr,
            bool queries = false, bool writes = false);

  void set_standby(uint32_t takeover_s, Sink on_listen, Learned learned) {
    core_.set_standby(takeover_s, std::move(on_listen), std::move(learned));
  }
  bool open(const std::string &device, int baud) {
    port_.set_device(device, baud);
    return core_.open();
  }
  void tick(uint32_t now_ms) { core_.tick(now_ms); }
  void close() { core_.close(); }
  MasterStatus status() const { return core_.status(); }

 private:
  SerialTransport port_;  // before core_, which uses it from its first tick to its destructor
  esphome::tigo_master::MasterCore core_;
};

}  // namespace tigod
