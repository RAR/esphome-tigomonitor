#include "master_core.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <random>

namespace esphome {
namespace tigo_master {

using namespace esphome::tigo_monitor;

#define LOGE(...) log_(3, __VA_ARGS__)
#define LOGW(...) log_(4, __VA_ARGS__)
#define LOGI(...) log_(6, __VA_ARGS__)
using master::Bytes;
using master::Frame;

const char *phase_name(Phase p) {
  switch (p) {
    case Phase::running: return "running";
    case Phase::probing: return "probing";
    case Phase::discovering: return "discovering";
  }
  return "unknown";
}

const char *fault_name(Fault f) {
  switch (f) {
    case Fault::none: return "none";
    case Fault::no_port: return "no_port";
    case Fault::no_reply: return "no_reply";
    case Fault::second_master: return "second_master";
    case Fault::refused: return "refused";
    case Fault::lease_refused: return "lease_refused";
    case Fault::discovery_failed: return "discovery_failed";
    case Fault::extra_tap: return "extra_tap";
  }
  return "unknown";
}

const char *standby_name(Standby s) {
  switch (s) {
    case Standby::off: return "off";
    case Standby::standing_by: return "standing_by";
    case Standby::taking_over: return "taking_over";
    case Standby::active: return "active";
  }
  return "unknown";
}

static int64_t unix_now() { return static_cast<int64_t>(time(nullptr)); }
static int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

MasterCore::MasterCore(Transport *port, Log log, Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench,
                       bool startup, bool housekeeping, std::vector<master::Learned41::Entry> learned,
                       Targets targets, bool queries, bool writes)
    : on_rx_(std::move(on_rx)), on_tx_(std::move(on_tx)), gateway_(gateway_id), bench_(bench), startup_(startup),
      housekeeping_on_(housekeeping), learned_(std::move(learned)), targets_(std::move(targets)),
      queries_on_(queries), writes_on_(queries && writes), phase_(startup ? Phase::probing : Phase::running),
      port_(port), log_sink_(std::move(log)) {}

MasterCore::~MasterCore() { close(); }

void MasterCore::log_(int prio, const char *fmt, ...) {
  if (!log_sink_) return;
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_sink_(prio, buf);
}

void MasterCore::set_standby(uint32_t takeover_s, Sink on_listen, Learned learned) {
  standby_ = Standby::standing_by;
  takeover_ms_ = takeover_s * 1000;
  on_listen_ = std::move(on_listen);
  learned_cb_ = std::move(learned);
}

bool MasterCore::open() {
  close();
  std::string error;
  if (!port_->open(error)) {
    set_fault(Fault::no_port, error);
    return false;
  }
  // The TAP may have moved on while the port was closed: read the lease
  // before renewing anything, and drop any half-read frame.
  lease_ = master::LeaseKeeper();
  splitter_ = protocol::FrameSplitter<std::string>();
  drop_housekeeping_();
  queries_.reset();
  holding_ = false;
  // Or it lost power, and came back at the factory address.
  if (startup_) phase_ = Phase::probing;
  LOGI("bus master on %s for gateway %04X%s", port_->name().c_str(), gateway_, bench_ ? " (bench mode)" : "");
  std::lock_guard<std::mutex> lock(mu_);
  status_ = MasterStatus{};
  status_.active = true;
  status_.housekeeping = housekeeping_on_;
  status_.learned_41 = static_cast<uint32_t>(learned_.size());
  status_.queries = queries_on_;
  status_.writes = writes_on_;
  // A standby listens for its full takeover time before ever sending, so
  // one that comes back finds the other already running the bus.
  if (standby_ != Standby::off) {
    standby_ = Standby::standing_by;
    status_.standby = standby_;
    status_.takeover_s = takeover_ms_ / 1000;
    heard_at_ms_ = steady_ms();
  }
  return true;
}

void MasterCore::close() {
  port_->close();
  std::lock_guard<std::mutex> lock(mu_);
  status_.active = false;
}

MasterStatus MasterCore::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  MasterStatus m = status_;
  int64_t heard = heard_at_ms_;
  if (m.standby == Standby::standing_by || m.standby == Standby::taking_over)
    m.master_heard_ms_ago = heard >= 0 ? steady_ms() - heard : -1;
  return m;
}

void MasterCore::set_fault(Fault f, const std::string &detail) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_.fault != f) {
    if (f == Fault::second_master || f == Fault::refused || f == Fault::no_port || f == Fault::lease_refused) LOGE("bus master: %s", detail.c_str());
    else if (f != Fault::none) LOGW("bus master: %s", detail.c_str());
  }
  status_.fault = f;
  status_.detail = detail;
  if (f == Fault::second_master || f == Fault::no_port) status_.active = false;
}

