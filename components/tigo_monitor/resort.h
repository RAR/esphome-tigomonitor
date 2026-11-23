#pragma once

// The re-sort: every TAP gets a new network, so every optimizer looks for a
// TAP again and joins the one it hears that lists it
// (docs/superpowers/specs/2026-11-23-tap-resort-design.md).
//
// An optimizer stays on its TAP while that TAP's network (PAN and key) stays
// the same; taking it off the TAP's list doesn't move it (the analyst,
// 2026-11-22). The CCA's 11-15 rediscovery, in the owner's capture (keys
// and PANs read as labels only): PV off to both TAPs; a GatewayCfg write to
// each, its channel kept, a new PAN each and one new key for both; PV on, a
// second later; the stored reads; 0xE1 to each naming the other's new
// network; a 2B erase of each table and every optimizer listed (0x8000) on
// both; windows ending 01 01. All 40 rejoined in about 12 minutes, each at
// its own address. When the window closed it trimmed each TAP to the
// optimizers that had joined it; those that hadn't stayed listed on both.
//
// Pi only: the ESP32 sends no 0xE1, so it never re-sorts.

#include "table_edit.h"
#include "tap_config.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

// One TAP as it runs before the re-sort: its last-read radio settings, and
// whether it saves only with the gateway Store (a G8).
struct ResortTap {
  uint16_t tap = 0;
  RadioBlock now;
  bool saves_with_store = false;
};

// The blocks each TAP gets, in taps' order: its own block kept whole but for
// the PAN and the key. Each PAN is pan_for() from two drawn bytes, past every
// TAP's current PAN and every one already picked; one key, drawn once, for
// all. Empty if some TAP finds no free PAN.
template<typename Draw> std::vector<RadioBlock> resort_networks(const std::vector<ResortTap> &taps, Draw draw) {
  std::set<uint16_t> in_use;
  for (const auto &t : taps) in_use.insert(t.now.pan);
  std::array<uint8_t, 16> key = key_from(draw);
  std::vector<RadioBlock> out;
  for (const auto &t : taps) {
    Bytes seed = {draw(), draw()};
    uint16_t pan = pan_for(seed, in_use);
    if (!pan) return {};
    in_use.insert(pan);
    RadioBlock b = t.now;
    b.pan = pan;
    b.key = key;
    out.push_back(b);
  }
  return out;
}

inline std::string resort_hex4_(uint16_t v) {
  char b[5];
  std::snprintf(b, sizeof(b), "%04X", v);
  return b;
}

// Every optimizer on any TAP's table, once each, listed (0x8000) at its own
// id, in id order. False, saying why, if two TAPs list one id under
// different MACs, if one MAC is listed at two ids, or if nothing is listed.
inline bool resort_list(const std::map<uint16_t, std::vector<TableRecord>> &tables, std::vector<TableRecord> &out,
                        std::string &why) {
  out.clear();
  std::map<uint16_t, std::pair<Bytes, uint16_t>> by_id;  // id -> (MAC, the TAP that listed it first)
  std::map<Bytes, std::pair<uint16_t, uint16_t>> by_mac;  // MAC -> (id, TAP)
  for (const auto &kv : tables) {
    for (const auto &r : kv.second) {
      auto i = by_id.find(r.id());
      if (i != by_id.end() && i->second.first != r.mac) {
        why = "TAP " + resort_hex4_(i->second.second) + " lists " + resort_hex4_(r.id()) + " as " +
              mac_hex(i->second.first) + " and TAP " + resort_hex4_(kv.first) + " as " + mac_hex(r.mac);
        return false;
      }
      auto m = by_mac.find(r.mac);
      if (m != by_mac.end() && m->second.first != r.id()) {
        why = mac_hex(r.mac) + " is listed at " + resort_hex4_(m->second.first) + " on TAP " +
              resort_hex4_(m->second.second) + " and at " + resort_hex4_(r.id()) + " on TAP " + resort_hex4_(kv.first);
        return false;
      }
      if (i == by_id.end()) by_id[r.id()] = {r.mac, kv.first};
      if (m == by_mac.end()) by_mac[r.mac] = {r.id(), kv.first};
    }
  }
  if (by_id.empty()) {
    why = "no TAP lists any optimizer";
    return false;
  }
  for (const auto &kv : by_id) out.push_back({kv.second.first, static_cast<uint16_t>(0x8000 | kv.first)});
  why.clear();
  return true;
}

// For each TAP, the MACs it lists that another TAP lists as joined: what a
// trim takes off it. A MAC joined on two TAPs is taken off neither.
inline std::map<uint16_t, std::vector<Bytes>> resort_trim(const std::map<uint16_t, std::vector<TableRecord>> &tables) {
  std::map<Bytes, std::vector<uint16_t>> joined_on;
  for (const auto &kv : tables)
    for (const auto &r : kv.second)
      if (r.joined()) joined_on[r.mac].push_back(kv.first);
  std::map<uint16_t, std::vector<Bytes>> out;
  for (const auto &kv : tables) {
    out[kv.first];
    for (const auto &r : kv.second) {
      auto j = joined_on.find(r.mac);
      if (j == joined_on.end() || j->second.size() != 1 || j->second[0] == kv.first) continue;
      out[kv.first].push_back(r.mac);
    }
  }
  return out;
}

// The waited-for MACs joined on no TAP, in waited's order, each with the id
// from the first table that lists it (0 if none does).
inline std::vector<TableRecord> resort_missing(const std::map<uint16_t, std::vector<TableRecord>> &tables,
                                               const std::vector<Bytes> &waited) {
  std::vector<TableRecord> out;
  for (const auto &m : waited) {
    bool joined = false;
    const TableRecord *listed = nullptr;
    for (const auto &kv : tables) {
      const TableRecord *r = TableWalk::find(kv.second, m);
      if (!r) continue;
      if (r->joined()) joined = true;
      if (!listed) listed = r;
    }
    if (!joined) out.push_back(listed ? *listed : TableRecord{m, 0});
  }
  return out;
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
