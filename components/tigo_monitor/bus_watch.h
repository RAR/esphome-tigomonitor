#pragma once

// What is said on the bus, named, and the frames that reconfigure a TAP.
//
// The names are the CCA's own where its local UI shows them (Mesh History,
// 2026-11-06); the rest are the protocol's. A reconfiguration is a request
// classify_request() says no master here ever sends: a GatewayCfg write,
// SET_CHANNEL, the scan, TAP firmware, EraseFlash, SetTable and the rest. Our
// own master can't send one, so hearing one means another device (a CCA,
// most likely) is on the bus and changing the TAP.
//
// An event keeps the frame's payload in hex, as the rest of the device's UI
// shows addresses and barcodes, but for one thing: the GatewayCfg block (the
// write, and the TAP's reply to any GatewayCfg request) holds the network key
// the monitor shows nowhere else, so it is never kept, only its length.

#include "tigo_master.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

inline const char *cmd_name(uint16_t cmd) {
  switch (cmd) {
    case 0x0006: return "Refused";
    case 0x000A: return "VERSION";
    case 0x000B: return "VERSION reply";
    case 0x000C: return "SET_CHANNEL";
    case 0x000D: return "SET_CHANNEL reply";
    case 0x000E: return "Gateway info";
    case 0x000F: return "Gateway info reply";
    case 0x0E02: return "Gateway info";
    case 0x0E03: return "Gateway info reply";
    case 0x0010: return "RESET";
    case 0x0012: return "STORE";
    case 0x0013: return "STORE reply";
    case 0x0014: return "SET_485_ID";
    case 0x0038: return "GET_MAC";
    case 0x0039: return "GET_MAC reply";
    case 0x003A: return "GET_MAC";
    case 0x003B: return "GET_MAC reply";
    case 0x003C: return "SET_ID_BY_MAC";
    case 0x003D: return "SET_ID_BY_MAC reply";
    case 0x0054: return "SET_GATEWAY_MODE";
    case 0x0058: return "SET_NETRUN";
    case 0x0070: return "Channel scan";
    case 0x0071: return "Channel scan reply";
    case 0x0072: return "Scan results";
    case 0x0073: return "Scan results reply";
    case 0x011E: return "CLEAR_ADT";
    case 0x0146: return "Query not taken";
    case 0x0148: return "Poll";
    case 0x0149: return "Poll reply";
    case 0x0182: return "SET_LIST_ENTRIES";
    case 0x0B00: return "PV switch";
    case 0x0B01: return "PV switch reply";
    case 0x0B0F: return "Tunnel";
    case 0x0B10: return "Tunnel reply";
    default:
      if (cmd >= 0x0520 && cmd <= 0x0527) return cmd & 1 ? "TAP firmware reply" : "TAP firmware";
      return "";
  }
}

// Tunnel subs. A reply is its request's sub + 1.
inline const char *sub_name(uint8_t sub) {
  switch (sub) {
    case 0x06: return "EncapString";
    case 0x07: return "EncapString reply";
    case 0x09: return "NetDiag";
    case 0x0D: return "GatewayCfg";
    case 0x0E: return "GatewayCfg reply";
    case 0x13: return "PeriodicCfg Write";
    case 0x14: return "Report Combined";
    case 0x17: return "PeriodicCfg";
    case 0x18: return "PeriodicCfg reply";
    case 0x1B: return "Sub 0x1B";
    case 0x1D: return "ResetNode";
    case 0x22: return "SamplingRate";
    case 0x23: return "SamplingRate reply";
    case 0x26: return "GetTable";
    case 0x27: return "GetTable reply";
    case 0x29: return "SetTable";
    case 0x2A: return "SetTable reply";
    case 0x2B: return "EraseFlash";
    case 0x2C: return "EraseFlash reply";
    case 0x2D: return "DiscState Write";
    case 0x2E: return "DiscState";
    case 0x2F: return "DiscState reply";
    case 0x30: return "NetDebug";
    case 0x31: return "Power";
    case 0x41: return "SetPgw";
    case 0x42: return "SetPgw reply";
    case 0xE1: return "SetKnownNetworks";
    case 0xE5: return "EraseStackTrace";
    default:
      if (sub >= 0x40 && sub <= 0x47) return "StarDisc";
      if (sub >= 0xE2 && sub <= 0xE6) return "StackTrace";
      return "";
  }
}

