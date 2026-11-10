#pragma once

// The TAP's bus master, in the CCA's place: the logic shared by
// tigomonitord (linux/src/bus_master.h) and the ESP32's tigo_master
// component. The port itself is behind Transport, so the core has no I/O of
// its own and no Linux or IDF calls.
//
// The daemon as the TAP's bus master, in the CCA's place. It sends what the
// CCA sends (tigo_master.h): 0x0148 telemetry polls, the discovery window's status read
// and renewal; with housekeeping on, the 0x41s, the hourly 0x0B00 and 0x22s
// and the node table walks; with queries on, the 0x06 dialogue with
// optimizers; and after re-addressing a TAP, the CCA's bring-up. Every byte
// it reads goes to the Monitor, so panels, history and MQTT work as they do
// when listening.
//
// One request and its reply per tick(). It has no thread or timer of its own:
// only the main loop's tick() sends anything, so a daemon that hangs stops
// renewing the TAP's discovery window. That is no fail-safe: on the roof's
// TS4-A-O optimizers the array kept producing with no master and the window
// closed, RSS on or off (measured 2026-11-03). Rapid shutdown is the RSD
// cutting the TAP's power.
//
// As a standby (bus_role standby) it only listens while another master runs
// the bus, ours or a CCA, and takes the bus over once no master has been
// heard for takeover_s. Any master it hears, it yields to: a CCA can't.

