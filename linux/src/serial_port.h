#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace tigod {

// A source of this form reads the bus from a network bridge instead of a
// local port.
static const char TCP_PREFIX[] = "tcp://";

// Splits "tcp://host:port" ("tcp://[v6]:port" for IPv6). False for anything
// else, including a port outside 1-65535.
bool parse_tcp_source(const std::string &source, std::string &host, std::string &port);

// Reads a serial port (8N1, raw) and hands every chunk to on_data, or a
// network bridge for a "tcp://host:port" device. device
// "auto" tries USB adapters first (/dev/ttyUSB*, /dev/ttyACM*), then the Pi's
// UART (/dev/serial0); empty opens nothing, so a port another program owns
// is never touched before someone picks ours. Survives the dongle being
// unplugged: it reopens the port every few seconds until it reappears.
// Blocks until stop is set.
void run_serial(const std::string &device, int baud, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data);

struct SerialPortInfo {
  std::string path;         // what to save: the stable name where there is one
  std::string device;       // the /dev/tty* node it points at
  std::string description;  // what it is, when the system knows
};

// The serial ports with hardware behind them, from /dev and /sys only:
// listing opens nothing, so it cannot disturb a port in use.
std::vector<SerialPortInfo> list_serial_ports();

// Plays a capture file back at roughly bus speed (38400 baud is ~3.8 kB/s), or
// as fast as possible with speed 0. Returns when the file ends or stop is set.
void run_replay(const std::string &path, double speed, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data);

}  // namespace tigod