// Text from the bus, made printable: CR/LF as spaces, trailing ones trimmed.
inline std::string bus_text(const uint8_t *p, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) out += p[i] == '\r' || p[i] == '\n' ? ' ' : std::isprint(p[i]) ? char(p[i]) : '.';
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// The payload as "37 24 92 66 …", but for the GatewayCfg block (a write's
// 36 bytes after its 01 00, or all of a 0x0E reply's data) and
// SetKnownNetworks' args, shown by length.
inline std::string bus_hex(const Frame &f) {
  size_t keep = f.payload.size();
  bool tunnel = (f.cmd == CMD_TUNNEL || f.cmd == CMD_TUNNEL_REPLY) && f.payload.size() >= 5;
  if (tunnel && f.cmd == CMD_TUNNEL && f.payload[3] == 0x0D && f.payload.size() > 7) keep = 7;
  if (tunnel && f.cmd == CMD_TUNNEL_REPLY && f.payload[3] == 0x0E && f.payload.size() > 5) keep = 5;
  // SetKnownNetworks: never decoded, and by its name a list of networks,
  // perhaps with their keys.
  bool known_networks = tunnel && f.cmd == CMD_TUNNEL && f.payload[3] == 0xE1 && f.payload.size() > 5;
  if (known_networks) keep = 5;
  std::string out;
  char b[4];
  for (size_t i = 0; i < keep; i++) {
    std::snprintf(b, sizeof(b), "%s%02X", i ? " " : "", f.payload[i]);
    out += b;
  }
  if (keep < f.payload.size())
    out += " + " + std::to_string(f.payload.size() - keep) + " bytes hidden (" +
           (known_networks ? "a list of networks, perhaps with keys" : "the TAP's network config and key") + ")";
  return out;
}

// The reconfiguration a request is, by name, or "" if a master here might
// send it.
// A reply goes to the TAP's id with the 0x8000 bit; 0xFFFF is a broadcast request.
inline bool is_reply(const Frame &f) { return (f.dest & 0x8000) && f.dest != 0xFFFF; }

inline std::string reconfiguration(const Frame &f) {
  if (is_reply(f)) return "";
  if (classify_request(f.cmd, f.payload) != Allow::never) return "";
  char buf[64];
  if (f.cmd == CMD_TUNNEL && f.payload.size() >= 5) {
    uint8_t sub = f.payload[3];
    if (sub == 0x0D) return "GatewayCfg write (the TAP's channel, network ID and key)";
    if (sub == SUB_ERASE_FLASH) return "EraseFlash (the optimizer table)";
    if (sub == 0x06) {
      std::string t = f.payload.size() > 7 ? bus_text(f.payload.data() + 7, f.payload.size() - 7) : "";
      return "An optimizer setting write (" + t + ")";
    }
    if (sub == 0x22) return "SamplingRate with data";
    if (*sub_name(sub)) return sub_name(sub);
  }
  if (f.cmd >= 0x0520 && f.cmd <= 0x0527) {
    std::snprintf(buf, sizeof(buf), "TAP firmware (0x%04X)", f.cmd);
    return buf;
  }
  static const uint16_t NAMED[] = {0x000C, 0x0070, 0x0072, 0x0012, 0x011E, 0x0182, 0x0058, 0x0054};
  for (uint16_t c : NAMED)
    if (f.cmd == c) return cmd_name(c);
  std::snprintf(buf, sizeof(buf), "A frame carrying the setup key (0x%04X)", f.cmd);
  for (size_t i = 0; i + 4 <= f.payload.size(); i++)
    if (f.payload[i] == 0x37 && f.payload[i + 1] == 0x24 && f.payload[i + 2] == 0x92 && f.payload[i + 3] == 0x66)
      return buf;
  std::snprintf(buf, sizeof(buf), "Command 0x%04X", f.cmd);
  return buf;
}