#include "../tigo_monitor/table_edit.h"
#include "../tigo_monitor/tigo_master.h"
#include "../tigo_monitor/tigo_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace esphome {
namespace tigo_master {

enum class Fault { none, no_port, no_reply, second_master, refused, discovery_refused, discovery_failed, extra_tap };
enum class Phase { running, probing, discovering };
// off: not a standby. standing_by: listening, another master runs the bus;
// taking_over: none heard for takeover_s, listening a moment longer before
// sending; active: running the bus.
enum class Standby { off, standing_by, taking_over, active };
const char *phase_name(Phase p);
const char *fault_name(Fault f);
const char *standby_name(Standby s);

// The PV switch's state (tigo_master.h's PvSwitch): what the owner asked for,
// or, heard, what another master last told the TAP. Saved by the caller, and
// passed back at the next start.
struct PvState {
  bool off = false;
  bool heard = false;
  int64_t since_unix = 0;
};

// The last change to the TAP's table the owner started (table_edit.h), for
// the Devices page: still running (active) or how it ended.
struct TableEditStatus {
  std::string kind;  // "add", "remove"
  bool active = false;
  bool finished = false;
  bool ok = false;
  bool dry_run = false;
  std::string mac;  // hex
  uint16_t id = 0;  // 0 until chosen
  int ack = -1;
  std::string step, message;
  int64_t started_unix = 0, finished_unix = 0;
  std::vector<std::string> dry_frames;  // what it would send: the sub, then the args after the seq, in hex
  // A removal stopped between its erase and its last write back, saved for
  // the next start, which needs master_table_writes on to finish it.
  bool interrupted = false;
};

// An optimizer added and not joined yet (table_edit.h's JoinWatch), and
// what became of one: joined, gone from the table, or given up on.
struct TableWaiting {
  std::string mac;  // hex
  uint16_t id = 0;
  int64_t since_unix = 0;
};
struct TableJoinOutcome {
  std::string mac;
  uint16_t id = 0;
  bool joined = false;
  std::string text;
  int64_t at_unix = 0;
};

// One TAP the master runs (docs/superpowers/specs/2026-11-10-multi-tap-design.md):
// its counts and state. MasterStatus's own fields sum the counts over every
// TAP and give the first TAP's state, so with one TAP they read as before.
struct TapStatus {
  uint16_t id = 0;
  std::string mac;      // hex, once it has answered a GET_MAC
  std::string version;  // VERSION's reply
  // running: polled. lost: stopped answering, asked again now and then.
  // removed: unanswered LOST_MISSES times, asked rarely (it keeps its id).
  std::string state = "running";
  esphome::tigo_monitor::master::DiscoveryWindow discovery;
  bool discovery_read = false;
  uint32_t renewals = 0, polls = 0, timeouts = 0, resyncs = 0;
  uint32_t tap_lost = 0, back_without_discovery = 0;
  std::vector<int64_t> recent_lost_unix;  // oldest first, at most MasterStatus::RECENT_LOST
  int64_t last_reply_unix = 0;
  uint32_t sent_41 = 0, sent_session = 0, sent_broadcast = 0, sent_walk_pages = 0, walks_done = 0;
  uint32_t sent_reads = 0, not_taken = 0, sent_writes = 0, answers = 0;
  bool pv_confirmed = false, pv_unconfirmed = false;
  int pv_last_sent = -1, pv_last_reply = -1;
};

struct MasterStatus {
  bool active = false;  // the port is open and nothing has stopped it
  Fault fault = Fault::none;
  Phase phase = Phase::running;
  esphome::tigo_monitor::master::DiscoveryWindow discovery;
  bool discovery_read = false;
  uint32_t renewals = 0;
  uint32_t polls = 0;
  uint32_t timeouts = 0;
  uint32_t resyncs = 0;  // times the poll cursor was taken from the TAP's reply
  int64_t last_reply_unix = 0;
  // Times the TAP stopped answering (10 requests in a row), the last few of
  // them, and times it came back with its discovery window gone: together, a TAP whose
  // power is being cut (the CCA does that to one it can't hear). A discovery window
  // running down to 0 between renewals is normal and not counted.
  uint32_t tap_lost = 0;
  std::vector<int64_t> recent_lost_unix;  // oldest first, at most RECENT_LOST
  static constexpr size_t RECENT_LOST = 10;
  uint32_t back_without_discovery = 0;
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
  uint32_t walks_done = 0;       // walks read to the end of the table
  bool queries = false;  // the settings
  bool writes = false;
  uint32_t sent_reads = 0;  // 0x06s
  uint32_t not_taken = 0;   // 0x06s the TAP answered 0x0146: it couldn't deliver them
  uint32_t sent_writes = 0;
  uint32_t answers = 0;     // optimizers' answers heard
  std::string tap_version;  // VERSION's reply, asked once per start
  std::string detail;
  Standby standby = Standby::off;
  uint32_t takeover_s = 0;
  int64_t master_heard_ms_ago = -1;  // standing by: since another master's last request (-1: none yet)
  uint32_t takeovers = 0;
  uint32_t yields = 0;  // times it gave the bus back to another master
  int64_t last_takeover_unix = 0;
  // False after the owner's "let it close" (request_discovery): read, not
  // renewed. Saved where set_discovery says, so a restart keeps it.
  bool discovery_renewing = true;
  bool window_wanted = true;  // what automatic discovery wants (true when it's off)
  int missing_expected = 0;   // and how many in the layout it is waiting for
  PvState pv;
  bool pv_confirmed = false;    // the TAP's last reply carried the byte
  bool pv_unconfirmed = false;  // it didn't, after PvSwitch::RETRIES
  int pv_last_sent = -1, pv_last_reply = -1;
  bool table_writes = false;  // the setting
  TableEditStatus table_edit;
  std::vector<TableWaiting> table_waiting;
  std::vector<TableJoinOutcome> table_joins;  // newest last, at most TABLE_JOINS_KEPT
  static constexpr size_t TABLE_JOINS_KEPT = 10;
  uint16_t table_tap = 0;      // the TAP the last change ran on
  std::vector<TapStatus> taps;  // in the order they are polled
};

// The serial port on the bus. The core calls it only from tick().
class Transport {
 public:
  virtual ~Transport() = default;
  // Whether there is a port to open at all (one has been chosen).
  virtual bool configured() const = 0;
  // false, with why not in error.
  virtual bool open(std::string &error) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  // The whole frame, or false with why not in error.
  virtual bool write(const uint8_t *data, size_t n, std::string &error) = 0;
  // Until the last byte written is on the wire.
  virtual void drain() = 0;
  // Up to n bytes, waiting at most timeout_ms for the first: the count, 0 on
  // a timeout, or -1 when the port failed or closed.
  virtual int read(char *buf, size_t n, int timeout_ms) = 0;
  virtual int baud() const = 0;
  virtual std::string name() const = 0;  // for the log
};

class MasterCore {
 public:
  // on_rx gets every byte read from the bus (the Monitor, and --capture);
  // on_tx every frame written (--capture and the Bus log), once the port's
  // drain returns, with how many ms after its last byte left that was: a
  // Linux UART's drain checks once a scheduler tick, so it can come back
  // after the reply is in. bench: allow the reads a TAP
  // on the bench answers (tigo_master.h's bench_only); writes are never sent
  // either way.
  using Sink = std::function<void(const char *, size_t)>;
  using SentSink = std::function<void(const char *, size_t, uint32_t late_ms)>;
  // startup: if the TAP is found at the factory address, re-address it to
  // gateway_id before polling (bus_master re-addressing writes, gated).
  // housekeeping: also send the 0x41s, with the learned content or else the
  // built-in, the hourly 0x0B00 and 0x22s, and the 0x26 walks
  // (tigo_master.h's Housekeeping). targets: the optimizers to send 0x41s
  // to, asked each minute. queries: the CCA's 0x06 reads to optimizers;
  // writes: and the two writes their answers call for (Queries).
  using Targets = std::function<std::vector<uint16_t>()>;
  // prio: 3 error, 4 warning, 6 info, as syslog.
  using Log = std::function<void(int prio, const std::string &msg)>;
  MasterCore(Transport *port, Log log, Sink on_rx, SentSink on_tx, uint16_t gateway_id, bool bench, bool startup,
             bool housekeeping = false, std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned = {},
             Targets targets = nullptr, bool queries = false, bool writes = false);
  static constexpr uint32_t TARGETS_EVERY_MS = 60000;
  ~MasterCore();