bool MasterCore::transact(uint16_t dest, uint16_t cmd, const Bytes &payload, Frame &reply, uint32_t timeout_ms) {
  if (standing_by_()) return false;  // a standby that just yielded sends nothing more
  master::Bytes wire = master::encode_frame(dest, cmd, payload);
  // What our own request looks like coming back, if the adapter hears itself.
  std::string sent(wire.begin() + 5, wire.end() - 2);
  std::string error;
  if (!port_->write(wire.data(), wire.size(), error)) {
    set_fault(Fault::no_port, "write failed: " + error);
    port_->close();  // reopened by tick()
    return false;
  }
  // A frame takes 10 bits a byte on the wire; past that, the drain waited
  // on the process, not the line.
  int64_t drain_from = steady_ms();
  port_->drain();
  int64_t drained = steady_ms();
  int baud = port_->baud();
  int64_t on_wire_ms = static_cast<int64_t>(wire.size()) * 10 * 1000 / (baud > 0 ? baud : 38400);
  if (drained - drain_from > on_wire_ms + OVERRUN_SLACK_MS)
    note_pause_(static_cast<uint32_t>(drained - drain_from - on_wire_ms), drained);
  if (on_tx_) on_tx_(reinterpret_cast<const char *>(wire.data()), wire.size());

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool got = false, foreign = false, refused = false;
  char buf[512];
  while (!got && !foreign && !refused) {
    int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    int64_t polled_from = steady_ms();
    int n = port_->read(buf, sizeof(buf), left);
    // A wait that ran past its timeout: the process was held up mid-request,
    // which the gap between ticks never sees.
    int64_t polled = steady_ms();
    if (polled - polled_from > left + OVERRUN_SLACK_MS)
      note_pause_(static_cast<uint32_t>(polled - polled_from - left), polled);
    if (n <= 0) break;
    if (on_rx_) {
      int64_t t0 = steady_ms();
      on_rx_(buf, static_cast<size_t>(n));
      int64_t t1 = steady_ms();
      note_pause_(static_cast<uint32_t>(t1 - t0), t1);
    }
    for (int i = 0; i < n; i++) {
      splitter_.feed(buf[i], [&](const std::string &body) {
        Frame f;
        if (!master::decode_frame(body, f)) return;
        if (f.dest == (dest | 0x8000) && f.cmd == 0x0006) {
          // The TAP's NAK. A gateway with no mesh behind it (a spare on the
          // bench) refuses the tunnel this way.
          if (refusals_logged_.insert(cmd).second) LOGW("bus master: the TAP refused command %04X (0x0006)", cmd);
          refused = true;
        } else if (f.dest == (dest | 0x8000) && f.cmd == cmd + 1) {
          // A late reply to the request before ours is skipped, not taken;
          // and so is a second answer (another TAP at the factory address).
          if (!got && is_reply(cmd, payload, f)) {
            reply = f;
            got = true;
          }
        } else if (!(f.dest & 0x8000) && body != sent) {
          foreign = true;  // a request we didn't send: another master
        }
      });
    }
  }
  if (foreign && standby_ != Standby::off) {
    yield_();
    return false;
  }
  if (foreign) {
    set_fault(Fault::second_master,
              "another master is on the bus (is the CCA still connected?); stopped transmitting");
    return false;
  }
  return got;
}

// Whether f answers this request: a tunnel reply echoes its seq; a poll
// reply, the poll's cursor (or states the TAP's whole cursor, which on_reply
// then adopts).
bool MasterCore::is_reply(uint16_t cmd, const master::Bytes &request, const Frame &f) const {
  if (cmd == master::CMD_TUNNEL) {
    master::TunnelReply t;
    return request.size() > 4 && master::parse_tunnel_reply(f.payload, t) && t.seq == request[4];
  }
  if (cmd == master::CMD_POLL) {
    master::ReplyCursor rc;
    return cursor_.matches(f) || (master::reply_cursor(f.payload, rc) && rc.full);
  }
  return true;
}

