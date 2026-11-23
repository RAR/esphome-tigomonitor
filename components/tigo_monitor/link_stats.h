#pragma once

// Each optimizer's link to the bus: which TAP it reports through, how many of
// its reports went missing today, and its RSSI over the last day.
//
// Pure and header-only, like tigo_protocol.h, so it builds on the host
// (tests/host/link_stats_test.cpp). Both builds feed it; its containers are
// tigo_alloc.h's, so on the ESP32 what it keeps (~2.3 KB an optimizer, for a
// day of RSSI) is in PSRAM.
//
// Loss is counted from the slot counter, not a sequence number: a power report
// carries none (no byte steps by one from report to report in the 2026-11-06
// bench captures). The slot counter is 2 bytes, Hslot (60 s) in the top 2 bits
// and 5 ms ticks in the low 14, wrapping every 4 minutes. Reports land on
// 400-tick (2 s) bins: on the bench a gap of 400 was the norm, 800/1200/1600
// meant 1/2/3 bins missed, and 0 a duplicate.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tigo_alloc.h"

namespace esphome {
namespace tigo_monitor {
namespace link {

using master::Map;
using master::Str;
using master::Vec;

static const uint32_t WRAP_TICKS = 48000;  // 4 Hslots of 12,000 ticks
static const uint32_t BIN_TICKS = 400;     // one report bin, 2 s
static const int64_t BUCKET_S = 300;       // RSSI averaged over 5 minutes
static const size_t BUCKETS = 288;         // a day of them
// A gap at least this long in wall-clock time isn't counted: the counter may
// have wrapped, and an optimizer that stopped at dusk lost nothing.
static const int64_t LONG_GAP_S = 200;
// A report behind the newest by under half the wrap, heard this soon after the
// last one, is a late one: a silence that long would take 2 minutes. On the
// roof they arrive up to 3,000 ticks (15 s) behind.
static const int64_t LATE_WITHIN_S = 60;
// A parent heard no later than this before its child joined through it
// counts as present then: it reports every few seconds while producing.
static const int64_t RELAY_STALE_S = 600;

inline uint32_t slot_ticks(uint16_t slot) { return (slot >> 14) * 12000u + (slot & 0x3FFF); }

struct Link {
  uint16_t tap = 0;  // the RS485 id it was last heard through
  int rssi = 0;      // the raw byte, higher is better; not dBm
  int64_t last_heard_s = 0;
  int64_t last_join_s = 0;  // 0 until a NetDiag join is heard
  Str parent;       // its parent's NWID from that join, hex; 0001 is the TAP
  Str nwid;         // its own NWID from that join: what children name
  uint32_t reports_today = 0;
  uint32_t lost_today = 0;

