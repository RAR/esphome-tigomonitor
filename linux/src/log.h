#pragma once

// Logging to stderr, which systemd sends to the journal. The <N> prefixes are
// sd-daemon priority levels, so `journalctl -p warning` works. The last lines
// are also kept in memory for the web UI's Logs page.

#include <cstdint>
#include <string>
#include <vector>

namespace tigod {
extern bool g_verbose;

struct LogLine {
  uint64_t seq;      // counts every line since start; never reused
  int64_t unix_ms;
  int prio;          // 3 error, 4 warning, 6 info, 7 debug
  std::string msg;
};

void log_line(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// The kept lines with seq >= after, oldest first, and the seq the next line
// will get.
std::vector<LogLine> log_since(uint64_t after, uint64_t *next);
}  // namespace tigod

#define LOG_AT(prio, fmt, ...) ::tigod::log_line(prio, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_AT(3, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_AT(4, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_AT(6, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...)                                  \
  do {                                                  \
    if (::tigod::g_verbose) LOG_AT(7, fmt, ##__VA_ARGS__); \
  } while (0)