// One request and what came back: the unit the Bus log shows.
struct Exchange {
  uint64_t id = 0;
  int64_t unix_ms = 0;        // the request's time (an unasked reply's own)
  bool ours = false;          // sent by this monitor's master
  bool other_master = false;  // a request not ours while this monitor runs the bus
  uint16_t addr = 0;          // the TAP's RS485 id; 0xFFFF a broadcast
  uint16_t cmd = 0;
  uint8_t seq = 0;            // a tunnel request's
  int sub = -1;
  int node = -1;              // a text query's optimizer
  std::string name;           // "Tunnel", "SET_CHANNEL"
  std::string sub_name;       // "DiscState"
  std::string category;       // discovery, queries, housekeeping, bringup, reconfig, other
  // waiting, answered, delivered, not_delivered, refused, no_reply, broadcast, unasked
  std::string result = "waiting";
  bool late = false;          // answered after the next request went out
  int rt_ms = -1;
  std::string summary;
  std::string request_hex, reply_hex;
  std::string reconfig;
  std::string answer;         // an optimizer's text answer to a query, from a later poll
  int answer_after_ms = -1;
};

// A firmware version heard on the bus: an optimizer's answer to ^00Version
// ("Mnode Version K8.0120 (2D)"), or a TAP's to VERSION ("Mgate Version G8").
struct VersionSeen {
  bool tap = false;
  uint16_t addr = 0;     // the optimizer's node id, or the TAP's RS485 id
  std::string model;     // the word before "Version": Mnode, Mgate
  std::string version;   // the rest
};

// "<model> Version <version>", or false.
inline bool parse_version(const std::string &text, VersionSeen &v) {
  size_t at = text.find(" Version ");
  if (at == 0 || at == std::string::npos || text.find(' ') < at) return false;
  std::string rest = text.substr(at + 9);
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
  if (rest.empty()) return false;
  v.model = text.substr(0, at);
  v.version = rest;
  return true;
}

// Each device's version, from when it was first seen, and the one it
// replaced: kept by the caller across restarts (the Pi's versions.json).
// The TAP's radio settings, from the 37-byte GatewayCfg block a read returns
// (docs/cca-tap-management.md): byte 1 the 802.15.4 channel, 14 SLT (the
// signal a node must beat to be admitted), 35 SLm. The same block holds the
// PAN and the 16-byte network key; those are never copied out.
struct TapRadio {
  uint16_t tap = 0;
  int channel = 0, slt = 0, slm = 0;
};

// req and rep: a tunnel request and the reply paired with it. Only a read
// (args 00 00 or 00 01) counts: a write (01 00 + the block) is echoed back
// with the same block, and isn't the TAP's own setting until it's read.
inline bool parse_gateway_cfg(const Frame &req, const Frame &rep, TapRadio &r) {
  TunnelReply t;
  if (req.cmd != CMD_TUNNEL || req.payload.size() < 7 || req.payload[3] != 0x0D || req.payload[5] != 0x00) return false;
  if (rep.cmd != CMD_TUNNEL_REPLY || !parse_tunnel_reply(rep.payload, t) || t.sub != 0x0E) return false;
  if (t.data.size() != 37 || t.data[0] != 0x00) return false;
  r.tap = rep.dest & 0x7FFF;
  r.channel = t.data[1];
  r.slt = t.data[14];
  r.slm = t.data[35];
  return true;
}

class VersionBook {
 public:
  enum Result { first, same, changed };
  struct Entry {
    bool tap = false;
    uint16_t addr = 0;
    std::string model, version, previous;
    int64_t since_ms = 0, last_ms = 0;  // seen at this version from, and last
  };
  Result record(const VersionSeen &v, int64_t now_ms) {
    Entry &e = entries_[{v.tap, v.addr}];
    e.last_ms = now_ms;
    if (e.version == v.version) return same;
    Result r = e.version.empty() ? first : changed;
    e.tap = v.tap;
    e.addr = v.addr;
    e.model = v.model;
    e.previous = e.version;
    e.version = v.version;
    e.since_ms = now_ms;
    return r;
  }
  void restore(const Entry &e) { entries_[{e.tap, e.addr}] = e; }
  const Entry *find(bool tap, uint16_t addr) const {
    auto it = entries_.find({tap, addr});
    return it == entries_.end() ? nullptr : &it->second;
  }
  const std::map<std::pair<bool, uint16_t>, Entry> &entries() const { return entries_; }

 private:
  std::map<std::pair<bool, uint16_t>, Entry> entries_;
};

// The last WINDOW_MS of the bus, for the Bus log's strip.
struct BusStats {
  uint32_t polls = 0, polls_lost = 0, polls_empty = 0, polls_refused = 0;
  std::map<std::string, uint32_t> count;  // exchanges by category
  uint32_t delivered = 0, not_delivered = 0;
  uint32_t other_master = 0, reconfig = 0;
  int rt_median_ms = -1, rt_max_ms = -1;
  std::string discovery;  // the latest DiscState read, however old
};

