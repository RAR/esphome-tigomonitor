#pragma once

// The daemon as the TAP's bus master, in the CCA's place. It sends what the
// CCA sends in steady state (tigo_master.h): 0x0148 telemetry polls, the run
// lease's check and renewal, and, with housekeeping on, the 0x41s learned
// while listening and the hourly 0x0B00. Every byte it reads goes to the Monitor, so
// panels, history and MQTT work as they do when listening.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the lease and the array goes safe.

#include "tigo_master.h"
#include "tigo_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tigod {

enum class Fault { none, no_port, no_reply, second_master, refused, lease_refused, discovery_failed, extra_tap };
enum class Phase { running, probing, discovering };
const char *phase_name(Phase p);
const char *fault_name(Fault f);

struct MasterStatus {
  bool active = false;  // the port is open and nothing has stopped it
  Fault fault = Fault::none;
  Phase phase = Phase::running;
  esphome::tigo_monitor::master::Lease lease;
  bool lease_read = false;
  uint32_t renewals = 0;
  uint32_t polls = 0;
  uint32_t timeouts = 0;
  uint32_t resyncs = 0;  // times the poll cursor was taken from the TAP's reply
  int64_t last_reply_unix = 0;
  std::vector<std::string> extra_taps;  // MACs (hex) of TAPs left at the factory address
  bool housekeeping = false;  // the setting
  uint32_t learned_41 = 0;    // 0x41 targets it has to send
  uint32_t sent_41 = 0;
  uint32_t sent_session = 0;  // 0x0B00s
  std::string detail;
};

class BusMaster {
 public:
  // on_rx gets every byte read from the bus (the Monitor, and --capture);
  // on_tx every byte written (--capture only). bench: allow the reads a TAP
  // on the bench answers (tigo_master.h's bench_only); writes are never sent
  // either way.
  using Sink = std::function<void(const char *, size_t)>;
  // startup: if the TAP is found at the factory address, re-address it to
  // gateway_id before polling (bus_master re-addressing writes, gated).
  // housekeeping: also send the learned 0x41s and the hourly 0x0B00
  // (tigo_master.h's Housekeeping); with none learned, only the 0x0B00.
  BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping = false,
            std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned = {});
  ~BusMaster();

  bool open(const std::string &device, int baud);
  // Sends at most one request and reads its reply, up to REPLY_TIMEOUT_MS.
  void tick(uint32_t now_ms);
  void close();
  MasterStatus status() const;

  static const int REPLY_TIMEOUT_MS = 50;
  static const uint32_t REOPEN_EVERY_MS = 5000;
  static const uint32_t LOG_FIRST_REPLIES = 20;  // poll reply headers logged at INFO, for the bench
  static const uint16_t FACTORY_ID = 0x1235;     // the universal factory RS485 address
  static const uint32_t NO_REPLY_FAULT_AFTER = 10;

 private:
  // Sends one frame and reads until the gateway's reply ends or the timeout.
  // false on a timeout, a write error or a second master.
  bool transact(uint16_t dest, uint16_t cmd, const esphome::tigo_monitor::master::Bytes &payload,
                esphome::tigo_monitor::master::Frame &reply, uint32_t timeout_ms = REPLY_TIMEOUT_MS);
  bool is_reply(uint16_t cmd, const esphome::tigo_monitor::master::Bytes &request,
                const esphome::tigo_monitor::master::Frame &f) const;
  void run_startup();  // probe + discovery, before steady state
  void set_fault(Fault f, const std::string &detail);
  void update_lease_fault_();
  void drop_housekeeping_();
  void report_extra_taps_(const std::vector<esphome::tigo_monitor::master::Bytes> &macs);

  Sink on_rx_, on_tx_;
  uint16_t gateway_;
  bool bench_, startup_;
  bool housekeeping_on_;
  std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned_;
  // Made each time polling starts, so a TAP brought up again gets the
  // 0x0B00 straight away, as the CCA's bring-up sends it.
  std::unique_ptr<esphome::tigo_monitor::master::Housekeeping> housekeeping_;
  bool readdressed_ = false;     // the last bring-up moved the TAP off the factory address
  bool session_reset_sent_ = false;  // the 0x0B00 00, sent once per start, as after a CCA reboot
  uint32_t sent_41_before_ = 0, sent_session_before_ = 0;  // from earlier Housekeepings
  Phase phase_;
  std::unique_ptr<esphome::tigo_monitor::master::Discovery> discovery_;
  int fd_ = -1;
  std::string device_;  // reopened every REOPEN_EVERY_MS while closed, as listening does
  int baud_ = 38400;
  bool tried_open_ = false;
  uint32_t last_open_try_ms_ = 0;
  esphome::tigo_monitor::master::LeaseKeeper lease_;
  esphome::tigo_monitor::master::PollCursor cursor_{0};
  uint8_t seq_ = 0;
  uint32_t timeouts_in_a_row_ = 0;
  uint32_t replies_logged_ = 0;
  bool version_asked_ = false;          // bench mode asks the TAP's version first
  std::set<uint16_t> refusals_logged_;  // commands the TAP NAKed (0x0006), logged once each
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
  mutable std::mutex mu_;
  MasterStatus status_;
};

}  // namespace tigod
