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
#include <poll.h>
#include <set>
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

void run_serial(const std::string &device, int baud, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data) {
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