inline std::string exchange_category(uint16_t cmd, int sub, const std::string &reconfig) {
  if (!reconfig.empty()) return "reconfig";
  switch (sub) {
    case 0x2D: case 0x2E: return "discovery";
    case 0x06: return "queries";
    case 0x41: case 0x22: case 0x26: case 0x17: return "housekeeping";
    case 0x0D: return "bringup";
    default: break;
  }
  switch (cmd) {
    case CMD_SESSION: return "housekeeping";
    case 0x000A: case 0x000E: case 0x0E02: case CMD_RESET: case CMD_STORE: case CMD_SET_485_ID:
    case CMD_GET_MAC: case CMD_GET_MAC_NOW: case CMD_SET_ID_BY_MAC:
      return "bringup";
    default: return "other";
  }
}

inline std::string hex16(const uint8_t *p, size_t n) {
  std::string out;
  char b[3];
  for (size_t i = 0; i < n; i++) {
    std::snprintf(b, sizeof(b), "%02X", p[i]);
    out += b;
  }
  return out;
}

// "Open, 408 s left · 40 of the 44 in the table have joined"
inline std::string window_summary(const DiscoveryWindow &w) {
  bool open = w.running && w.remaining_s > 0;
  return (open ? "Open, " + std::to_string(w.remaining_s) + " s left" : std::string("Closed")) + " · " +
         std::to_string(w.joined) + " of the " + std::to_string(w.node_count) + " in the table have joined";
}

// What the request asks, before any reply.
inline std::string request_summary(const Frame &f) {
  const Bytes &p = f.payload;
  char b[64];
  if (f.cmd == CMD_TUNNEL && p.size() >= 5) {
    Bytes a(p.begin() + 5, p.end());
    switch (p[3]) {
      case 0x06:
        return a.size() > 2 ? bus_text(a.data() + 2, a.size() - 2) : "";
      case 0x2D:
        if (a.size() >= 7) {
          std::snprintf(b, sizeof(b), "Renew for %u s, %u in the table", a[3] << 8 | a[4], a[5] << 8 | a[6]);
          return b;
        }
        return "";
      case 0x22:
        return a == Bytes{0x00, 0x00} ? "To every optimizer" : a == Bytes{0x00, 0x01} ? "To the TAP" : "";
      case 0x0D:
        if (a.size() == 2 && a[0] == 0x00) return "Read the TAP's network config (part " + std::to_string(a[1]) + ")";
        return "";
      case 0x41:  // SetPgw: the node, then 00 00 00 <stream> 00 (Housekeeping in tigo_master.h)
        if (a.size() >= 6) {
          std::snprintf(b, sizeof(b), "Preferred gateway for node %04X · stream %02X", a[0] << 8 | a[1], a[5]);
          return b;
        }
        return "";
      default:
        return "";
    }
  }
  switch (f.cmd) {
    case CMD_SESSION:
      return p.size() == 1 ? (p[0] ? "PV on" : "PV off") : "";
    case CMD_RESET:
      return "Reset";
    case CMD_STORE:
      return "Store the TAP's RS485 id";
    case CMD_SET_485_ID:
      if (p.size() == 6) {
        std::snprintf(b, sizeof(b), "Every TAP to %04X", p[4] << 8 | p[5]);
        return b;
      }
      return "";
    case CMD_SET_ID_BY_MAC:
      if (p.size() == 14) {
        std::snprintf(b, sizeof(b), " to %04X", p[12] << 8 | p[13]);
        return "TAP " + hex16(p.data() + 4, 8) + b;
      }
      return "";
    default:
      return "";
  }
}