  // The TAPs to run, in polling order (the saved map); call before open().
  // Without it, the one at gateway_id. An empty MAC is one not heard yet.
  struct TapEntry {
    uint16_t id = 0;
    esphome::tigo_monitor::master::Bytes mac;
  };
  void set_taps(const std::vector<TapEntry> &taps);
  std::vector<TapEntry> taps() const;  // as now known, MACs included
  // The optimizers to send 0x41s to through one TAP, asked each minute.
  // While more than one TAP is run, each gets its own from it; with one TAP,
  // or without it, targets' (all of them).
  using TapTargets = std::function<std::vector<uint16_t>(uint16_t tap)>;
  void set_tap_targets(TapTargets t) { tap_targets_ = std::move(t); }
  // master_adopt_taps: a TAP heard at the factory address that isn't one of
  // these starts the CCA's herd (tigo_master.h's Herd), which moves every
  // TAP on the bus, gives each its saved id or a new one, and saves the list
  // (saved). Off, it is reported and left alone, as with one TAP. Needs
  // startup on; call before open().
  using TapsSaved = std::function<void(const std::vector<TapEntry> &taps)>;
  void set_adopt_taps(bool on, TapsSaved saved) {
    adopt_taps_ = on;
    taps_saved_ = std::move(saved);
  }
  // The owner's herd, to find a TAP left at some other address: at the next
  // tick. False, with why, unless adopting is on and this monitor runs the
  // bus with no change to a table running.
  bool request_herd(std::string &why);
  // With more than one TAP, one that stopped answering is asked once every
  // LOST_PROBE_MS while the others are polled, and after LOST_MISSES
  // unanswered, once every REMOVED_PROBE_MS.
  static constexpr uint32_t LOST_PROBE_MS = 30000;
  static constexpr uint32_t LOST_MISSES = 10;
  static constexpr uint32_t REMOVED_PROBE_MS = 300000;