  // Internal.
  struct Bucket {
    int32_t index = -1;  // unix_s / BUCKET_S
    uint16_t sum = 0;    // at most 255 reports of 255
    uint8_t n = 0;
  };
  int day = 0;
  bool have_slot = false;
  uint32_t last_ticks = 0;
  uint32_t cadence = 0;  // 0 = not learned today
  // The last few late reports, so one heard twice fills its hole once.
  struct Late {
    uint32_t ticks = 0;
    int64_t unix_s = 0;  // 0 = empty
  };
  std::array<Late, 8> late{};
  uint8_t late_next = 0;
  std::array<Bucket, BUCKETS> ring{};
};

class LinkStats {
 public:
  // day: anything that changes at local midnight (the daemon passes YYYYMMDD).
  void on_report(const Str &addr, uint16_t tap, int rssi, uint16_t slot, int64_t unix_s, int day) {
    Link &l = links_[addr];
    roll_(l, day);
    uint32_t ticks = slot_ticks(slot);
    bool late = false;
    if (l.have_slot && unix_s - l.last_heard_s < LONG_GAP_S) {
      uint32_t gap = (ticks + WRAP_TICKS - l.last_ticks) % WRAP_TICKS;
      if (gap == 0) {  // the same report again
        l.last_heard_s = unix_s;
        return;
      }
      if (gap > WRAP_TICKS / 2 && unix_s - l.last_heard_s < LATE_WITHIN_S) {
        // Behind the newest: a report that arrived late, filling a hole
        // already counted as lost; or the same late report again.
        for (const auto &e : l.late)
          if (e.unix_s && e.ticks == ticks && unix_s - e.unix_s < LATE_WITHIN_S) {
            l.last_heard_s = unix_s;
            return;
          }
        l.late[l.late_next] = {ticks, unix_s};
        l.late_next = (l.late_next + 1) % l.late.size();
        late = true;
        if (l.lost_today) l.lost_today--;
      } else if (gap <= WRAP_TICKS / 2) {
        uint32_t step = gap < BIN_TICKS ? BIN_TICKS : gap;
        // The first gap of the day only teaches the cadence, so a loss in it
        // goes uncounted; and while the cadence is still a multiple of the
        // bin, a smaller gap counts nothing.
        if (l.cadence == 0 || step < l.cadence) l.cadence = step;
        l.lost_today += (gap + l.cadence / 2) / l.cadence - 1;
      }
      // Otherwise more than half the wrap: a silence of 2 minutes or so,
      // too long to tell from the counter alone. Not counted.
    }
    l.have_slot = true;
    if (!late) l.last_ticks = ticks;
    l.tap = tap;
    l.rssi = rssi;
    l.last_heard_s = unix_s;
    l.reports_today++;
    Link::Bucket &b = l.ring[bucket_of_(unix_s) % BUCKETS];
    int32_t index = static_cast<int32_t>(unix_s / BUCKET_S);
    if (b.index != index) b = Link::Bucket{index, 0, 0};
    if (b.n < 255 && rssi >= 0 && rssi <= 255) {
      b.sum = static_cast<uint16_t>(b.sum + rssi);
      b.n++;
    }
  }

  // Starts a new day's counts for every optimizer not heard since midnight,
  // which on_report alone would leave reading yesterday's. Call before
  // reading the counts.
  void roll(int day) {
    for (auto &e : links_) roll_(e.second, day);
  }

  void on_join(const Str &addr, int64_t unix_s, const Str &parent, const Str &nwid) {
    Link &l = links_[addr];
    l.last_join_s = unix_s;
    l.parent = parent;
    l.nwid = nwid;
  }

  // The node its parent is: the one that last joined with the parent's NWID,
  // looked up now rather than at the join, so a parent heard later still
  // resolves. Empty for the TAP, a NWID no node holds, or its own. One not
  // heard or joined since shortly before the child's join is gone (a spare
  // switched off days ago keeps its last NWID on record, and the TAP hands
  // NWIDs out again), so it is no relay.
  Str relay_of(const Str &addr) const {
    auto it = links_.find(addr);
    if (it == links_.end()) return "";
    const Link &l = it->second;
    if (l.parent.empty() || l.parent == "0001" || l.parent == l.nwid) return "";
    Str best;
    int64_t when = 0;
    // On the child's own TAP: each TAP hands NWIDs out on its own network,
    // so another TAP may have a node with the same one. A TAP not heard yet
    // (no report since the join) is no reason to rule one out.
    for (const auto &e : links_)
      if (e.first != addr && e.second.nwid == l.parent && e.second.last_join_s >= when &&
          (!l.tap || !e.second.tap || e.second.tap == l.tap) &&
          std::max(e.second.last_heard_s, e.second.last_join_s) >= l.last_join_s - RELAY_STALE_S) {
        best = e.first;
        when = e.second.last_join_s;
      }
    return best;
  }