// The request's summary, finished with what came back.
inline std::string reply_summary(const Frame &req, const Frame &rep, const std::string &asked) {
  if (rep.cmd == 0x0006) return "The TAP refused it (0x0006)";
  if (rep.cmd == CMD_NOT_TAKEN)
    return (asked.empty() ? std::string() : asked + " · ") +
           "the TAP couldn't deliver it (0x0146): the optimizer may be off or rejoining";
  char b[64];
  TunnelReply t;
  if (rep.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(rep.payload, t)) {
    DiscoveryWindow w;
    if (parse_discovery_window(t, w)) return window_summary(w);
    if (t.sub == 0x27 && t.data.size() >= 4) {
      unsigned start = t.data[0] << 8 | t.data[1], n = t.data[2] << 8 | t.data[3];
      if (!n) std::snprintf(b, sizeof(b), "The end of the table, from index %04X", start);
      else std::snprintf(b, sizeof(b), "%u entr%s from index %04X", n, n == 1 ? "y" : "ies", start);
      return b;
    }
    return asked;
  }
  if (rep.cmd == 0x000B) {
    std::string v = bus_text(rep.payload.data(), rep.payload.size());
    size_t at = v.find(" Version");
    if (at != std::string::npos) v.erase(at, 8);
    return v;
  }
  if (rep.cmd == CMD_SESSION_REPLY && rep.payload.size() == 1 && req.payload.size() == 1)
    return asked + (rep.payload[0] == req.payload[0] ? " · the TAP confirmed"
                                                     : std::string(" · the TAP answered ") + (rep.payload[0] ? "on" : "off"));
  if ((rep.cmd == 0x0039 || rep.cmd == 0x003B) && rep.payload.size() >= 10) {
    std::snprintf(b, sizeof(b), " at %04X", rep.payload[8] << 8 | rep.payload[9]);
    return hex16(rep.payload.data(), 8) + b;
  }
  return asked;
}

// The bus as exchanges: each request with what came back, the last KEEP of
// them. Polls (the CCA's ~44 a second) are only counted, in stats(); an
// optimizer's answer that a poll brings back goes on the query that asked.
class BusWatch {
 public:
  static const size_t KEEP = 300;
  static const int64_t WINDOW_MS = 5 * 60000;
  static const int64_t NO_REPLY_MS = 1000;       // still open this long: nothing came
  static const int64_t OURS_RECENT_MS = 10000;   // we sent within this: we run the bus
  static const int64_t ANSWER_WITHIN_MS = 60000;

  // Returns the frame's reconfiguration, or "".
  std::string on_frame(const Frame &f, bool ours, int64_t unix_ms) {
    if (is_reply(f)) {
      on_reply_(f, unix_ms);
      return "";
    }
    close_open_(unix_ms);
    if (ours) last_ours_ms_ = unix_ms;
    if (f.cmd == CMD_POLL) {
      polls_++;
      poll_open_ = true;
      poll_sent_ms_ = unix_ms;
      return "";
    }
    Exchange e;
    e.id = ++next_id_;
    e.unix_ms = unix_ms;
    e.ours = ours;
    e.other_master = !ours && last_ours_ms_ >= 0 && unix_ms - last_ours_ms_ < OURS_RECENT_MS;
    e.addr = f.dest;
    e.cmd = f.cmd;
    e.name = cmd_name(f.cmd);
    if (e.name.empty()) e.name = hex_cmd_(f.cmd);
    if (f.cmd == CMD_TUNNEL && f.payload.size() >= 5) {
      e.sub = f.payload[3];
      e.seq = f.payload[4];
      e.sub_name = sub_name(f.payload[3]);
      if (e.sub_name.empty()) e.sub_name = "sub " + hex_cmd_(f.payload[3]).substr(4);
      if ((f.payload[3] == 0x06 || f.payload[3] == 0x41) && f.payload.size() > 7) e.node = f.payload[5] << 8 | f.payload[6];
    }
    e.reconfig = reconfiguration(f);
    if (e.sub == 0x0D && !e.reconfig.empty()) e.sub_name = "GatewayCfg write";
    e.category = exchange_category(f.cmd, e.sub, e.reconfig);
    e.summary = e.reconfig.empty() ? request_summary(f) : e.reconfig;
    e.request_hex = bus_hex(f);
    if (f.dest == 0xFFFF) e.result = "broadcast";
    else {
      open_id_ = e.id;
      open_req_ = f;
    }
    push_(e);
    return e.reconfig;
  }

