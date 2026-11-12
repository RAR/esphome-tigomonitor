// The roof layout: where each panel and each TAP physically sits, as
// Topology > Roof places them on a grid of panel-sized cells. One cell per
// panel, so panels in touching cells are neighbours on the roof; a gap
// between roof faces is empty cells. Portrait or landscape only changes how
// a panel is drawn.
//
// Kept apart from the wiring layout (inverter > MPPT > string > panel): that
// says how panels are connected, this says where they are. It feeds the roof
// view, and later choosing which optimizers the TAP leans on for the far
// part of the array.
#pragma once

#include <nlohmann/json.hpp>

#include <cctype>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct RoofPanel {
  std::string addr;  // the optimizer's short address, 4 hex digits, upper case
  int x = 0, y = 0;  // the cell, columns and rows from the top left
  bool portrait = true;
};

struct RoofTap {
  std::string id;  // the TAP's RS485 id, 4 hex digits, upper case
  int x = 0, y = 0;
};

struct Roof {
  std::vector<RoofPanel> panels;
  std::vector<RoofTap> taps;
};

static const int ROOF_CELLS = 200;     // columns and rows, each 0..199
static const size_t ROOF_MAX = 1024;   // panels, as a TAP's table holds

// 4 hex digits, upper-cased into out; false otherwise.
inline bool roof_hex4(const nlohmann::json &j, std::string &out) {
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

inline bool roof_cell(const nlohmann::json &item, int &x, int &y) {
  auto ok = [](const nlohmann::json &v) { return v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() < ROOF_CELLS; };
  if (!item.contains("x") || !item.contains("y") || !ok(item["x"]) || !ok(item["y"])) return false;
  x = item["x"].get<int>();
  y = item["y"].get<int>();
  return true;
}

// {"panels": [{"addr", "x", "y", "portrait"}], "taps": [{"id", "x", "y"}]}
// into out; "" when it's a roof, else why not (out is then unchanged).
inline std::string roof_from_json(const nlohmann::json &j, Roof &out) {
  if (!j.is_object() || !j.contains("panels") || !j["panels"].is_array() || !j.contains("taps") || !j["taps"].is_array())
    return "expected {\"panels\": [...], \"taps\": [...]}";
  if (j["panels"].size() > ROOF_MAX) return "too many panels";
  Roof r;
  std::set<std::string> addrs, ids;
  std::set<std::pair<int, int>> cells;
  for (const auto &p : j["panels"]) {
    RoofPanel rp;
    if (!p.is_object() || !p.contains("addr") || !roof_hex4(p["addr"], rp.addr)) return "a panel's addr must be 4 hex digits";
    if (!roof_cell(p, rp.x, rp.y)) return "panel " + rp.addr + ": x and y must be whole numbers 0-199";
    if (p.contains("portrait")) {
      if (!p["portrait"].is_boolean()) return "panel " + rp.addr + ": portrait must be true or false";
      rp.portrait = p["portrait"].get<bool>();
    }
    if (!addrs.insert(rp.addr).second) return "panel " + rp.addr + " is placed twice";
    if (!cells.insert({rp.x, rp.y}).second) return "two things are in the same place, at " + std::to_string(rp.x) + "," + std::to_string(rp.y);
    r.panels.push_back(rp);
  }
  for (const auto &t : j["taps"]) {
    RoofTap rt;
    if (!t.is_object() || !t.contains("id") || !roof_hex4(t["id"], rt.id)) return "a TAP's id must be 4 hex digits";
    if (!roof_cell(t, rt.x, rt.y)) return "TAP " + rt.id + ": x and y must be whole numbers 0-199";
    if (!ids.insert(rt.id).second) return "TAP " + rt.id + " is placed twice";
    if (!cells.insert({rt.x, rt.y}).second) return "two things are in the same place, at " + std::to_string(rt.x) + "," + std::to_string(rt.y);
    r.taps.push_back(rt);
  }
  out = std::move(r);
  return "";
}

inline nlohmann::json roof_to_json(const Roof &r) {
  nlohmann::json panels = nlohmann::json::array(), taps = nlohmann::json::array();
  for (const auto &p : r.panels) panels.push_back({{"addr", p.addr}, {"x", p.x}, {"y", p.y}, {"portrait", p.portrait}});
  for (const auto &t : r.taps) taps.push_back({{"id", t.id}, {"x", t.x}, {"y", t.y}});
  return {{"panels", panels}, {"taps", taps}};
}
