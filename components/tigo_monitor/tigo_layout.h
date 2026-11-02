#pragma once

// The site layout as the Topology page edits it: inverters, their MPPTs, the
// strings on each MPPT, and the panels in each string, in order.
//
// The Pi daemon and the ESP32 both serve the same page, so the rules for a
// layout live here once: what a valid one is, what each panel is called, how
// an incoming layout (a CCA sync, a cloud import, a node-table import, or on
// the ESP32 the YAML's inverters) compares with the one the user made, and
// whether to apply it or hold it for the user to decide. A panel's place is
// stored where it always was, in its node's labels; the "skeleton" encoded
// here keeps what no node carries: the inverters, and MPPTs and strings that
// have no panels yet.
//
// Header-only and free of ESPHome dependencies, so the rules are exercised on
// the host (tests/host/layout_test.cpp). Plain std types: a layout is a few
// KB at most.

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace layout {

struct StringSpec {
  std::string label;          // identity: what nodes carry as cca_string_label; never changes
  std::string display_label;  // empty: show label
  int rating_w = 0;           // per-panel nameplate rating, 0 = not set
  std::vector<std::string> panels;  // barcodes, in order
};

struct MpptSpec {
  std::string label;  // "MPPT 1", unique across the site
  std::vector<StringSpec> strings;
};

struct InverterSpec {
  std::string name, display_name;
  std::vector<MpptSpec> mppts;
};

struct Layout {
  std::vector<InverterSpec> inverters;
};

// One panel's place, as the node-table labels that express it.
struct Assignment {
  std::string barcode, label, string_label, mppt_label;
};

// (inverter name, its MPPT labels): an incoming inverter list on both builds.
using InverterMppts = std::vector<std::pair<std::string, std::vector<std::string>>>;

// The ESP32 keeps a node in a '|'-delimited 256-byte NVS slot with three of
// these in it, so every name is capped and may not hold the delimiter.
constexpr size_t kMaxName = 32;
// The skeleton fits eight 256-byte NVS slots on the ESP32.
constexpr size_t kMaxSkeleton = 2048;

inline std::string panel_name(const StringSpec &s, size_t index) {
  return (s.display_label.empty() ? s.label : s.display_label) + std::to_string(index + 1);
}

// The trailing number of a panel name ("A3" -> 3), or INT_MAX when it has none,
// so unnumbered panels sort last.
inline int position_of(const std::string &panel_label) {
  size_t i = panel_label.size();
  while (i > 0 && panel_label[i - 1] >= '0' && panel_label[i - 1] <= '9') i--;
  if (i == panel_label.size() || panel_label.size() - i > 9) return INT_MAX;
  return std::atoi(panel_label.c_str() + i);
}

namespace detail {
inline std::string name_problem(const std::string &what, const std::string &name, bool required) {
  if (name.empty()) return required ? what + " needs a name." : "";
  if (name.size() > kMaxName)
    return what + " \"" + name + "\" is longer than " + std::to_string(kMaxName) + " characters.";
  for (char c : name)
    if (c == '|' || static_cast<unsigned char>(c) < 0x20)
      return what + " \"" + name + "\" has a character it can't use ('|' or a control character).";
  return "";
}
}  // namespace detail

// "" when the layout can be saved, else one sentence for the user.
inline std::string validate(const Layout &l, const std::function<bool(const std::string &)> &known_barcode) {
  std::set<std::string> inverters, mppts, strings, panels;
  for (const auto &inv : l.inverters) {
    std::string p = detail::name_problem("An inverter", inv.name, true);
    if (p.empty()) p = detail::name_problem("The inverter name", inv.display_name, false);
    if (!p.empty()) return p;
    if (!inverters.insert(inv.name).second) return "Two inverters are called \"" + inv.name + "\".";
    for (const auto &m : inv.mppts) {
      if (!(p = detail::name_problem("An MPPT", m.label, true)).empty()) return p;
      if (!mppts.insert(m.label).second) return "\"" + m.label + "\" is listed twice; each MPPT belongs to one inverter.";
      for (const auto &s : m.strings) {
        if (!(p = detail::name_problem("A string", s.label, true)).empty()) return p;
        if (!(p = detail::name_problem("The string name", s.display_label, false)).empty()) return p;
        if (!strings.insert(s.label).second) return "Two strings are called \"" + s.label + "\".";
        if (s.rating_w < 0 || s.rating_w > 65535) return "String \"" + s.label + "\" has a rating out of range.";
        for (size_t i = 0; i < s.panels.size(); i++) {
          const std::string &bc = s.panels[i];
          if (!known_barcode(bc)) return "Panel " + bc + " isn't in the node table any more.";
          if (!panels.insert(bc).second) return "Panel " + bc + " is in the layout twice.";
          std::string name = panel_name(s, i);
          if (name.size() > kMaxName)
            return "\"" + name + "\" is longer than " + std::to_string(kMaxName) + " characters; shorten the string's name.";
        }
      }
    }
  }
  return "";
}

inline std::vector<Assignment> assignments(const Layout &l) {
  std::vector<Assignment> out;
  for (const auto &inv : l.inverters)
    for (const auto &m : inv.mppts)
      for (const auto &s : m.strings)
        for (size_t i = 0; i < s.panels.size(); i++) out.push_back({s.panels[i], panel_name(s, i), s.label, m.label});
  return out;
}

struct Change {
  std::string barcode;
  Assignment now, then;  // now: as it stands; then: as the incoming layout has it
};

// One Change per incoming panel whose place or name differs. A panel the
// incoming layout doesn't mention is left alone, as a sync leaves it.
inline std::vector<Change> diff(const std::vector<Assignment> &current, const std::vector<Assignment> &incoming) {
  std::map<std::string, const Assignment *> by_bc;
  for (const auto &a : current) by_bc[a.barcode] = &a;
  std::vector<Change> out;
  for (const auto &in : incoming) {
    auto it = by_bc.find(in.barcode);
    Assignment now;
    now.barcode = in.barcode;
    if (it != by_bc.end()) now = *it->second;
    if (now.label == in.label && now.string_label == in.string_label && now.mppt_label == in.mppt_label) continue;
    out.push_back({in.barcode, now, in});
  }
  return out;
}

inline uint32_t fingerprint(const std::vector<Assignment> &as, const InverterMppts &inverters) {
  std::vector<std::string> lines;
  for (const auto &a : as) lines.push_back("P|" + a.barcode + "|" + a.label + "|" + a.string_label + "|" + a.mppt_label);
  for (const auto &inv : inverters) {
    std::string l = "I|" + inv.first;
    for (const auto &m : inv.second) l += "|" + m;
    lines.push_back(l);
  }
  std::sort(lines.begin(), lines.end());
  uint32_t h = 2166136261u;  // FNV-1a
  for (const auto &l : lines) {
    for (char c : l) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    h = (h ^ '\n') * 16777619u;
  }
  return h;
}

enum class Offer { apply, hold, unchanged, declined };

// Until the user saves a layout, whatever arrives applies, as it always has.
// After that, a difference waits for the user, unless it is the very one
// they already turned down.
inline Offer decide(bool manual, bool has_changes, uint32_t incoming_fp, uint32_t declined_fp) {
  if (!manual) return Offer::apply;
  if (!has_changes) return Offer::unchanged;
  if (incoming_fp == declined_fp) return Offer::declined;
  return Offer::hold;
}

// Inverters, MPPTs and strings without their panels, one per line:
//   v1|<manual 0/1>|<declined fingerprint, hex>
//   I|<name>|<display name>
//   M|<label>
//   S|<label>|<display label>|<rating W>
// validate() keeps '|' and newlines out of every field.
inline std::string encode_skeleton(const Layout &l, bool manual, uint32_t declined_fp) {
  char head[32];
  std::snprintf(head, sizeof(head), "v1|%d|%08x\n", manual ? 1 : 0, static_cast<unsigned>(declined_fp));
  std::string out = head;
  for (const auto &inv : l.inverters) {
    out += "I|" + inv.name + "|" + inv.display_name + "\n";
    for (const auto &m : inv.mppts) {
      out += "M|" + m.label + "\n";
      for (const auto &s : m.strings)
        out += "S|" + s.label + "|" + s.display_label + "|" + std::to_string(s.rating_w) + "\n";
    }
  }
  return out;
}

inline bool decode_skeleton(const std::string &text, Layout &l, bool &manual, uint32_t &declined_fp) {
  auto split = [](const std::string &line) {
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
      size_t bar = line.find('|', start);
      f.push_back(line.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
      if (bar == std::string::npos) return f;
      start = bar + 1;
    }
  };
  Layout out;
  bool head = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    start = nl == std::string::npos ? text.size() : nl + 1;
    if (line.empty()) continue;
    auto f = split(line);
    if (!head) {
      if (f.size() != 3 || f[0] != "v1") return false;
      manual = f[1] == "1";
      declined_fp = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 16));
      head = true;
    } else if (f[0] == "I" && f.size() == 3) {
      out.inverters.push_back({f[1], f[2], {}});
    } else if (f[0] == "M" && f.size() == 2 && !out.inverters.empty()) {
      out.inverters.back().mppts.push_back({f[1], {}});
    } else if (f[0] == "S" && f.size() == 4 && !out.inverters.empty() && !out.inverters.back().mppts.empty()) {
      StringSpec s;
      s.label = f[1];
      s.display_label = f[2];
      s.rating_w = std::atoi(f[3].c_str());
      out.inverters.back().mppts.back().strings.push_back(s);
    } else {
      return false;
    }
  }
  if (!head) return false;
  l = std::move(out);
  return true;
}

}  // namespace layout
}  // namespace tigo_monitor
}  // namespace esphome
