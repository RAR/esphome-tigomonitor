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

const char *fault_name(Fault f) {
  switch (f) {
    case Fault::none: return "none";
    case Fault::no_port: return "no_port";
    case Fault::no_reply: return "no_reply";
    case Fault::second_master: return "second_master";
    case Fault::refused: return "refused";
    case Fault::lease_refused: return "lease_refused";
  }
  return "unknown";
}

static int64_t unix_now() { return static_cast<int64_t>(time(nullptr)); }

BusMaster::BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench)
    : on_rx_(std::move(on_rx)), on_tx_(std::move(on_tx)), gateway_(gateway_id), bench_(bench) {}

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
  LOGI("bus master on %s for gateway %04X%s", path.c_str(), gateway_, bench_ ? " (bench mode)" : "");
  std::lock_guard<std::mutex> lock(mu_);
  status_ = MasterStatus{};
  status_.active = true;
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

bool BusMaster::transact(uint16_t cmd, const Bytes &payload, Frame &reply) {
  master::Bytes wire = master::encode_frame(gateway_, cmd, payload);
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

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(REPLY_TIMEOUT_MS);
  bool got = false, foreign = false;
  char buf[512];
  while (!got && !foreign) {
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
        if (f.dest == (gateway_ | 0x8000) && f.cmd == cmd + 1) {
          // A late reply to the request before ours is skipped, not taken.
          if (is_reply(cmd, payload, f)) {
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
  master::Request req = master::next_request(lease_, cursor_, seq_, now_ms);
  if (!master::may_send(req.cmd, req.payload, bench_)) {
    set_fault(Fault::refused, "refused to send command " + std::to_string(req.cmd));
    return;
  }
  Frame reply;
  bool ok = transact(req.cmd, req.payload, reply);
  if (status().fault == Fault::second_master) return;

  bool lease_request = req.cmd == master::CMD_TUNNEL;
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
  if (timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER)
    set_fault(Fault::no_reply, "the TAP has not answered " + std::to_string(NO_REPLY_FAULT_AFTER) + " requests in a row");
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