  // Stand by rather than run the bus (bus_role standby); call before open().
  // on_listen gets the bytes read while standing by (the Monitor, and the
  // 0x41 learning); learned is asked for the 0x41s at each takeover.
  using Learned = std::function<std::vector<esphome::tigo_monitor::master::Learned41::Entry>()>;
  void set_standby(uint32_t takeover_s, Sink on_listen, Learned learned);
  static constexpr uint32_t TAKEOVER_JITTER_MS = 500;  // at most, so two standbys starting together don't tie

  // The PV state to start from (the one saved), and where to save a change;
  // call before open().
  using PvSaved = std::function<void(const PvState &)>;
  // silent: the saved state couldn't be read, so say nothing to the TAP
  // about PV until it is switched, as after a takeover that heard nothing.
  void set_pv(const PvState &state, PvSaved saved, bool silent = false);
  // The owner's off or on, sent in the next tick. False while standing by:
  // only the master running the bus may change it.
  bool request_pv(bool off);
  // The owner's discovery switch, for testing: false lets the window close
  // (no renewals; status reads go on), true renews it again. False while
  // standing by, like request_pv.
  bool request_discovery(bool renew);
  // The discovery switch as last saved, and where to save a change; call
  // before open(). Without it the window is renewed and nothing is saved.
  using DiscoverySaved = std::function<void(bool renew)>;
  void set_discovery(bool renew, DiscoverySaved saved);
  // Read the TAP's GatewayCfg once at the start (the CCA's own bring-up
  // read, 0x0D 00 00), for its channel and SLT. Off unless the owner asks;
  // call before open().
  void set_read_tap_config(bool on) { read_tap_config_ = on; }
  // Automatic discovery (discovery_policy.h): false lets the window close,
  // as the CCA does with every expected optimizer found. The owner's switch
  // (request_discovery) still wins: renewed only when both want it.
  // missing: how many expected optimizers haven't reported today, for the UI.
  void set_window_wanted(bool wanted, int missing = 0);
  // master_table_writes: lets an add or removal the owner starts send its
  // table writes and erase. Off, only a dry run can be started. Call before open().
  void set_table_writes(bool on) { table_writes_on_ = on; }
  // Where a removal saves the records it will write back, before its erase,
  // and clears them once they are back (survivors null). False: not saved,
  // so nothing is erased. resume: a list saved by a removal that stopped
  // after its erase, finished at the start (with master_table_writes on).
  // Call before open(); without it no removal can run.
  using TableSaved = std::function<bool(const esphome::tigo_monitor::master::Bytes &mac,
                                        const std::vector<esphome::tigo_monitor::master::TableRecord> *survivors)>;
  void set_table_saved(TableSaved saved, const esphome::tigo_monitor::master::Bytes &resume_mac = {},
                       const std::vector<esphome::tigo_monitor::master::TableRecord> *resume = nullptr);
  // Starts adding an optimizer to the TAP's table, at id (0: the lowest
  // free), in the next tick. false, with why, while a change is running,
  // while standing by, with PV held off or the window held closed by the
  // owner, or without master_table_writes (a dry run needs none of those
  // but the first two).
  // tap: the TAP's id (0: the first). avoid: ids listed on other TAPs, which
  // an add choosing the lowest free one skips (optimizer ids are system-wide).
  bool request_table_add(const esphome::tigo_monitor::master::Bytes &mac, uint16_t id, bool dry_run, std::string &why,
                         uint16_t tap = 0, const std::set<uint16_t> &avoid = {});
  // Starts removing one: the table erased and what stays written back. The
  // same refusals, but for the window, and nowhere to save what stays.
  bool request_table_remove(const esphome::tigo_monitor::master::Bytes &mac, bool dry_run, std::string &why,
                            uint16_t tap = 0);
  // Starts replacing one: the table erased and written back without it,
  // with the new one listed (not joined) at its address, then waited for
  // like an add. The removal's refusals, and a new MAC that isn't 8 bytes.
  // Told, from the bus thread, when a replace's new optimizer has joined:
  // the old MAC and the new, in hex, for the layout to follow. Call before open().
  using TableReplaced = std::function<void(const std::string &old_mac, const std::string &new_mac)>;
  void set_on_replaced(TableReplaced cb) { on_replaced_ = std::move(cb); }
  // Told, from the bus thread, when a removal (a retire) has taken an
  // optimizer off the TAP's list: its MAC in hex. Call before open().
  using TableRemoved = std::function<void(const std::string &mac)>;
  void set_on_removed(TableRemoved cb) { on_removed_ = std::move(cb); }
  bool request_table_replace(const esphome::tigo_monitor::master::Bytes &old_mac,
                             const esphome::tigo_monitor::master::Bytes &new_mac, bool dry_run, std::string &why,
                             uint16_t tap = 0);
  // Stops waiting for an added optimizer to join (it stays listed, and
  // joins whenever the window is next open). False if it isn't waited for.
  bool request_table_stop_waiting(const esphome::tigo_monitor::master::Bytes &mac);