void MasterCore::tick(uint32_t now_ms) {
  int64_t start = steady_ms();
  if (last_tick_end_ms_ >= 0) note_pause_(static_cast<uint32_t>(start - last_tick_end_ms_), start);
  else gap_window_start_ms_ = start;
  tick_(now_ms);
  last_tick_end_ms_ = steady_ms();
}

// A stretch the bus waited on the daemon: between ticks, or inside on_rx_
// (the Monitor decoding, and anything it does under its lock).
void MasterCore::note_pause_(uint32_t ms, int64_t now) {
  if (now - gap_window_start_ms_ >= GAP_WINDOW_MS) {
    gap_prev_window_max_ms_ = gap_window_max_ms_;
    gap_window_max_ms_ = 0;
    gap_window_start_ms_ = now;
  }
  if (ms > gap_window_max_ms_) gap_window_max_ms_ = ms;
  std::lock_guard<std::mutex> lock(mu_);
  if (ms > status_.gap_max_ms) {
    status_.gap_max_ms = ms;
    status_.gap_max_at_unix = unix_now();
  }
  status_.gap_recent_ms = std::max(gap_window_max_ms_, gap_prev_window_max_ms_);
}

void MasterCore::tick_(uint32_t now_ms) {
  if (status().fault == Fault::second_master) return;
  if (!port_->is_open()) {
    // A port that isn't there yet (a USB adapter still enumerating) or went
    // away: try again every few seconds, as listening does.
    if (!port_->configured() || (tried_open_ && now_ms - last_open_try_ms_ < REOPEN_EVERY_MS)) return;
    tried_open_ = true;
    last_open_try_ms_ = now_ms;
    if (!open()) return;
  }
  if (bench_ && !version_asked_) {
    version_asked_ = true;
    Frame v;
    if (transact(gateway_, 0x000A, {}, v)) LOGI("bus master: TAP version: %s", std::string(v.payload.begin(), v.payload.end()).c_str());
    else LOGW("bus master: no answer to VERSION (0x000A)");
    return;
  }
  if (standing_by_()) {
    stand_by_(now_ms);
    return;
  }
  if (startup_ && phase_ != Phase::running) {
    run_startup(now_ms);
    return;
  }
  { std::lock_guard<std::mutex> lock(mu_); status_.phase = phase_; }
  // After a re-address the CCA's bring-up reads go out whatever the
  // housekeeping setting; the rest only with it.
  if ((housekeeping_on_ || readdressed_) && !housekeeping_) {
    master::HousekeepingPlan plan;
    plan.send_41 = plan.session = plan.broadcast = plan.walks = housekeeping_on_;
    plan.bring_up_reads = readdressed_;
    plan.session_reset = housekeeping_on_ && readdressed_ && !session_reset_sent_;
    session_reset_sent_ = session_reset_sent_ || plan.session_reset;
    housekeeping_ = std::make_unique<master::Housekeeping>(learned_, now_ms, plan, now_ms ^ unix_now());
    targets_at_ms_ = now_ms - TARGETS_EVERY_MS;
    if (housekeeping_on_)
      LOGI("bus master: housekeeping on: %zu learned 0x41s%s", learned_.size(),
           learned_.empty() ? " (none heard: using the built-in content)" : "");
  }
  if (queries_on_ && !queries_) {
    queries_ = std::make_unique<master::Queries>(now_ms, writes_on_);
    LOGI("bus master: queries to optimizers on%s", writes_on_ ? ", with the CCA's two writes" : "");
  }
  // Asked each minute, or each second while none have been heard yet.
  if (housekeeping_ && targets_ && now_ms - targets_at_ms_ >= (targets_empty_ ? 1000 : TARGETS_EVERY_MS)) {
    std::vector<uint16_t> t = targets_();
    targets_empty_ = t.empty();
    housekeeping_->set_targets(t);
    targets_at_ms_ = now_ms;
  }
  master::Request req = master::next_request(lease_, cursor_, seq_, now_ms, housekeeping_.get(), queries_.get());
  // Bring-up frames are allowed only straight after a re-address.
  bool bring_up = req.bring_up && readdressed_;
  if (housekeeping_) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.sent_41 = sent_41_before_ + housekeeping_->sent_41();
    status_.sent_session = sent_session_before_ + housekeeping_->sent_session();
    status_.sent_broadcast = sent_broadcast_before_ + housekeeping_->sent_broadcast();
    status_.sent_walk_pages = sent_walk_pages_before_ + housekeeping_->sent_walk_pages();
  }
  if (queries_) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.sent_reads = queries_->sent_reads();
    status_.sent_writes = queries_->sent_writes();
    status_.answers = queries_->answers();
  }
  if (!master::may_send(req.cmd, req.payload, bench_, bring_up, writes_on_)) {
    set_fault(Fault::refused, "refused to send command " + std::to_string(req.cmd));
    return;
  }
  Frame reply;
  bool ok = transact(gateway_, req.cmd, req.payload, reply);
  if (status().fault == Fault::second_master || standing_by_()) return;
  if (housekeeping_) housekeeping_->on_reply(req, ok, reply);
  if (ok && queries_ && req.cmd == master::CMD_POLL) queries_->on_poll_reply(reply.payload, now_ms);
  if (ok && req.cmd == 0x000A) {
    std::string v(reply.payload.begin(), reply.payload.end());
    for (char &c : v)
      if (c == '\r' || c == '\n') c = ' ';
    while (!v.empty() && v.back() == ' ') v.pop_back();
    LOGI("bus master: TAP version: %s", v.c_str());
    std::lock_guard<std::mutex> lock(mu_);
    status_.tap_version = v;
  }

  bool lease_request = req.cmd == master::CMD_TUNNEL && req.payload.size() > 3 &&
                       (req.payload[3] == master::SUB_LEASE_CHECK || req.payload[3] == master::SUB_LEASE_RENEW);
  if (ok) {
    timeouts_in_a_row_ = 0;
    master::Lease lease;
    master::TunnelReply tunnel;
    bool lease_read = lease_request && master::parse_tunnel_reply(reply.payload, tunnel) &&
                      master::parse_lease(tunnel, lease);
    if (req.cmd == master::CMD_POLL) {
      uint32_t before = cursor_.resyncs();
      cursor_.on_reply(reply);
      if (cursor_.resyncs() != before) LOGW("bus master: took the TAP's poll cursor, %04X", cursor_.next());
      if (replies_logged_ < LOG_FIRST_REPLIES) {
        replies_logged_++;
        std::string h;
        char b[4];
        for (size_t i = 0; i < reply.payload.size() && i < 12; i++) {
          snprintf(b, sizeof(b), "%02X ", reply.payload[i]);
          h += b;
        }
        LOGI("bus master: poll %u reply header %s(%zu bytes); cursor now %04X", static_cast<unsigned>(replies_logged_), h.c_str(),
             reply.payload.size(), cursor_.next());
      }
    }
    if (lease_read) {
      // The CCA lets it run down to 0 and renews then, so that's routine;
      // finding it gone when the TAP comes back is not.
      bool out = !lease.running || lease.remaining_s == 0;
      if (out && lost_since_lease_read_)
        LOGW("bus master: the TAP came back without its run lease (%u nodes); renewing", static_cast<unsigned>(lease.node_count));
      else if (out)
        LOGI("bus master: the TAP's run lease is up (%u nodes); renewing", static_cast<unsigned>(lease.node_count));
      lease_.on_lease(lease, now_ms);
    }
    update_lease_fault_();
    std::lock_guard<std::mutex> lock(mu_);
    if (req.cmd == master::CMD_POLL) status_.polls++;
    status_.resyncs = cursor_.resyncs();
    if (lease_read) {
      if (lost_since_lease_read_ && (!lease.running || lease.remaining_s == 0)) status_.back_without_lease++;
      lost_since_lease_read_ = false;
      status_.lease = lease;
      status_.lease_read = true;
    }
    status_.renewals = lease_.renewals();
    status_.last_reply_unix = unix_now();
    if (status_.fault == Fault::no_reply) {
      status_.fault = Fault::none;
      status_.detail.clear();
      LOGI("bus master: the TAP is answering again");
    }
    return;
  }
  if (req.cmd == master::CMD_POLL) cursor_.on_timeout();
  if (lease_request) lease_.on_no_reply(now_ms);
  timeouts_in_a_row_++;
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.timeouts++;
    status_.renewals = lease_.renewals();
    if (timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER) {
      status_.tap_lost++;
      lost_since_lease_read_ = true;
      auto &recent = status_.recent_lost_unix;
      if (recent.size() == MasterStatus::RECENT_LOST) recent.erase(recent.begin());
      recent.push_back(unix_now());
    }
  }
  update_lease_fault_();
  // A gateway with no optimizers behind it, or one not running, refuses
  // polls outright (0x0006); say that rather than "not answering".
  // A TAP that went quiet may have lost power and come back at the factory
  // address: in startup mode, go and look for it again.
  if (startup_ && timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER) {
    LOGW("bus master: the TAP stopped answering at %04X; looking for it again", gateway_);
    phase_ = Phase::probing;
    lease_ = master::LeaseKeeper();
    drop_housekeeping_();
  }
  if (timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER)
    set_fault(Fault::no_reply, refusals_logged_.count(master::CMD_POLL)
                                   ? "the TAP refuses polls (0x0006): no optimizers behind it, or it isn't running"
                                   : "the TAP has not answered " + std::to_string(NO_REPLY_FAULT_AFTER) +
                                         " requests in a row");
}

