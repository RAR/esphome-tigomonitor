// The array layout: where each panel and each TAP physically sits, as
// Topology > Array places them on a grid of panel-sized cells. One cell
// per panel, so panels in touching cells are neighbours; a gap, such as
// between roof faces or ground-mount rows, is empty cells. Portrait or
// landscape only changes how a panel is drawn. A TAP can share a panel's
// square, as when it's mounted under that panel.
//
// Kept apart from the wiring layout (inverter > MPPT > string > panel): that
// says how panels are connected, this says where they are. It feeds the array
// view, and later choosing which optimizers the TAP leans on for the far
// part of the array.
#pragma once

#include <nlohmann/json.hpp>

#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct ArrayPanel {
  std::string addr;  // the optimizer's short address, 4 hex digits, upper case
  int x = 0, y = 0;  // the cell, columns and rows from the top left
  bool portrait = true;
};

struct ArrayTap {
  std::string id;  // the TAP's RS485 id, 4 hex digits, upper case
  int x = 0, y = 0;
};

struct ArrayLayout {
  std::vector<ArrayPanel> panels;
  std::vector<ArrayTap> taps;
};

static const int ARRAY_CELLS = 200;     // columns and rows, each 0..199
static const size_t ARRAY_MAX = 1024;   // panels, as a TAP's table holds

// 4 hex digits, upper-cased into out; false otherwise.
inline bool array_hex4(const nlohmann::json &j, std::string &out) {
  if (!j.is_string()) return false;
  std::string s = j.get<std::string>();
  if (s.size() != 4) return false;
  for (char &c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  out = s;
  return true;
}

inline bool array_cell(const nlohmann::json &item, int &x, int &y) {
  auto ok = [](const nlohmann::json &v) { return v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() < ARRAY_CELLS; };
  if (!item.contains("x") || !item.contains("y") || !ok(item["x"]) || !ok(item["y"])) return false;
  x = item["x"].get<int>();
  y = item["y"].get<int>();
  return true;
}

// {"panels": [{"addr", "x", "y", "portrait"}], "taps": [{"id", "x", "y"}]}
// into out; "" when it's an array, else why not (out is then unchanged).
inline std::string array_from_json(const nlohmann::json &j, ArrayLayout &out) {
  if (!j.is_object() || !j.contains("panels") || !j["panels"].is_array() || !j.contains("taps") || !j["taps"].is_array())
    return "expected {\"panels\": [...], \"taps\": [...]}";
  if (j["panels"].size() > ARRAY_MAX) return "too many panels";
  ArrayLayout r;
  std::set<std::string> addrs, ids;
  std::set<std::pair<int, int>> panel_cells, tap_cells;  // a TAP may share a panel's square, as under it
  for (const auto &p : j["panels"]) {
    ArrayPanel rp;
    if (!p.is_object() || !p.contains("addr") || !array_hex4(p["addr"], rp.addr)) return "a panel's addr must be 4 hex digits";
    if (!array_cell(p, rp.x, rp.y)) return "panel " + rp.addr + ": x and y must be whole numbers 0-199";
    if (p.contains("portrait")) {
      if (!p["portrait"].is_boolean()) return "panel " + rp.addr + ": portrait must be true or false";
      rp.portrait = p["portrait"].get<bool>();
    }
    if (!addrs.insert(rp.addr).second) return "panel " + rp.addr + " is placed twice";
    if (!panel_cells.insert({rp.x, rp.y}).second) return "two panels are in the same place, at " + std::to_string(rp.x) + "," + std::to_string(rp.y);
    r.panels.push_back(rp);
  }
  for (const auto &t : j["taps"]) {
    ArrayTap rt;
    if (!t.is_object() || !t.contains("id") || !array_hex4(t["id"], rt.id)) return "a TAP's id must be 4 hex digits";
    if (!array_cell(t, rt.x, rt.y)) return "TAP " + rt.id + ": x and y must be whole numbers 0-199";
    if (!ids.insert(rt.id).second) return "TAP " + rt.id + " is placed twice";
    if (!tap_cells.insert({rt.x, rt.y}).second) return "two TAPs are in the same place, at " + std::to_string(rt.x) + "," + std::to_string(rt.y);
    r.taps.push_back(rt);
  }
  out = std::move(r);
  return "";
}

inline nlohmann::json array_to_json(const ArrayLayout &r) {
  nlohmann::json panels = nlohmann::json::array(), taps = nlohmann::json::array();
  for (const auto &p : r.panels) panels.push_back({{"addr", p.addr}, {"x", p.x}, {"y", p.y}, {"portrait", p.portrait}});
  for (const auto &t : r.taps) taps.push_back({{"id", t.id}, {"x", t.x}, {"y", t.y}});
  return {{"panels", panels}, {"taps", taps}};
}
