#include "serial_port.h"

#include "log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <sys/socket.h>
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

static int open_port(const std::string &device, int baud, int access = O_RDONLY) {
  speed_t speed = to_speed(baud);
  if (speed == 0) {
    LOGE("unsupported baud rate %d", baud);
    return -1;
  }
  int fd = ::open(device.c_str(), access | O_NOCTTY | O_CLOEXEC);
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

int open_port_rw(const std::string &device, int baud) { return open_port(device, baud, O_RDWR); }

static std::string resolve_auto();

std::string resolve_serial_port(const std::string &device) { return device == "auto" ? resolve_auto() : device; }

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

static std::string real_path(const std::string &path) {
  char buf[PATH_MAX];
  return ::realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

static std::vector<std::string> dir_entries(const std::string &dir) {
  std::vector<std::string> names;
  if (DIR *d = ::opendir(dir.c_str())) {
    while (dirent *e = ::readdir(d))
      if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    ::closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

// "usb-FTDI_FT232R_USB_UART_A10K1234-if00-port0" -> "FTDI FT232R USB UART A10K1234"
static std::string describe_by_id(std::string name) {
  if (name.rfind("usb-", 0) == 0) name = name.substr(4);
  size_t cut = name.rfind("-if");
  if (cut != std::string::npos) name = name.substr(0, cut);
  std::replace(name.begin(), name.end(), '_', ' ');
  return name;
}

std::vector<SerialPortInfo> list_serial_ports() {
  std::vector<SerialPortInfo> out;
  std::set<std::string> seen;  // device nodes already listed
  // The Pi's UARTs, under the names the HAT guides use. serial1 is the one
  // the Pi's own Bluetooth sits on, which the CCA page needs.
  for (const auto &alias : {std::make_pair("/dev/serial0", "Raspberry Pi UART (GPIO 14/15), where an RS485 HAT is"),
                            std::make_pair("/dev/serial1", "Raspberry Pi Bluetooth's UART, not for RS485")}) {
    std::string dev = real_path(alias.first);
    if (dev.empty()) continue;
    out.push_back({alias.first, dev, alias.second});
    seen.insert(dev);
  }
  // USB adapters by their stable names, which also say what they are.
  for (const auto &name : dir_entries("/dev/serial/by-id")) {
    std::string path = "/dev/serial/by-id/" + name;
    std::string dev = real_path(path);
    if (dev.empty() || !seen.insert(dev).second) continue;
    out.push_back({path, dev, describe_by_id(name)});
  }
  // Anything else with hardware behind it. Consoles and ptys have no
  // device link; legacy PC ports with nothing fitted report type 0.
  for (const auto &name : dir_entries("/sys/class/tty")) {
    std::string sys = "/sys/class/tty/" + name;
    if (::access((sys + "/device").c_str(), F_OK) != 0) continue;
    if (name.rfind("ttyS", 0) == 0) {
      std::ifstream type(sys + "/type");
      int t = 0;
      if (!(type >> t) || t == 0) continue;
    }
    std::string dev = "/dev/" + name;
    if (::access(dev.c_str(), F_OK) != 0 || !seen.insert(dev).second) continue;
    out.push_back({dev, dev, ""});
  }
  return out;
}

bool parse_tcp_source(const std::string &source, std::string &host, std::string &port) {
  if (source.rfind(TCP_PREFIX, 0) != 0) return false;
  std::string rest = source.substr(sizeof(TCP_PREFIX) - 1);
  size_t colon = rest.rfind(':');
  if (rest.size() > 2 && rest.front() == '[') {  // [IPv6]:port
    size_t close = rest.find(']');
    if (close == std::string::npos || close + 1 != colon) return false;
    host = rest.substr(1, close - 1);
  } else {
    if (colon == std::string::npos || rest.find(':') != colon) return false;
    host = rest.substr(0, colon);
  }
  port = rest.substr(colon + 1);
  if (host.empty() || port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
    return false;
  int n = std::atoi(port.c_str());
  return n >= 1 && n <= 65535;
}

// Connects within a few seconds or gives up; -1 with errno set on failure.
static int connect_tcp(const std::string &host, const std::string &port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (gai != 0) {
    errno = gai == EAI_SYSTEM ? errno : EHOSTUNREACH;
    return -1;
  }
  int fd = -1;
  for (addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      int err = 0;
      socklen_t len = sizeof(err);
      if (::poll(&pfd, 1, 5000) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) break;
      errno = err != 0 ? err : ETIMEDOUT;
    }
    int saved = errno;
    ::close(fd);
    fd = -1;
    errno = saved;
  }
  ::freeaddrinfo(res);
  if (fd < 0) return -1;
  // The bus goes quiet at night, so silence proves nothing; keepalives find
  // a bridge that lost power or Wi-Fi within about a minute instead.
  int on = 1, idle = 30, interval = 10, count = 3;
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
  return fd;
}

// The bus bytes from a network bridge: an ESP32 running stream_server, or an
// RS485-to-Ethernet converter in TCP server mode. Only ever reads, so nothing
// reaches the bus from here.
static void run_tcp(const std::string &source, const std::atomic<bool> &stop,
                    const std::function<void(const char *, size_t)> &on_data) {
  std::string host, port;
  if (!parse_tcp_source(source, host, port)) {
    LOGE("%s is not a bridge address; expected tcp://host:port", source.c_str());
    while (!stop) sleep_unless_stopped(stop, std::chrono::seconds(1));
    return;
  }
  bool warned = false;
  char buf[4096];
  while (!stop) {
    int fd = connect_tcp(host, port);
    if (fd < 0) {
      if (!warned) LOGW("cannot reach the bridge at %s (%s); retrying every 5 s", source.c_str(), strerror(errno));
      warned = true;
      sleep_unless_stopped(stop, std::chrono::seconds(5));
      continue;
    }
    LOGI("listening to the bridge at %s", source.c_str());
    warned = false;
    while (!stop) {
      pollfd pfd{fd, POLLIN, 0};
      int r = ::poll(&pfd, 1, 500);
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) continue;
      ssize_t n = r < 0 ? -1 : ::recv(fd, buf, sizeof(buf), 0);
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (n <= 0) {
        LOGW("lost the bridge at %s (%s)", source.c_str(), n == 0 ? "it closed the connection" : strerror(errno));
        break;
      }
      on_data(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    sleep_unless_stopped(stop, std::chrono::seconds(1));
  }
}

void run_serial(const std::string &device, int baud, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data) {
  if (device.rfind(TCP_PREFIX, 0) == 0) {
    run_tcp(device, stop, on_data);
    return;
  }
  if (device.empty()) {
    LOGW("no serial port chosen yet; pick the RS485 adapter under Tools in the web UI");
    while (!stop) sleep_unless_stopped(stop, std::chrono::seconds(1));
    return;
  }
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
