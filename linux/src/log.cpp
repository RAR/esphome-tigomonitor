#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace tigod {

// The Pi on a 44-panel roof logs about 830 lines a day, so 2,000 is over two
// days. Debug lines (--verbose) come several a second and get their own
// buffer, so turning them on can't push the rest out.
static constexpr size_t KEEP = 2000;
static constexpr size_t KEEP_DEBUG = 500;

static std::mutex mu;
static std::deque<LogLine> kept, kept_debug;
static uint64_t next_seq = 1;

void log_line(int prio, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
  std::lock_guard<std::mutex> lock(mu);
  // One write per line, under the lock, so lines from two threads never mix.
  std::fprintf(stderr, "<%d>%s\n", prio, buf);
  std::deque<LogLine> &q = prio >= 7 ? kept_debug : kept;
  q.push_back({next_seq++, now, prio, buf});
  if (q.size() > (prio >= 7 ? KEEP_DEBUG : KEEP)) q.pop_front();
}

std::vector<LogLine> log_since(uint64_t after, uint64_t *next) {
  std::lock_guard<std::mutex> lock(mu);
  std::vector<LogLine> out;
  for (const LogLine &l : kept)
    if (l.seq >= after) out.push_back(l);
  for (const LogLine &l : kept_debug)
    if (l.seq >= after) out.push_back(l);
  std::sort(out.begin(), out.end(), [](const LogLine &a, const LogLine &b) { return a.seq < b.seq; });
  *next = next_seq;
  return out;
}

}  // namespace tigod
