#include "bus_master.h"

#include "log.h"
#include "serial_port.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace tigod {

using namespace esphome::tigo_monitor;
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

static int64_t unix_now() { return static_cast<int64_t>(time(nullptr)); }

BusMaster::BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping,
                     std::vector<master::Learned41::Entry> learned, Targets targets, bool queries, bool writes)
    : on_rx_(std::move(on_rx)), on_tx_(std::move(on_tx)), gateway_(gateway_id), bench_(bench), startup_(startup),
      housekeeping_on_(housekeeping), learned_(std::move(learned)), targets_(std::move(targets)),
      queries_on_(queries), writes_on_(queries && writes), phase_(startup ? Phase::probing : Phase::running) {}

BusMaster::~BusMaster() { close(); }

bool BusMaster::open(const std::string &device, int baud) {
  close();
  device_ = device;
  baud_ = baud;
  if (device.rfind(TCP_PREFIX, 0) == 0) {
    set_fault(Fault::no_port, "the bus master needs a serial port on the bus, not a network bridge");
    return false;
  }
  std::string path = resolve_serial_port(device);
  fd_ = path.empty() ? -1 : open_port_rw(path, baud);
  if (fd_ < 0) {
    set_fault(Fault::no_port, std::string("cannot open ") + (path.empty() ? device : path) + ": " +
                                  (path.empty() ? "no adapter found" : strerror(errno)));
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
  LOGI("bus master on %s for gateway %04X%s", path.c_str(), gateway_, bench_ ? " (bench mode)" : "");
  std::lock_guard<std::mutex> lock(mu_);
  status_ = MasterStatus{};
  status_.active = true;
  status_.housekeeping = housekeeping_on_;
  status_.learned_41 = static_cast<uint32_t>(learned_.size());
  status_.queries = queries_on_;
  status_.writes = writes_on_;
  return true;
}

void BusMaster::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  std::lock_guard<std::mutex> lock(mu_);
  status_.active = false;
}

MasterStatus BusMaster::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

void BusMaster::set_fault(Fault f, const std::string &detail) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_.fault != f) {
    if (f == Fault::second_master || f == Fault::refused || f == Fault::no_port || f == Fault::lease_refused) LOGE("bus master: %s", detail.c_str());
    else if (f != Fault::none) LOGW("bus master: %s", detail.c_str());
  }
  status_.fault = f;
  status_.detail = detail;
  if (f == Fault::second_master || f == Fault::no_port) status_.active = false;
}

bool BusMaster::transact(uint16_t dest, uint16_t cmd, const Bytes &payload, Frame &reply, uint32_t timeout_ms) {
  master::Bytes wire = master::encode_frame(dest, cmd, payload);
  // What our own request looks like coming back, if the adapter hears itself.
  std::string sent(wire.begin() + 5, wire.end() - 2);
  size_t off = 0;
  while (off < wire.size()) {
    ssize_t n = ::write(fd_, wire.data() + off, wire.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      set_fault(Fault::no_port, std::string("write failed: ") + strerror(errno));
      ::close(fd_);
      fd_ = -1;  // reopened by tick()
      return false;
    }
    off += static_cast<size_t>(n);
  }
  tcdrain(fd_);
  if (on_tx_) on_tx_(reinterpret_cast<const char *>(wire.data()), wire.size());

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool got = false, foreign = false, refused = false;
  char buf[512];
  while (!got && !foreign && !refused) {
    int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    pollfd pfd{fd_, POLLIN, 0};
    int r = ::poll(&pfd, 1, left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n <= 0) break;
    if (on_rx_) on_rx_(buf, static_cast<size_t>(n));
    for (ssize_t i = 0; i < n; i++) {
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
bool BusMaster::is_reply(uint16_t cmd, const master::Bytes &request, const Frame &f) const {
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

void BusMaster::tick(uint32_t now_ms) {
  if (status().fault == Fault::second_master) return;
  if (fd_ < 0) {
    // A port that isn't there yet (a USB adapter still enumerating) or went
    // away: try again every few seconds, as listening does.
    if (device_.empty() || (tried_open_ && now_ms - last_open_try_ms_ < REOPEN_EVERY_MS)) return;
    tried_open_ = true;
    last_open_try_ms_ = now_ms;
    if (!open(device_, baud_)) return;
  }
  if (bench_ && !version_asked_) {
    version_asked_ = true;
    Frame v;
    if (transact(gateway_, 0x000A, {}, v)) LOGI("bus master: TAP version: %s", std::string(v.payload.begin(), v.payload.end()).c_str());
    else LOGW("bus master: no answer to VERSION (0x000A)");
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
  if (status().fault == Fault::second_master) return;
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
        LOGI("bus master: poll %u reply header %s(%zu bytes); cursor now %04X", replies_logged_, h.c_str(),
             reply.payload.size(), cursor_.next());
      }
    }
    if (lease_read) {
      if (!lease.running || lease.remaining_s == 0)
        LOGW("bus master: the TAP's run lease is out (%u nodes); renewing", lease.node_count);
      lease_.on_lease(lease, now_ms);
    }
    update_lease_fault_();
    std::lock_guard<std::mutex> lock(mu_);
    if (req.cmd == master::CMD_POLL) status_.polls++;
    status_.resyncs = cursor_.resyncs();
    if (lease_read) {
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
void BusMaster::run_startup(uint32_t now_ms) {
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

// Counts carry over; the next one starts again when polling does.
void BusMaster::drop_housekeeping_() {
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
void BusMaster::report_extra_taps_(const std::vector<master::Bytes> &macs) {
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
void BusMaster::update_lease_fault_() {
  Fault f = status().fault;
  if (lease_.stuck() && f != Fault::lease_refused && f != Fault::second_master)
    set_fault(Fault::lease_refused, "the TAP did not take " + std::to_string(master::LeaseKeeper::RENEW_LIMIT) +
                                        " lease renewals in a row; stopped renewing until it shows time on the lease");
  else if (!lease_.stuck() && f == Fault::lease_refused)
    set_fault(Fault::none, "");
}

}  // namespace tigod