// Startup: bring a TAP up before steady polling. Probe with a read first, so
// the re-addressing writes only fire when the TAP really is at the factory
// address. If it already answers at the configured id (a warm reset, or the
// CCA removed without a power cut), skip discovery.
void MasterCore::run_startup(uint32_t now_ms) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = phase_;
  }
  // The quiet after the bring-up's RESET: nothing at all goes out.
  if (holding_ && static_cast<int32_t>(now_ms - hold_until_ms_) < 0) return;
  holding_ = false;
  Frame r;
  if (phase_ == Phase::probing) {
    readdressed_ = false;
    if (transact(gateway_, master::CMD_GET_MAC_NOW, {}, r)) {  // already addressed
      LOGI("bus master: the TAP is already at %04X; polling", gateway_);
      discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, gateway_, /*scan_only=*/true);
      phase_ = Phase::discovering;  // just the scan for another TAP, then running
      timeouts_in_a_row_ = 0;
    } else if (transact(FACTORY_ID, master::CMD_GET_MAC_NOW, {}, r)) {  // at the factory address
      LOGW("bus master: the TAP is at the factory address %04X; re-addressing it to %04X", FACTORY_ID, gateway_);
      discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, gateway_);
      readdressed_ = true;
      phase_ = Phase::discovering;
      timeouts_in_a_row_ = 0;
    } else {
      timeouts_in_a_row_++;
      if (timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER)
        set_fault(Fault::no_reply, "no TAP answered at the configured id or the factory address");
    }
    return;
  }
  // phase_ == discovering
  master::DiscoveryRequest d = discovery_->next();
  if (d.done) {
    LOGI("bus master: the TAP is now at %04X; polling", gateway_);
    phase_ = Phase::running;
    if (status().fault == Fault::discovery_failed) set_fault(Fault::none, "");
    report_extra_taps_(discovery_->extra_taps());
    return;
  }
  if (d.failed) {
    // Say so, wait, and look again: a TAP slow back from the RESET is found
    // at its id then; one still at the factory address is re-addressed.
    set_fault(Fault::discovery_failed, "re-addressing the TAP failed; it did not answer at the id it was given");
    discovery_.reset();
    phase_ = Phase::probing;
    holding_ = true;
    hold_until_ms_ = now_ms + RETRY_AFTER_FAIL_MS;
    return;
  }
  if (d.idle_ms) {
    holding_ = true;
    hold_until_ms_ = now_ms + d.idle_ms;
    return;
  }
  if (!master::may_send(d.cmd, d.payload, bench_, /*startup=*/true)) {
    set_fault(Fault::refused, "refused a discovery command " + std::to_string(d.cmd));
    return;
  }
  if (transact(d.dest, d.cmd, d.payload, r, d.wait_ms ? d.wait_ms : REPLY_TIMEOUT_MS)) discovery_->on_reply(r);
}