  const std::deque<Exchange> &exchanges() const { return exchanges_; }
  // As they stand at now_ms: one still open past NO_REPLY_MS got nothing.
  std::deque<Exchange> exchanges_at(int64_t now_ms) const {
    std::deque<Exchange> out = exchanges_;
    for (Exchange &e : out)
      if (e.result == "waiting" && now_ms - e.unix_ms >= NO_REPLY_MS) no_reply_(e);
    return out;
  }
  uint64_t polls() const { return polls_; }
  // The TAP radio settings read since the last call.
  std::vector<TapRadio> take_radios() {
    std::vector<TapRadio> out;
    out.swap(radios_);
    return out;
  }
  // The versions heard since the last call.
  std::vector<VersionSeen> take_versions() {
    std::vector<VersionSeen> out;
    out.swap(versions_);
    return out;
  }

  BusStats stats(int64_t now_ms) const {
    BusStats s;
    for (const char *c : {"discovery", "queries", "housekeeping", "bringup", "reconfig", "other"}) s.count[c] = 0;
    int64_t from = now_ms - WINDOW_MS;
    std::vector<int> rts;
    for (const PollMark &p : poll_marks_) {
      if (p.unix_ms < from) continue;
      s.polls++;
      if (p.rt_ms == LOST) s.polls_lost++;
      else if (p.rt_ms == REFUSED) s.polls_refused++;
      else {
        rts.push_back(p.rt_ms);
        if (p.empty) s.polls_empty++;
      }
    }
    for (const Exchange &e : exchanges_at(now_ms)) {
      if (e.unix_ms < from) continue;
      s.count[e.category]++;
      if (e.result == "delivered") s.delivered++;
      if (e.result == "not_delivered") s.not_delivered++;
      if (e.other_master) s.other_master++;
      if (!e.reconfig.empty()) s.reconfig++;
      if (e.rt_ms >= 0) rts.push_back(e.rt_ms);
    }
    if (!rts.empty()) {
      std::sort(rts.begin(), rts.end());
      s.rt_median_ms = rts[(rts.size() - 1) / 2];
      s.rt_max_ms = rts.back();
    }
    s.discovery = discovery_;
    return s;
  }

 private:
  static const int LOST = -1, REFUSED = -2;
  struct PollMark {
    int64_t unix_ms;
    int rt_ms;  // or LOST, REFUSED
    bool empty;
  };

