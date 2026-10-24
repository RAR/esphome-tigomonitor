#include "serial_port.h"

#include "log.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace tigod {

static speed_t to_speed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return 0;
  }
}

static int open_port(const std::string &device, int baud) {
  speed_t speed = to_speed(baud);
  if (speed == 0) {
    LOGE("unsupported baud rate %d", baud);
    return -1;
  }
  int fd = ::open(device.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) return -1;
  termios tio{};
  if (tcgetattr(fd, &tio) != 0) {
    ::close(fd);
    return -1;
  }
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);
  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    ::close(fd);
    return -1;
  }
  tcflush(fd, TCIFLUSH);
  return fd;
}

static std::string resolve_auto() {
  for (const char *pattern : {"/dev/ttyUSB", "/dev/ttyACM"}) {
    for (int i = 0; i < 4; i++) {
      std::string dev = pattern + std::to_string(i);
      if (::access(dev.c_str(), F_OK) == 0) return dev;
    }
  }
  if (::access("/dev/serial0", F_OK) == 0) return "/dev/serial0";
  return "";
}

static void sleep_unless_stopped(const std::atomic<bool> &stop, std::chrono::milliseconds ms) {
  auto until = std::chrono::steady_clock::now() + ms;
  while (!stop && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void run_serial(const std::string &device, int baud, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data) {
  bool warned = false;
  char buf[4096];
  while (!stop) {
    std::string dev = device == "auto" ? resolve_auto() : device;
    int fd = dev.empty() ? -1 : open_port(dev, baud);
    if (fd < 0) {
      if (!warned) {
        if (dev.empty()) LOGW("no serial adapter found; plug in the RS485 adapter (checking every 5 s)");
        else LOGW("cannot open %s (%s); retrying every 5 s", dev.c_str(), strerror(errno));
      }
      warned = true;
      sleep_unless_stopped(stop, std::chrono::seconds(5));
      continue;
    }
    LOGI("listening on %s at %d baud", dev.c_str(), baud);
    warned = false;
    while (!stop) {
      pollfd pfd{fd, POLLIN, 0};
      int r = ::poll(&pfd, 1, 500);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        LOGW("%s went away", dev.c_str());
        break;
      }
      if (r == 0) continue;
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (n <= 0) {  // 0 after POLLIN: the device was unplugged
        LOGW("%s went away", dev.c_str());
        break;
      }
      on_data(buf, static_cast<size_t>(n));
    }
    ::close(fd);
  }
}

void run_replay(const std::string &path, double speed, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    LOGE("cannot open %s", path.c_str());
    return;
  }
  LOGI("replaying %s", path.c_str());
  constexpr size_t CHUNK = 64;  // ~17 ms of bus time at 38400 baud
  char buf[CHUNK];
  auto chunk_time = std::chrono::duration<double>(CHUNK * 10 / 38400.0 / (speed > 0 ? speed : 1));
  while (!stop && in) {
    in.read(buf, CHUNK);
    std::streamsize n = in.gcount();
    if (n <= 0) break;
    on_data(buf, static_cast<size_t>(n));
    if (speed > 0) std::this_thread::sleep_for(chunk_time);
  }
  LOGI("replay finished");
}

}  // namespace tigod
