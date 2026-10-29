#include "bus_master.h"

#include "log.h"
#include "serial_port.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace tigod {

BusMaster::BusMaster(Sink on_rx, Sink on_tx, uint16_t gateway_id, bool bench, bool startup, bool housekeeping,
                     std::vector<esphome::tigo_monitor::master::Learned41::Entry> learned, Targets targets,
                     bool queries, bool writes)
    : core_(&port_, [](int prio, const std::string &msg) { log_line(prio, "%s", msg.c_str()); }, std::move(on_rx),
            std::move(on_tx), gateway_id, bench, startup, housekeeping, std::move(learned), std::move(targets),
            queries, writes) {}

bool SerialTransport::open(std::string &error) {
  close();
  if (device_.rfind(TCP_PREFIX, 0) == 0) {
    error = "the bus master needs a serial port on the bus, not a network bridge";
    return false;
  }
  path_ = resolve_serial_port(device_);
  fd_ = path_.empty() ? -1 : open_port_rw(path_, baud_);
  if (fd_ < 0) {
    error = "cannot open " + (path_.empty() ? device_ : path_) + ": " + (path_.empty() ? "no adapter found" : strerror(errno));
    return false;
  }
  return true;
}

void SerialTransport::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool SerialTransport::write(const uint8_t *data, size_t n, std::string &error) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd_, data + off, n - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      error = strerror(errno);
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

void SerialTransport::drain() { tcdrain(fd_); }

int SerialTransport::read(char *buf, size_t n, int timeout_ms) {
  pollfd pfd{fd_, POLLIN, 0};
  int r = ::poll(&pfd, 1, timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  if (r == 0) return 0;
  ssize_t got = ::read(fd_, buf, n);
  return got > 0 ? static_cast<int>(got) : -1;
}

}  // namespace tigod
