#pragma once

// Logging to stderr, which systemd sends to the journal. The <N> prefixes are
// sd-daemon priority levels, so `journalctl -p warning` works.

#include <cstdio>

namespace tigod {
extern bool g_verbose;
}

#define LOG_AT(prio, fmt, ...) std::fprintf(stderr, "<" #prio ">" fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_AT(3, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_AT(4, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_AT(6, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...)                                  \
  do {                                                  \
    if (::tigod::g_verbose) LOG_AT(7, fmt, ##__VA_ARGS__); \
  } while (0)
