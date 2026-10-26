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
  }
  return "unknown";
}

static int64_t unix_now() { return static_cast<int64_t>(time(nullptr)); }

BusMaster::BusMaster(Monitor &mon, uint16_t gateway_id, bool bench) : mon_(mon), gateway_(gateway_id), bench_(bench) {}

BusMaster::~BusMaster() { close(); }

bool BusMaster::open(const std::string &device, int baud) {
  close();
  fd_ = open_port_rw(device, baud);
  if (fd_ < 0) {
    set_fault(Fault::no_port, std::string("cannot open ") + device + ": " + strerror(errno));
    return false;
  }
  LOGI("bus master on %s for gateway %04X%s", device.c_str(), gateway_, bench_ ? " (bench mode)" : "");
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
    if (f == Fault::second_master || f == Fault::refused || f == Fault::no_port) LOGE("bus master: %s", detail.c_str());
    else if (f != Fault::none) LOGW("bus master: %s", detail.c_str());
  }
  status_.fault = f;
  status_.detail = detail;
  if (f == Fault::second_master || f == Fault::no_port) status_.active = false;
}

bool BusMaster::transact(uint16_t cmd, const Bytes &payload, Frame &reply) {
  Bytes wire = master::encode_frame(gateway_, cmd, payload);
  // What our own request looks like coming back, if the adapter hears itself.
  std::string sent(wire.begin() + 5, wire.end() - 2);
  size_t off = 0;
  while (off < wire.size()) {
    ssize_t n = ::write(fd_, wire.data() + off, wire.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      set_fault(Fault::no_port, std::string("write failed: ") + strerror(errno));
      return false;
    }
    off += static_cast<size_t>(n);
  }
  tcdrain(fd_);

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
    mon_.feed(buf, static_cast<size_t>(n));
    for (ssize_t i = 0; i < n; i++) {
      splitter_.feed(buf[i], [&](const std::string &body) {
        Frame f;
        if (!master::decode_frame(body, f)) return;
        if (f.dest == (gateway_ | 0x8000) && f.cmd == cmd + 1) {
          reply = f;
          got = true;
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

void BusMaster::tick(uint32_t now_ms) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0 || status_.fault == Fault::second_master) return;
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
    if (req.cmd == master::CMD_POLL) cursor_.on_reply(reply);
    if (lease_read) {
      if (!lease.running || lease.remaining_s == 0)
        LOGW("bus master: the TAP's run lease is out (%u nodes); renewing", lease.node_count);
      lease_.on_lease(lease, now_ms);
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (req.cmd == master::CMD_POLL) status_.polls++;
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
  if (timeouts_in_a_row_ == NO_REPLY_FAULT_AFTER)
    set_fault(Fault::no_reply, "the TAP has not answered " + std::to_string(NO_REPLY_FAULT_AFTER) + " requests in a row");
}

}  // namespace tigod
