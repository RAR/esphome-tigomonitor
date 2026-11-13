// Which optimizers a TAP leans on for the far part of the array: the pool
// that the 0x41 PGW streams 01 and 02 rotate through, after the TAP itself.
//
// Our own rule, not the CCA's. The CCA scores every candidate by how many
// panels surround it on its layout and takes those over a threshold in
// its own list order (owner's analysis of its firmware, 2026-11-12); on
// the owner's roof that picked String B, the face farthest from the TAP,
// all day. Here that is the rule itself:
//   1. named: pgw_panels from the settings, by panel name or address;
//   2. roof: of the optimizers reporting through this TAP and placed on
//      the roof (roof.h), the farthest from this TAP's marker; panels
//      around one (all eight directions) only break an exact tie. Not
//      neighbours first: on an A-frame roof the two faces are drawn with a
//      gap for the ridge, and a dense block's corner would beat the far
//      face (the owner's roof, 2026-11-12);
//   3. loss: no roof to go by, the worst loss so far today.
#pragma once

#include "roof.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

static const size_t PGW_POOL = 4;

struct PgwMember {
  uint16_t tuid;     // its short address
  std::string name;  // its panel name, "" if none
  double loss;       // reports lost over the last day, 0-1; below 0 unknown
};

inline std::string pgw_upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// What the monitor knows of one panel, for pgw_members.
struct PgwSeen {
  uint16_t tuid;
  std::string name;
  uint16_t tap;     // the TAP it was last heard through; 0 not since the restart
  bool lately;      // heard in the last day
  uint32_t reports;  // today
  uint32_t lost;     // today
};

// The optimizers reporting through tap: those heard through it lately.
// With no other TAP heard lately, also every panel not heard since a
// restart (tap 0): a restart forgets who was heard through what, and at
// night nothing reports, so without them the pool would be chosen from the
// few heard since and wander until sunrise.
inline std::vector<PgwMember> pgw_members(const std::vector<PgwSeen> &seen, uint16_t tap) {
  bool others = false;
  for (const auto &s : seen)
    if (s.tap && s.tap != tap && s.lately) others = true;
  std::vector<PgwMember> out;
  for (const auto &s : seen) {
    bool mine = s.tap == tap ? s.lately : (!s.tap && !others);
    if (!mine) continue;
    uint32_t all = s.reports + s.lost;
    out.push_back({s.tuid, s.name, all >= 100 ? static_cast<double>(s.lost) / all : -1.0});
  }
  return out;
}

// The pool for TAP tap_id (4 hex digits) from members, the optimizers
// reporting through it; why says which rule chose it: "named", "roof",
// "loss" or "none".
inline std::vector<uint16_t> pgw_pool(const Roof &roof, const std::string &tap_id, const std::vector<PgwMember> &members,
                                      const std::vector<std::string> &named, std::string &why) {
  std::vector<uint16_t> out;
  // 1. Named, in the order given.
  if (!named.empty()) {
    for (const std::string &want : named) {
      std::string w = pgw_upper(want);
      for (const auto &m : members) {
        char addr[5];
        std::snprintf(addr, sizeof(addr), "%04X", m.tuid);
        if ((w == pgw_upper(m.name) || w == addr) && std::find(out.begin(), out.end(), m.tuid) == out.end()) {
          out.push_back(m.tuid);
          break;
        }
      }
      if (out.size() == PGW_POOL) break;
    }
    if (!out.empty()) {
      why = "named";
      return out;
    }
  }
  // 2. The roof.
  const RoofTap *tap = nullptr;
  for (const auto &t : roof.taps)
    if (pgw_upper(t.id) == pgw_upper(tap_id)) tap = &t;
  if (tap) {
    std::map<std::string, const RoofPanel *> at;
    std::set<std::pair<int, int>> cells;
    for (const auto &p : roof.panels) {
      at[pgw_upper(p.addr)] = &p;
      cells.insert({p.x, p.y});
    }
    struct Ranked {
      uint16_t tuid;
      double dist;
      int around;
    };
    std::vector<Ranked> placed;
    for (const auto &m : members) {
      char addr[5];
      std::snprintf(addr, sizeof(addr), "%04X", m.tuid);
      auto it = at.find(addr);
      if (it == at.end()) continue;
      const RoofPanel &p = *it->second;
      int around = 0;
      for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
          if ((dx || dy) && cells.count({p.x + dx, p.y + dy})) around++;
      placed.push_back({m.tuid, std::hypot(p.x - tap->x, p.y - tap->y), around});
    }
    if (placed.size() >= PGW_POOL) {
      std::sort(placed.begin(), placed.end(), [](const Ranked &a, const Ranked &b) {
        if (a.dist != b.dist) return a.dist > b.dist;
        if (a.around != b.around) return a.around > b.around;
        return a.tuid < b.tuid;
      });
      for (size_t i = 0; i < PGW_POOL; i++) out.push_back(placed[i].tuid);
      why = "roof";
      return out;
    }
  }
  // 3. The worst loss.
  std::vector<PgwMember> known;
  for (const auto &m : members)
    if (m.loss >= 0) known.push_back(m);
  std::sort(known.begin(), known.end(), [](const PgwMember &a, const PgwMember &b) {
    return a.loss != b.loss ? a.loss > b.loss : a.tuid < b.tuid;
  });
  for (size_t i = 0; i < known.size() && i < PGW_POOL; i++) out.push_back(known[i].tuid);
  why = out.empty() ? "none" : "loss";
  return out;
}
