#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace tigod {

// Reads a serial port (8N1, raw) and hands every chunk to on_data. Survives the
// dongle being unplugged: it reopens the port every few seconds until it
// reappears. Blocks until stop is set.
void run_serial(const std::string &device, int baud, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data);

// Plays a capture file back at roughly bus speed (38400 baud is ~3.8 kB/s), or
// as fast as possible with speed 0. Returns when the file ends or stop is set.
void run_replay(const std::string &path, double speed, const std::atomic<bool> &stop,
                const std::function<void(const char *, size_t)> &on_data);

}  // namespace tigod