  bool open();
  // Sends at most one request and reads its reply, up to REPLY_TIMEOUT_MS.
  void tick(uint32_t now_ms);  // measures the gap, then tick_()
  void close();
  MasterStatus status() const;
  // The optimizers' last !Info and !Mppt answers, by short id (queries on).
  std::map<uint16_t, esphome::tigo_monitor::master::Queries::Setup> setups() const;

  static constexpr int REPLY_TIMEOUT_MS = 50;
  static constexpr uint32_t REOPEN_EVERY_MS = 5000;
  static constexpr uint32_t LOG_FIRST_REPLIES = 20;  // poll reply headers logged at INFO, for the bench
  static constexpr uint16_t FACTORY_ID = 0x1235;     // the universal factory RS485 address
  static constexpr uint32_t NO_REPLY_FAULT_AFTER = 10;
  // Missed answers in a row at the TAP's own address before the factory
  // address is tried: one lost reply is not a TAP that forgot its id.
  static constexpr uint32_t PROBE_MISSES_BEFORE_FACTORY = 3;
  // A TAP that refuses every request (0x0006) is asked once a second, not
  // forty times: it is answering, so this isn't a TAP lost.
  static constexpr uint32_t REFUSED_RETRY_MS = 1000;
  static constexpr uint32_t RETRY_AFTER_FAIL_MS = 30000;  // a failed bring-up looks again after this

 private:
  // One TAP on the bus and everything kept for it: what with one TAP was the
  // master's own. st is under mu_; the rest is the tick thread's.
  struct Tap {
    uint16_t id = 0;
    esphome::tigo_monitor::master::Bytes mac;  // the TAP that answered at id, once one has
    TapStatus st;
    esphome::tigo_monitor::master::WindowKeeper window;
    esphome::tigo_monitor::master::PollCursor cursor{0};
    // Made each time polling starts, so a TAP brought up again gets the
    // 0x0B00 straight away, as the CCA's bring-up sends it.
    std::unique_ptr<esphome::tigo_monitor::master::Housekeeping> housekeeping;
    std::unique_ptr<esphome::tigo_monitor::master::Queries> queries;
    // Made each time polling starts, from pv_state_, so a TAP brought up
    // again hears the held state first.
    std::unique_ptr<esphome::tigo_monitor::master::PvSwitch> pv;
    // The added optimizers waited for on this TAP.
    esphome::tigo_monitor::master::JoinWatch join_watch;
    uint32_t sent_41_before = 0, sent_session_before = 0;  // from earlier Housekeepings
    uint32_t sent_broadcast_before = 0, sent_walk_pages_before = 0, walks_done_before = 0;
    uint32_t targets_at_ms = 0;
    bool targets_empty = true;
    uint32_t timeouts_in_a_row = 0;
    uint32_t refusals_in_a_row = 0;
    uint32_t refused_at_ms = 0;
    uint32_t replies_logged = 0;
    bool version_asked = false;  // the TAP's version, asked once per start
    bool lost_since_window_read = false;  // the next discovery read says whether it came back without one
    bool readdressed = false;         // the last bring-up moved it off the factory address
    bool session_reset_sent = false;  // the 0x0B00 00, sent once per start, as after a CCA reboot
    bool config_read_sent = false;    // once a master lifetime, not at each takeover
    // More than one TAP: lost (stopped answering), it is asked at most once
    // every LOST_PROBE_MS; removed after LOST_MISSES of those.
    bool lost = false;
    uint32_t lost_misses = 0;
    uint32_t next_try_ms = 0;
    uint32_t pv_gen = 0;  // the request_pv its switch has been told
  };

