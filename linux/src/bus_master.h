#pragma once

// The daemon as the TAP's bus master, in the CCA's place. It sends what the
// CCA sends (tigo_master.h): 0x0148 telemetry polls, the run lease's check
// and renewal; with housekeeping on, the 0x41s, the hourly 0x0B00 and 0x22s
// and the node table walks; with queries on, the 0x06 dialogue with
// optimizers; and after re-addressing a TAP, the CCA's bring-up. Every byte
// it reads goes to the Monitor, so panels, history and MQTT work as they do
// when listening.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the lease and the array goes safe.
//
// As a standby (bus_role standby) it only listens while another master runs
// the bus, ours or a CCA, and takes the bus over once no master has been
// heard for takeover_s. Any master it hears, it yields to: a CCA can't.

#include "tigo_master.h"
#include "tigo_protocol.h"

#include <atomic>
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
// off: not a standby. standing_by: listening, another master runs the bus;
// taking_over: none heard for takeover_s, listening a moment longer before
// sending; active: running the bus.
enum class Standby { off, standing_by, taking_over, active };
const char *phase_name(Phase p);
const char *fault_name(Fault f);
const char *standby_name(Standby s);

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
  // Times the TAP stopped answering (10 requests in a row), the last few of
  // them, and times it came back with its lease gone: together, a TAP whose
  // power is being cut (the CCA does that to one it can't hear). A lease
  // running down to 0 between renewals is normal and not counted.
  uint32_t tap_lost = 0;
  std::vector<int64_t> recent_lost_unix;  // oldest first, at most RECENT_LOST
  static const size_t RECENT_LOST = 10;
  uint32_t back_without_lease = 0;
  // The longest the bus waited on the daemon rather than the TAP: from the
  // end of one tick to the start of the next, or inside on_rx (decoding).
  // Normally ~10 ms; anything long is the rest of the daemon holding the
  // loop up. recent covers this window and the last (5 to 10 min).
  uint32_t gap_max_ms = 0;
  int64_t gap_max_at_unix = 0;
  uint32_t gap_recent_ms = 0;
  std::vector<std::string> extra_taps;  // MACs (hex) of TAPs left at the factory address
  bool housekeeping = false;  // the setting
  uint32_t learned_41 = 0;    // 0x41s heard from the CCA (0: the built-in content)
  uint32_t sent_41 = 0;
  uint32_t sent_session = 0;  // 0x0B00s
  uint32_t sent_broadcast = 0;   // empty 0x22s
  uint32_t sent_walk_pages = 0;  // 0x26 node table pages
  bool queries = false;  // the settings
  bool writes = false;
  uint32_t sent_reads = 0;  // 0x06s
  uint32_t sent_writes = 0;
  uint32_t answers = 0;     // optimizers' answers heard
  std::string tap_version;  // VERSION's reply, from a bring-up
  std::string detail;
  Standby standby = Standby::off;
  uint32_t takeover_s = 0;
  int64_t master_heard_ms_ago = -1;  // standing by: since another master's last request (-1: none yet)
  uint32_t takeovers = 0;
  uint32_t yields = 0;  // times it gave the bus back to another master
  int64_t last_takeover_unix = 0;
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
  // housekeeping: also send the 0x41s, with the learned content or else the
  // built-in, the hourly 0x0B00 and 0x22s, and the 0x26 walks
  // (tigo_master.h's Housekeeping). targets: the optimizers to send 0x41s
  // to, asked each minute. queries: the CCA's 0x06 reads to optimizers;
  // writes: and the two writes their answers call for (Queries).
  using Targets = std::function<std::vector<uint16_t>()>;
  BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping = false,
            std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned = {}, Targets targets = nullptr,
            bool queries = false, bool writes = false);
  static const uint32_t TARGETS_EVERY_MS = 60000;
  ~BusMaster();

  // Stand by rather than run the bus (bus_role standby); call before open().
  // on_listen gets the bytes read while standing by (the Monitor, and the
  // 0x41 learning); learned is asked for the 0x41s at each takeover.
  using Learned = std::function<std::vector<esphome::tigo_monitor::master::Learned41::Entry>()>;
  void set_standby(uint32_t takeover_s, Sink on_listen, Learned learned);
  static const uint32_t TAKEOVER_JITTER_MS = 500;  // at most, so two standbys starting together don't tie

  bool open(const std::string &device, int baud);
  // Sends at most one request and reads its reply, up to REPLY_TIMEOUT_MS.
  void tick(uint32_t now_ms);  // measures the gap, then tick_()
  void close();
  MasterStatus status() const;

  static const int REPLY_TIMEOUT_MS = 50;
  static const uint32_t REOPEN_EVERY_MS = 5000;
  static const uint32_t LOG_FIRST_REPLIES = 20;  // poll reply headers logged at INFO, for the bench
  static const uint16_t FACTORY_ID = 0x1235;     // the universal factory RS485 address
  static const uint32_t NO_REPLY_FAULT_AFTER = 10;
  static const uint32_t RETRY_AFTER_FAIL_MS = 30000;  // a failed bring-up looks again after this

 private:
  // Sends one frame and reads until the gateway's reply ends or the timeout.
  // false on a timeout, a write error or a second master.
  bool transact(uint16_t dest, uint16_t cmd, const esphome::tigo_monitor::master::Bytes &payload,
                esphome::tigo_monitor::master::Frame &reply, uint32_t timeout_ms = REPLY_TIMEOUT_MS);
  bool is_reply(uint16_t cmd, const esphome::tigo_monitor::master::Bytes &request,
                const esphome::tigo_monitor::master::Frame &f) const;
  void run_startup(uint32_t now_ms);  // probe + discovery, before steady state
  void stand_by_(uint32_t now_ms);   // listen for another master; take over when there is none
  void take_over_(int64_t quiet_ms);
  void yield_();
  void set_standby_(Standby s);
  bool standing_by_() const { return standby_ == Standby::standing_by || standby_ == Standby::taking_over; }
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
  Targets targets_;
  bool queries_on_, writes_on_;
  std::unique_ptr<esphome::tigo_monitor::master::Queries> queries_;
  uint32_t hold_until_ms_ = 0;  // discovery's quiet after a RESET
  bool holding_ = false;
  uint32_t targets_at_ms_ = 0;
  bool targets_empty_ = true;
  void tick_(uint32_t now_ms);
  void note_pause_(uint32_t ms, int64_t now_steady_ms);
  static const int64_t GAP_WINDOW_MS = 5 * 60000;
  // How far past its due a wait inside a request may run before the excess
  // counts as a pause: the process was not running (stopped, or starved).
  static const int64_t OVERRUN_SLACK_MS = 20;
  int64_t last_tick_end_ms_ = -1;  // steady clock
  int64_t gap_window_start_ms_ = 0;
  uint32_t gap_window_max_ms_ = 0, gap_prev_window_max_ms_ = 0;
  bool lost_since_lease_read_ = false;  // the next lease read says whether it came back without one
  bool readdressed_ = false;     // the last bring-up moved the TAP off the factory address
  bool session_reset_sent_ = false;  // the 0x0B00 00, sent once per start, as after a CCA reboot
  uint32_t sent_41_before_ = 0, sent_session_before_ = 0;  // from earlier Housekeepings
  uint32_t sent_broadcast_before_ = 0, sent_walk_pages_before_ = 0;
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
  Standby standby_ = Standby::off;
  uint32_t takeover_ms_ = 0;
  uint32_t jitter_ms_ = 0;
  Sink on_listen_;
  Learned learned_cb_;
  std::atomic<int64_t> heard_at_ms_{-1};  // steady clock: another master's last request
  mutable std::mutex mu_;
  MasterStatus status_;
};

}  // namespace tigod