// Standing by: read what is on the bus, for the Monitor, and note each
// request another master sends. None for takeover_s, then a random moment
// more, and this one takes the bus over.
void MasterCore::stand_by_(uint32_t) {
  char buf[512];
  int n = port_->read(buf, sizeof(buf), REPLY_TIMEOUT_MS);
  if (n < 0) {
    set_fault(Fault::no_port, "read failed: the port closed or failed");
    port_->close();  // reopened by tick()
    return;
  }
  if (n > 0) {
    const Sink &sink = on_listen_ ? on_listen_ : on_rx_;
    if (sink) sink(buf, static_cast<size_t>(n));
    bool heard = false;
    for (int i = 0; i < n; i++) {
      splitter_.feed(buf[i], [&](const std::string &body) {
        Frame f;
        if (master::decode_frame(body, f) && !(f.dest & 0x8000)) heard = true;
      });
    }
    if (heard) {
      heard_at_ms_ = steady_ms();
      if (standby_ == Standby::taking_over) {
        LOGI("standby: a master is running the bus again; standing by");
        set_standby_(Standby::standing_by);
      }
    }
  }
  int64_t quiet = steady_ms() - heard_at_ms_;
  if (standby_ == Standby::standing_by && quiet >= takeover_ms_) {
    static std::minstd_rand rng(static_cast<uint32_t>(steady_ms()));
    jitter_ms_ = rng() % (TAKEOVER_JITTER_MS + 1);
    set_standby_(Standby::taking_over);
  }
  if (standby_ == Standby::taking_over && quiet >= static_cast<int64_t>(takeover_ms_) + jitter_ms_) take_over_(quiet);
}

