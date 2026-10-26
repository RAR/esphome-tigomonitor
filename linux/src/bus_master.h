#pragma once

// The daemon as the TAP's bus master, in the CCA's place. It sends what the
// CCA sends in steady state (tigo_master.h): 0x0148 telemetry polls, and the
// run lease's check and renewal. Every byte it reads goes to the Monitor, so
// panels, history and MQTT work as they do when listening.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the lease and the array goes safe.

#include "monitor.h"

#include "tigo_master.h"
#include "tigo_protocol.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace tigod {

enum class Fault { none, no_port, no_reply, second_master, refused };
const char *fault_name(Fault f);

struct MasterStatus {
  bool active = false;  // the port is open and nothing has stopped it
  Fault fault = Fault::none;
  esphome::tigo_monitor::master::Lease lease;
  bool lease_read = false;
  uint32_t renewals = 0;
  uint32_t polls = 0;
  uint32_t timeouts = 0;
  int64_t last_reply_unix = 0;
  std::string detail;
};

class BusMaster {
 public:
  // bench: allow the reads a TAP on the bench answers (tigo_master.h's
  // bench_only); writes are never sent either way.
  BusMaster(Monitor &mon, uint16_t gateway_id, bool bench);
  ~BusMaster();

  bool open(const std::string &device, int baud);
  // Sends at most one request and reads its reply, up to REPLY_TIMEOUT_MS.
  void tick(uint32_t now_ms);
  void close();
  MasterStatus status() const;

  static const int REPLY_TIMEOUT_MS = 50;
  static const uint32_t REOPEN_EVERY_MS = 5000;
  static const uint32_t NO_REPLY_FAULT_AFTER = 10;

 private:
  // Sends one frame and reads until the gateway's reply ends or the timeout.
  // false on a timeout, a write error or a second master.
  bool transact(uint16_t cmd, const esphome::tigo_monitor::master::Bytes &payload,
                esphome::tigo_monitor::master::Frame &reply);
  void set_fault(Fault f, const std::string &detail);

  Monitor &mon_;
  uint16_t gateway_;
  bool bench_;
  int fd_ = -1;
  std::string device_;  // reopened every REOPEN_EVERY_MS while closed, as listening does
  int baud_ = 38400;
  bool tried_open_ = false;
  uint32_t last_open_try_ms_ = 0;
  esphome::tigo_monitor::master::LeaseKeeper lease_;
  esphome::tigo_monitor::master::PollCursor cursor_{0};
  uint8_t seq_ = 0;
  uint32_t timeouts_in_a_row_ = 0;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
  mutable std::mutex mu_;
  MasterStatus status_;
};

}  // namespace tigod
