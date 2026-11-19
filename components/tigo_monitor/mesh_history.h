#pragma once
// What both builds keep about the mesh over days: each optimizer's reports
// and losses per day, the joins it made and through whom, and whether an
// optimizer's settings answer has really changed. No I/O: the Pi saves them
// in its state, the ESP32 on its history's flash. tigo_alloc.h's containers,
// so on the ESP32 they're in PSRAM.
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "tigo_alloc.h"

namespace esphome {
namespace tigo_monitor {
namespace mesh_history {

using master::Map;
using master::Str;
using master::Vec;

// Reports and losses by day ("YYYY-MM-DD", local) and optimizer address.
// A day's counts are noted as they grow, so the last noted are the day's.
class LossBook {
 public:
  using Counts = std::pair<uint32_t, uint32_t>;  // reports, lost
  void note(const Str &date, const Str &addr, uint32_t reports, uint32_t lost) {
    Counts &c = days_[date][addr];
    // A restart can start the day's counts again; keep the larger.
    if (reports >= c.first) c = {reports, lost};
  }
  void prune(size_t keep) {
    while (days_.size() > keep) days_.erase(days_.begin());
  }
  void restore(const Str &date, const Str &addr, uint32_t reports, uint32_t lost) {
    days_[date][addr] = {reports, lost};
  }
  const Map<Str, Map<Str, Counts>> &days() const { return days_; }

 private:
  Map<Str, Map<Str, Counts>> days_;
};

// One NetDiag join, as kept: who, when, its parent's NWID and its own.
struct JoinEntry {
  Str addr;
  int64_t unix_s = 0;
  Str parent;
  Str nwid;
};

// A join's relay, by address: the optimizer that joined under the parent's
// NWID nearest in time, within a day either side (NWIDs are handed out
// again later). Empty for the TAP (0001), its own NWID, or a NWID no join
// in the log holds.
inline Str resolve_relay(const Vec<JoinEntry> &log, size_t i) {
  const JoinEntry &e = log[i];
  Str out;
  if (e.parent.empty() || e.parent == "0001" || e.parent == e.nwid) return out;
  int64_t best = 86400 + 1;
  for (const JoinEntry &f : log) {
    if (f.addr == e.addr || f.nwid != e.parent) continue;
    int64_t d = std::llabs(f.unix_s - e.unix_s);
    if (d < best) {
      best = d;
      out = f.addr;
    }
  }
  return out;
}

// Each join's relay, as resolve_relay has it.
inline Vec<Str> resolve_relays(const Vec<JoinEntry> &log) {
  Vec<Str> out(log.size());
  for (size_t i = 0; i < log.size(); i++) out[i] = resolve_relay(log, i);
  return out;
}

inline std::vector<std::string> tokens(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// The settings answer (the tokens after "!Smrt") in the H9's placeholder
// form: zeros in every whole field.
inline bool placeholder(const std::string &settings) {
  auto t = tokens(settings);
  if (t.size() < 11) return false;
  for (size_t i : {0, 1, 2, 3, 4, 5, 6, 7, 8, 10})
    if (t[i].find_first_not_of('0') != std::string::npos) return false;
  return true;
}

// Whether an optimizer's settings changed between two answers: a whole
// field, never the stored flag (index 9) or a K8's cut-off tail, and never
// between two placeholders. The first answer heard is no change.
inline bool settings_changed(const std::string &before, const std::string &after) {
  if (before.empty() || after.empty()) return false;
  if (placeholder(before) && placeholder(after)) return false;
  auto a = tokens(before), b = tokens(after);
  if (a.size() < 11 || b.size() < 11) return false;
  for (size_t i : {0, 1, 2, 3, 4, 5, 6, 7, 8, 10})
    if (a[i] != b[i]) return true;
  // A whole answer (15 tokens) carries four more whole fields.
  if (a.size() >= 15 && b.size() >= 15)
    for (size_t i = 11; i < 15; i++)
      if (a[i] != b[i]) return true;
  return false;
}

}  // namespace mesh_history
}  // namespace tigo_monitor
}  // namespace esphome