  // An optimizer deleted from the node table is forgotten here too.
  void erase(const Str &addr) { links_.erase(addr); }
  // Drops each address the node table doesn't hold (in_table false) that
  // hasn't been heard or joined for quiet_s: what a gateway renumbering
  // leaves behind. A new optimizer stays while it's recent. How many went.
  size_t prune(const std::function<bool(const Str &)> &in_table, int64_t unix_s, int64_t quiet_s) {
    size_t n = 0;
    for (auto it = links_.begin(); it != links_.end();) {
      const Link &l = it->second;
      if (!in_table(it->first) && unix_s - std::max(l.last_heard_s, l.last_join_s) >= quiet_s) {
        it = links_.erase(it);
        n++;
      } else {
        ++it;
      }
    }
    return n;
  }

  // The last day of 5-minute RSSI averages, oldest first; -1 where nothing
  // was heard.
  Vec<int> rssi_24h(const Str &addr, int64_t unix_s) const {
    Vec<int> out(BUCKETS, -1);
    auto it = links_.find(addr);
    if (it == links_.end()) return out;
    int64_t now = unix_s / BUCKET_S;
    for (size_t i = 0; i < BUCKETS; i++) {
      int64_t index = now - static_cast<int64_t>(BUCKETS - 1 - i);
      if (index < 0) continue;
      const Link::Bucket &b = it->second.ring[index % BUCKETS];
      if (b.index == index && b.n) out[i] = (b.sum + b.n / 2) / b.n;
    }
    return out;
  }

  // The last day of the RSSI ring as text, for the daemon's state file: 4 hex
  // digits a bucket, its mean then how many reports made it, oldest first,
  // ending at the bucket holding unix_s. Empty if nothing was heard that day.
  Str rssi_dump(const Str &addr, int64_t unix_s) const {
    auto it = links_.find(addr);
    if (it == links_.end()) return "";
    static const char HEX[] = "0123456789abcdef";
    int64_t now = unix_s / BUCKET_S;
    Str out;
    bool any = false;
    for (size_t i = 0; i < BUCKETS; i++) {
      int64_t index = now - static_cast<int64_t>(BUCKETS - 1 - i);
      int mean = 0, n = 0;
      if (index >= 0) {
        const Link::Bucket &b = it->second.ring[index % BUCKETS];
        if (b.index == index && b.n) {
          n = b.n;
          mean = (b.sum + n / 2) / n;
          any = true;
        }
      }
      for (int v : {mean, n}) {
        out += HEX[v >> 4];
        out += HEX[v & 15];
      }
    }
    return any ? out : "";
  }

  // Puts back what rssi_dump wrote; `to` is the index (unix_s / BUCKET_S)
  // of its last bucket. Anything else is ignored.
  void rssi_restore(const Str &addr, int32_t to, const Str &hex) {
    if (hex.size() != BUCKETS * 4) return;
    Vec<int> v(hex.size());
    for (size_t i = 0; i < hex.size(); i++) {
      char c = hex[i];
      v[i] = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (v[i] < 0) return;
    }
    Link &l = links_[addr];
    for (size_t i = 0; i < BUCKETS; i++) {
      int32_t index = to - static_cast<int32_t>(BUCKETS - 1 - i);
      int mean = v[i * 4] * 16 + v[i * 4 + 1], n = v[i * 4 + 2] * 16 + v[i * 4 + 3];
      if (index < 0 || !n) continue;
      l.ring[index % BUCKETS] = Link::Bucket{index, static_cast<uint16_t>(mean * n), static_cast<uint8_t>(n)};
    }
  }

  const Map<Str, Link> &links() const { return links_; }

 private:
  static void roll_(Link &l, int day) {
    if (l.day == day) return;
    l.day = day;
    l.reports_today = 0;
    l.lost_today = 0;
    l.cadence = 0;
  }
  static size_t bucket_of_(int64_t unix_s) { return static_cast<size_t>(unix_s / BUCKET_S); }
  Map<Str, Link> links_;
};

}  // namespace link
}  // namespace tigo_monitor
}  // namespace esphome