// As at start, minus the open: the lease is read before anything renews
// it, the poll cursor is taken from the TAP's reply, and a TAP that lost
// power is brought up only if startup is on.
void MasterCore::take_over_(int64_t quiet_ms) {
  LOGW("standby: no master heard for %.1f s; taking over the bus", quiet_ms / 1000.0);
  lease_ = master::LeaseKeeper();
  drop_housekeeping_();
  queries_.reset();
  holding_ = false;
  readdressed_ = false;
  timeouts_in_a_row_ = 0;
  if (learned_cb_) learned_ = learned_cb_();
  phase_ = startup_ ? Phase::probing : Phase::running;
  set_standby_(Standby::active);
  std::lock_guard<std::mutex> lock(mu_);
  status_.takeovers++;
  status_.last_takeover_unix = unix_now();
  status_.learned_41 = static_cast<uint32_t>(learned_.size());
  if (status_.fault != Fault::extra_tap) {
    status_.fault = Fault::none;
    status_.detail.clear();
  }
}

// Another master's request, heard while running the bus: it wins, whoever
// it is. The other side, if it is a standby too, yields as well; their
// takeover times differ, so they don't tie again.
void MasterCore::yield_() {
  LOGW("standby: another master is on the bus; standing by");
  heard_at_ms_ = steady_ms();
  set_standby_(Standby::standing_by);
  std::lock_guard<std::mutex> lock(mu_);
  status_.yields++;
}

void MasterCore::set_standby_(Standby s) {
  standby_ = s;
  std::lock_guard<std::mutex> lock(mu_);
  status_.standby = s;
}

// Counts carry over; the next one starts again when polling does.
void MasterCore::drop_housekeeping_() {
  if (!housekeeping_) return;
  sent_41_before_ += housekeeping_->sent_41();
  sent_session_before_ += housekeeping_->sent_session();
  sent_broadcast_before_ += housekeeping_->sent_broadcast();
  sent_walk_pages_before_ += housekeeping_->sent_walk_pages();
  housekeeping_.reset();
}

// Only one TAP is supported. One the scan heard at the factory address was
// left there: not re-addressed, not polled. Say so, naming it, until a
// bring-up finds none.
void MasterCore::report_extra_taps_(const std::vector<master::Bytes> &macs) {
  std::vector<std::string> hexes;
  for (const auto &mac : macs) {
    std::string h;
    char b[3];
    for (uint8_t c : mac) {
      snprintf(b, sizeof(b), "%02x", c);
      h += b;
    }
    LOGW("bus master: another TAP answered at the factory address (MAC %s); only one TAP is supported, so it "
         "was left there and is not polled", h.c_str());
    hexes.push_back(h);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.extra_taps = hexes;
  }
  if (!hexes.empty()) {
    std::string list;
    for (const auto &h : hexes) list += (list.empty() ? "" : ", ") + h;
    set_fault(Fault::extra_tap, "another TAP is on the bus at the factory address (MAC " + list +
                                    "); only one TAP is supported, so its panels are not monitored");
  } else if (status().fault == Fault::extra_tap) {
    set_fault(Fault::none, "");
  }
}

// The TAP not taking renewals means the array will drop to safe mode:
// say so, and clear it once a reading shows time on the lease.
void MasterCore::update_lease_fault_() {
  Fault f = status().fault;
  if (lease_.stuck() && f != Fault::lease_refused && f != Fault::second_master)
    set_fault(Fault::lease_refused, "the TAP did not take " + std::to_string(master::LeaseKeeper::RENEW_LIMIT) +
                                        " lease renewals in a row; stopped renewing until it shows time on the lease");
  else if (!lease_.stuck() && f == Fault::lease_refused)
    set_fault(Fault::none, "");
}

}  // namespace tigo_master
}  // namespace esphome