  static std::string hex_cmd_(uint16_t cmd) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%04X", cmd);
    return b;
  }
  static void no_reply_(Exchange &e) {
    e.result = "no_reply";
    if (e.summary.empty()) e.summary = "Nothing back";
  }
  Exchange *find_(uint64_t id) {
    for (auto it = exchanges_.rbegin(); it != exchanges_.rend(); ++it)
      if (it->id == id) return &*it;
    return nullptr;
  }
  void push_(const Exchange &e) {
    exchanges_.push_back(e);
    if (exchanges_.size() > KEEP) exchanges_.pop_front();
  }
  void mark_poll_(int64_t at, int rt, bool empty) {
    poll_marks_.push_back({at, rt, empty});
    while (!poll_marks_.empty() && poll_marks_.front().unix_ms < at - WINDOW_MS) poll_marks_.pop_front();
  }
  // A new request: whatever was still open got nothing back.
  void close_open_(int64_t now) {
    if (poll_open_) mark_poll_(poll_sent_ms_, LOST, false);
    poll_open_ = false;
    if (open_id_) {
      Exchange *e = find_(open_id_);
      if (e) {
        no_reply_(*e);
        recent_unanswered_.push_back({open_id_, open_req_});
        if (recent_unanswered_.size() > 4) recent_unanswered_.pop_front();
      }
    }
    open_id_ = 0;
    (void) now;
  }
  static bool answers_(const Frame &req, const Frame &rep) {
    if ((rep.dest & 0x7FFF) != req.dest) return false;
    if (rep.cmd == 0x0006) return true;
    if (rep.cmd == CMD_NOT_TAKEN) return req.cmd == CMD_TUNNEL && req.payload.size() > 3 && req.payload[3] == 0x06;
    if (rep.cmd != req.cmd + 1) return false;
    if (req.cmd != CMD_TUNNEL) return true;
    TunnelReply t;
    return req.payload.size() > 4 && parse_tunnel_reply(rep.payload, t) && t.seq == req.payload[4];
  }
  void fill_(Exchange &e, const Frame &req, const Frame &rep, int64_t now) {
    TapRadio radio;
    if (parse_gateway_cfg(req, rep, radio)) {
      radios_.push_back(radio);
      if (radios_.size() > 16) radios_.erase(radios_.begin());  // nobody taking them
    }
    e.rt_ms = static_cast<int>(now - e.unix_ms);
    e.reply_hex = bus_hex(rep);
    std::string asked = e.reconfig.empty() ? request_summary(req) : e.reconfig;
    if (rep.cmd == 0x0006) e.result = "refused";
    else if (rep.cmd == CMD_NOT_TAKEN) e.result = "not_delivered";
    else if (e.sub == 0x06) e.result = "delivered";
    else e.result = "answered";
    e.summary = e.reconfig.empty() || rep.cmd == 0x0006 ? reply_summary(req, rep, asked) : e.reconfig;
    TunnelReply t;
    DiscoveryWindow w;
    if (rep.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(rep.payload, t) && parse_discovery_window(t, w))
      discovery_ = window_summary(w);
  }
  void on_reply_(const Frame &f, int64_t now) {
    // The poll's own reply.
    if (poll_open_ && (f.cmd == CMD_POLL_REPLY || f.cmd == 0x0006)) {
      poll_open_ = false;
      if (f.cmd == 0x0006) {
        mark_poll_(poll_sent_ms_, REFUSED, false);
        return;
      }
      bool empty = true;
      for_each_poll_packet(f.payload, [&](uint8_t type, uint16_t tuid, const uint8_t *data, size_t n) {
        empty = false;
        if (type == 0x07) {
          std::string text = bus_text(data, n);
          version_(false, tuid, text);
          answer_(tuid, text, now);
        }
      });
      mark_poll_(poll_sent_ms_, static_cast<int>(now - poll_sent_ms_), empty);
      return;
    }
    if (f.cmd == CMD_POLL_REPLY) return;  // a poll we didn't hear asked
    if (f.cmd == 0x000B) version_(true, f.dest & 0x7FFF, bus_text(f.payload.data(), f.payload.size()));
    if (open_id_ && answers_(open_req_, f)) {
      if (Exchange *e = find_(open_id_)) fill_(*e, open_req_, f, now);
      open_id_ = 0;
      return;
    }
    // Late: the reply to a request the next one already closed.
    for (auto it = recent_unanswered_.rbegin(); it != recent_unanswered_.rend(); ++it) {
      if (!answers_(it->second, f)) continue;
      if (Exchange *e = find_(it->first)) {
        fill_(*e, it->second, f, now);
        e->late = true;
      }
      recent_unanswered_.erase(std::next(it).base());
      return;
    }
    Exchange e;
    e.id = ++next_id_;
    e.unix_ms = now;
    e.addr = f.dest & 0x7FFF;
    e.cmd = f.cmd;
    e.name = cmd_name(f.cmd);
    if (e.name.empty()) e.name = hex_cmd_(f.cmd);
    TunnelReply t;
    if (f.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(f.payload, t)) {
      e.sub = t.sub;
      e.sub_name = sub_name(t.sub);
    }
    e.category = "other";
    e.result = "unasked";
    e.summary = "A reply to no request heard";
    e.reply_hex = bus_hex(f);
    push_(e);
  }
  void version_(bool tap, uint16_t addr, const std::string &text) {
    VersionSeen v;
    if (!parse_version(text, v)) return;
    v.tap = tap;
    v.addr = addr;
    versions_.push_back(v);
    if (versions_.size() > 64) versions_.erase(versions_.begin());  // nobody taking them
  }
  void answer_(uint16_t tuid, std::string text, int64_t now) {
    for (auto it = exchanges_.rbegin(); it != exchanges_.rend(); ++it) {
      if (now - it->unix_ms > ANSWER_WITHIN_MS) return;
      if (it->sub != 0x06 || it->node != tuid || it->result != "delivered" || !it->answer.empty()) continue;
      it->answer = text;
      it->answer_after_ms = static_cast<int>(now - (it->unix_ms + it->rt_ms));
      return;
    }
  }

  std::deque<Exchange> exchanges_;
  std::deque<PollMark> poll_marks_;
  std::deque<std::pair<uint64_t, Frame>> recent_unanswered_;
  uint64_t next_id_ = 0, open_id_ = 0;
  Frame open_req_;
  bool poll_open_ = false;
  int64_t poll_sent_ms_ = 0, last_ours_ms_ = -1;
  uint64_t polls_ = 0;
  std::string discovery_;
  std::vector<VersionSeen> versions_;
  std::vector<TapRadio> radios_;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
