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
// Nothing here keeps a payload. An event holds the frame's names, its length,
// and text only for a text query and VERSION's reply, with any long hex run
// masked: no MAC, PAN, key or table entry gets into the log.

#include "tigo_master.h"

#include <cctype>
#include <cstdio>
#include <deque>
#include <string>

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

// Text from the bus, made safe to show: printable only, CR/LF trimmed, and
// any run of 8 or more hex digits (a MAC, a barcode) replaced with <hex>.
inline std::string bus_text(const uint8_t *p, size_t n) {
  std::string raw;
  for (size_t i = 0; i < n; i++) raw += p[i] == '\r' || p[i] == '\n' ? ' ' : std::isprint(p[i]) ? char(p[i]) : '.';
  std::string out;
  for (size_t i = 0; i < raw.size();) {
    size_t j = i;
    while (j < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[j]))) j++;
    if (j - i >= 8) {
      out += "<hex>";
      i = j;
    } else if (j > i) {
      out.append(raw, i, j - i);
      i = j;
    } else {
      out += raw[i++];
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// The reconfiguration a request is, by name, or "" if a master here might
// send it.
inline std::string reconfiguration(const Frame &f) {
  if (f.dest & 0x8000) return "";  // a reply
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

struct BusEvent {
  int64_t unix_ms = 0;
  bool from_tap = false;  // a reply (dest has 0x8000)
  bool ours = false;      // sent by this monitor's own master
  uint16_t addr = 0;      // the gateway's RS485 id (0xFFFF a broadcast)
  uint16_t cmd = 0;
  int sub = -1;           // a tunnel frame's sub
  int node = -1;          // a text query's optimizer id
  uint16_t len = 0;       // payload bytes
  std::string name;       // "Tunnel · DiscState"
  std::string text;       // a query's text or VERSION's reply, masked
  std::string reconfig;   // what it reconfigures, if it does
};

// The last KEEP frames other than polls, which are counted instead (the CCA
// sends ~44 a second; they would push everything else out in seconds).
class BusWatch {
 public:
  static const size_t KEEP = 300;

  // Returns the frame's reconfiguration, or "".
  std::string on_frame(const Frame &f, bool ours, int64_t unix_ms) {
    if (f.cmd == CMD_POLL || f.cmd == CMD_POLL_REPLY) {
      polls_++;
      return "";
    }
    BusEvent e;
    e.unix_ms = unix_ms;
    e.from_tap = f.dest & 0x8000;
    e.ours = ours;
    e.addr = f.dest & 0x7FFF;
    if (f.dest == 0xFFFF) e.addr = 0xFFFF;
    e.cmd = f.cmd;
    e.len = static_cast<uint16_t>(f.payload.size());
    e.name = cmd_name(f.cmd);
    if (e.name.empty()) {
      char b[16];
      std::snprintf(b, sizeof(b), "0x%04X", f.cmd);
      e.name = b;
    }
    if ((f.cmd == CMD_TUNNEL || f.cmd == CMD_TUNNEL_REPLY) && f.payload.size() >= 5) {
      e.sub = f.payload[3];
      const char *s = sub_name(f.payload[3]);
      char b[16];
      if (!*s) {
        std::snprintf(b, sizeof(b), "sub 0x%02X", f.payload[3]);
        s = b;
      }
      e.name += std::string(" · ") + s;
      if (f.cmd == CMD_TUNNEL && f.payload[3] == 0x06 && f.payload.size() > 7) {
        e.node = f.payload[5] << 8 | f.payload[6];
        e.text = bus_text(f.payload.data() + 7, f.payload.size() - 7);
      }
    }
    if (f.cmd == 0x000B) e.text = bus_text(f.payload.data(), f.payload.size());
    if (f.cmd == 0x000B) {  // "Mgate Version G8.59": the word Version adds nothing
      size_t v = e.text.find(" Version");
      if (v != std::string::npos) e.text.erase(v, 8);
    }
    e.reconfig = reconfiguration(f);
    events_.push_back(e);
    if (events_.size() > KEEP) events_.pop_front();
    return e.reconfig;
  }

  const std::deque<BusEvent> &events() const { return events_; }
  uint64_t polls() const { return polls_; }

 private:
  std::deque<BusEvent> events_;
  uint64_t polls_ = 0;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
