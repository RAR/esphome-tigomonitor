#pragma once

// The TAPs another master was heard running, for a monitor that takes over
// the bus with no saved TAP list (taps.json). Only this monitor's own
// bring-up (a herd) writes that list; after a CCA ran the bus, there was
// none, and the monitor ran only the TAP at gateway_id (2026-11-16, on a
// roof with two).
//
// A TAP to run is one some optimizer reported through in the last day: the
// ids a CCA's self-test borrows answer GET_MAC but carry no optimizers. Two
// ids with one MAC are one TAP re-addressed; the newer is kept.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tigod {

struct HeardTap {
  uint16_t id = 0;
  std::string mac;  // 16 hex digits, or "" if its MAC wasn't heard
};

// optimizer_taps: for each optimizer, the TAP it was last heard through and
// when (unix s); macs: each TAP id's MAC as heard. In id order.
inline std::vector<HeardTap> heard_taps(const std::vector<std::pair<uint16_t, int64_t>> &optimizer_taps,
                                        const std::map<uint16_t, std::string> &macs, int64_t now_s,
                                        int64_t within_s = 86400) {
  std::map<uint16_t, int64_t> latest;
  for (const auto &o : optimizer_taps)
    if (o.first && now_s - o.second <= within_s) latest[o.first] = std::max(latest[o.first], o.second);
  // One MAC under two ids: the id heard from last.
  std::map<std::string, uint16_t> by_mac;
  std::vector<HeardTap> out;
  for (const auto &kv : latest) {
    auto m = macs.find(kv.first);
    std::string mac = m == macs.end() ? "" : m->second;
    if (!mac.empty()) {
      auto seen = by_mac.find(mac);
      if (seen != by_mac.end()) {
        if (latest[seen->second] >= kv.second) continue;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const HeardTap &t) { return t.id == seen->second; }), out.end());
      }
      by_mac[mac] = kv.first;
    }
    out.push_back({kv.first, mac});
  }
  return out;
}

}  // namespace tigod