  // Sends one frame and reads until the gateway's reply ends or the timeout.
  // false on a timeout, a write error or a second master.
  // all: every reply heard until the timeout goes there, not just the first.
  bool transact(uint16_t dest, uint16_t cmd, const esphome::tigo_monitor::master::Bytes &payload,
                esphome::tigo_monitor::master::Frame &reply, uint32_t timeout_ms = REPLY_TIMEOUT_MS,
                std::vector<esphome::tigo_monitor::master::Frame> *all = nullptr);
  bool is_reply(uint16_t cmd, const esphome::tigo_monitor::master::Bytes &request,
                const esphome::tigo_monitor::master::Frame &f) const;
  void run_startup(uint32_t now_ms);  // probe + discovery, before steady state
  void ask_version_(Tap &t);          // VERSION (0x000A), into its status
  void stand_by_(uint32_t now_ms);    // listen for another master; take over when there is none
  void take_over_(int64_t quiet_ms);
  void yield_();
  void listen_stopped_();
  // A master's request: to a TAP, or to 0xFFFF, a broadcast. A reply's
  // address has the high bit set, and 0xFFFF has it too.
  static bool is_request_(const esphome::tigo_monitor::master::Frame &f) {
    return !(f.dest & 0x8000) || f.dest == 0xFFFF;
  }
  void set_standby_(Standby s);
  bool standing_by_() const { return standby_ == Standby::standing_by || standby_ == Standby::taking_over; }
  void set_fault(Fault f, const std::string &detail);
  void log_(int prio, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void update_discovery_fault_();
  void drop_housekeeping_(Tap &t);
  void restart_taps_();  // window, housekeeping, queries and counts back to a start, for every TAP
  void report_extra_taps_(const std::vector<esphome::tigo_monitor::master::Bytes> &macs);
  void note_stranger_(const esphome::tigo_monitor::master::Bytes &mac);
  // The TAP to serve this tick (round robin over those due), or null.
  Tap *next_tap_(uint32_t now_ms);
  Tap *tap_by_id_(uint16_t id);  // 0: the first
  Tap &first_() { return *taps_.front(); }
  bool several_() const { return taps_.size() > 1; }
  void tap_answered_(Tap &t);
  void tap_unanswered_(Tap &t, uint32_t now_ms);

  Sink on_rx_;
  SentSink on_tx_;
  uint16_t gateway_;
  bool bench_, startup_;
  bool housekeeping_on_;
  bool read_tap_config_ = false;
  std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned_;
  Targets targets_;
  TapTargets tap_targets_;
  bool queries_on_, writes_on_;
  // The TAPs, in polling order; the vector itself changes only under mu_.
  std::vector<std::unique_ptr<Tap>> taps_;
  size_t turn_ = 0;  // the next TAP's turn
  bool table_writes_on_ = false;
  std::unique_ptr<esphome::tigo_monitor::master::TableJob> table_job_;
  std::unique_ptr<esphome::tigo_monitor::master::TableJob> table_job_pending_;  // under mu_
  uint16_t table_tap_ = 0;          // the TAP table_job_ runs on
  uint16_t table_tap_pending_ = 0;  // under mu_: and table_job_pending_'s
  bool table_job_running_ = false;  // under mu_: pending or table_job_
  TableSaved table_saved_;
  // A removal cut off between its erase and its write back (the TAP
  // restarted): what stays, tried again once the TAP has answered for
  // RESUME_AFTER_MS, and every RESUME_AFTER_MS after a try that fails.
  esphome::tigo_monitor::master::Bytes resume_mac_;
  std::vector<esphome::tigo_monitor::master::TableRecord> resume_stay_;
  uint16_t resume_tap_ = 0;
  bool resume_waiting_ = false;
  uint32_t resume_at_ms_ = 0;
  static constexpr uint32_t RESUME_AFTER_MS = 60000;
  bool table_refusal_(bool dry_run, uint16_t tap, std::string &why) const;  // under mu_
  void start_table_job_(std::unique_ptr<esphome::tigo_monitor::master::TableJob> job, uint16_t tap);  // under mu_
  void sync_table_status_(bool finished_now);
  void settle_table_job_(uint32_t now_ms);
  // The owner's "stop waiting"s for the next tick (under mu_).
  std::vector<esphome::tigo_monitor::master::Bytes> stop_waiting_;
  std::map<std::string, int64_t> waiting_since_;  // by MAC hex
  std::map<std::string, std::string> replaced_from_;  // a replace's new MAC → the old, both hex, until it joins
  TableReplaced on_replaced_;
  TableRemoved on_removed_;
  void sync_join_status_(Tap &t);
  // Every TAP's queries' answers, kept across a restart of them (a
  // takeover, a TAP power cut); under mu_.
  std::map<uint16_t, esphome::tigo_monitor::master::Queries::Setup> setups_;
  PvState pv_state_;       // under mu_
  bool discovery_renewing_ = true;  // under mu_
  bool window_wanted_ = true;       // under mu_
  uint32_t pv_gen_ = 0;  // under mu_: bumped by request_pv; each TAP's switch follows it
  // Standing by: whether a 0x0B00 has been heard since it began. A takeover
  // without one leaves the switch silent (tigo_master.h's PvSwitch).
  bool pv_heard_since_standby_ = false;  // under mu_
  bool pv_silent_ = false;               // under mu_
  PvSaved pv_saved_;
  DiscoverySaved discovery_saved_;
  void sync_pv_status_(Tap &t);
  void hear_pv_(bool off);
  uint32_t hold_until_ms_ = 0;  // discovery's quiet after a RESET
  bool holding_ = false;
  void tick_(uint32_t now_ms);
  void note_pause_(uint32_t ms, int64_t now_steady_ms);
  static constexpr int64_t GAP_WINDOW_MS = 5 * 60000;
  // How far past its due a wait inside a request may run before the excess
  // counts as a pause: the process was not running (stopped, or starved).
  static constexpr int64_t OVERRUN_SLACK_MS = 20;
  int64_t last_tick_end_ms_ = -1;  // steady clock
  int64_t gap_window_start_ms_ = 0;
  uint32_t gap_window_max_ms_ = 0, gap_prev_window_max_ms_ = 0;
  Phase phase_;
  std::unique_ptr<esphome::tigo_monitor::master::Discovery> discovery_;
  std::unique_ptr<esphome::tigo_monitor::master::Herd> herd_;  // instead of discovery_, adopting
  bool adopt_taps_ = false;
  TapsSaved taps_saved_;
  bool herd_wanted_ = false;  // under mu_: request_herd's, for the next tick
  void start_herd_(const char *why);
  void herd_done_();
  uint16_t discovery_tap_ = 0;  // the TAP discovery_ brings up
  size_t probe_index_ = 0;      // probing: the TAP being looked for
  uint32_t probe_misses_ = 0;   // GET_MAC_NOWs in a row unanswered at its id
  Transport *port_;  // reopened every REOPEN_EVERY_MS while closed, as listening does
  Log log_sink_;
  bool tried_open_ = false;
  uint32_t last_open_try_ms_ = 0;
  // The TAP the request in flight went to: transact's poll cursor match.
  const Tap *in_flight_ = nullptr;
  uint8_t seq_ = 0;
  bool refused_ = false;  // the last transact() got the TAP's 0x0006
  bool not_taken_ = false;  // the last transact(), a text query, got the TAP's 0x0146
  std::set<uint16_t> not_taken_logged_;  // nodes whose queries the TAP couldn't deliver, logged once each
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

}  // namespace tigo_master
}  // namespace esphome
