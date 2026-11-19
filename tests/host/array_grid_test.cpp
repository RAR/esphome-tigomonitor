// Host test for array_grid.h: the array layout's parser and writer, shared
// by both builds, against the Pi's earlier implementation on nlohmann::json
// (copied below as `ref`): the same layout or the same error for each input,
// and the same text out.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/array_grid_test.cpp -o /tmp/array_grid_test && /tmp/array_grid_test
#include "array_grid.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ref {
struct ArrayPanel {
  std::string addr;  // the optimizer's short address, 4 hex digits, upper case
  int x = 0, y = 0;  // the cell, columns and rows from the top left
  bool portrait = true;
  std::string barcode;  // its barcode, upper case; empty in a layout saved before
                        // barcodes were kept. The address can change under a
                        // panel (a renumbering); the barcode can't.
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

// 1-32 letters and digits, upper-cased into out; false otherwise.
inline bool array_barcode(const nlohmann::json &j, std::string &out) {
  if (!j.is_string()) return false;
  std::string s = j.get<std::string>();
  if (s.empty() || s.size() > 32) return false;
  for (char &c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c))) return false;
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

// {"panels": [{"addr", "barcode"?, "x", "y", "portrait"}], "taps": [{"id", "x", "y"}]}
// into out; "" when it's an array, else why not (out is then unchanged).
inline std::string array_from_json(const nlohmann::json &j, ArrayLayout &out) {
  if (!j.is_object() || !j.contains("panels") || !j["panels"].is_array() || !j.contains("taps") || !j["taps"].is_array())
    return "expected {\"panels\": [...], \"taps\": [...]}";
  if (j["panels"].size() > ARRAY_MAX) return "too many panels";
  ArrayLayout r;
  std::set<std::string> addrs, ids, barcodes;
  std::set<std::pair<int, int>> panel_cells, tap_cells;  // a TAP may share a panel's square, as under it
  for (const auto &p : j["panels"]) {
    ArrayPanel rp;
    if (!p.is_object() || !p.contains("addr") || !array_hex4(p["addr"], rp.addr)) return "a panel's addr must be 4 hex digits";
    if (!array_cell(p, rp.x, rp.y)) return "panel " + rp.addr + ": x and y must be whole numbers 0-199";
    if (p.contains("barcode") && !array_barcode(p["barcode"], rp.barcode))
      return "panel " + rp.addr + ": barcode must be letters and digits";
    if (!rp.barcode.empty() && !barcodes.insert(rp.barcode).second) return "barcode " + rp.barcode + " is placed twice";
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
  for (const auto &p : r.panels) {
    nlohmann::json o = {{"addr", p.addr}, {"x", p.x}, {"y", p.y}, {"portrait", p.portrait}};
    if (!p.barcode.empty()) o["barcode"] = p.barcode;
    panels.push_back(o);
  }
  for (const auto &t : r.taps) taps.push_back({{"id", t.id}, {"x", t.x}, {"y", t.y}});
  return {{"panels", panels}, {"taps", taps}};
}

// Each panel to the address its barcode has now (barcode -> address, upper
// case), so a renumbering doesn't move panels around the drawing. A panel
// with no barcode saved keeps its address, unless a panel that has one now
// holds that address: then it can't be told which panel it was, and it goes
// back to the tray. A barcode not in the table yet keeps its address.
inline void array_follow_barcodes(ArrayLayout &r, const std::map<std::string, std::string> &addr_of) {
  std::set<std::string> claimed;
  for (auto &p : r.panels) {
    if (p.barcode.empty()) continue;
    auto it = addr_of.find(p.barcode);
    if (it != addr_of.end()) p.addr = it->second;
    claimed.insert(p.addr);
  }
  std::vector<ArrayPanel> kept;
  std::set<std::string> seen;
  for (auto &p : r.panels) {
    if (p.barcode.empty() && claimed.count(p.addr)) continue;
    if (!seen.insert(p.addr).second) continue;  // two barcodes now at one address: the first wins
    kept.push_back(p);
  }
  r.panels = std::move(kept);
}

// Give each panel with no barcode the one its address has now (address ->
// barcode), as a save does, so the layout follows barcodes from then on.
inline void array_fill_barcodes(ArrayLayout &r, const std::map<std::string, std::string> &barcode_of) {
  for (auto &p : r.panels) {
    if (!p.barcode.empty()) continue;
    auto it = barcode_of.find(p.addr);
    if (it != barcode_of.end()) p.barcode = it->second;
  }
}

// From a node table (anything with .addr and .long_address): barcode ->
// address and address -> barcode, upper case. A provisional alias (0x8000
// set) loses to the real address of the same barcode.
template<class Nodes>
void array_node_maps(const Nodes &nodes, std::map<std::string, std::string> &addr_of,
                     std::map<std::string, std::string> &barcode_of) {
  auto up = [](std::string v) {
    for (char &c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return v;
  };
  auto alias = [](const std::string &a) { return !a.empty() && std::strchr("89ABCDEF", a[0]) != nullptr; };
  for (const auto &n : nodes) {
    if (n.long_address.empty() || n.addr.size() != 4) continue;
    std::string a = up(n.addr), b = up(n.long_address);
    auto it = addr_of.find(b);
    if (it == addr_of.end() || (alias(it->second) && !alias(a))) addr_of[b] = a;
    barcode_of[a] = b;
  }
}

}  // namespace ref

using namespace esphome::tigo_monitor::array_grid;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

// As the Pi answered a POST: a body that isn't JSON at all, then the rules.
static std::string ref_parse(const std::string &text, ref::ArrayLayout &r) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded()) return "Expected a JSON object.";
  return ref::array_from_json(j, r);
}

static bool same(const ArrayLayout &a, const ref::ArrayLayout &b) {
  if (a.panels.size() != b.panels.size() || a.taps.size() != b.taps.size()) return false;
  for (size_t i = 0; i < a.panels.size(); i++) {
    const auto &p = a.panels[i];
    const auto &q = b.panels[i];
    if (std::string(p.addr.c_str()) != q.addr || p.x != q.x || p.y != q.y || p.portrait != q.portrait ||
        std::string(p.barcode.c_str()) != q.barcode)
      return false;
  }
  for (size_t i = 0; i < a.taps.size(); i++)
    if (std::string(a.taps[i].id.c_str()) != b.taps[i].id || a.taps[i].x != b.taps[i].x || a.taps[i].y != b.taps[i].y)
      return false;
  return true;
}

static const char *const INPUTS[] = {
    // Accepted.
    R"({"panels": [{"addr": "0026", "x": 3, "y": 2, "portrait": true}, {"addr": "000a", "x": 4, "y": 2, "portrait": false}],
        "taps": [{"id": "1201", "x": 9, "y": 7}]})",
    R"({"panels": [], "taps": []})",
    R"(  {"taps":[],"panels":[{"addr":"0002","x":0,"y":0}]}  )",
    "{\n\t\"panels\" :\r\n [ { \"addr\" : \"0026\" , \"x\" : 5 , \"y\" : 5 } ] ,\"taps\":[{\"id\":\"1201\",\"x\":5,\"y\":5}]}",
    R"({"panels": [{"addr": "0026", "barcode": "04c05b0000000001", "x": 3, "y": 2}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "04C05B\u0030000000001", "x": -0, "y": 199}], "taps": []})",
    // Unknown keys, nested anything, are skipped; a repeated key, the last wins.
    R"({"note": {"a": [1, 2.5e3, {"b": "]}\"{"}], "c": null}, "panels": 3, "panels": [{"addr": "0026", "x": 0, "y": 0,
        "extra": [[], {}, true, false, null, "\\"], "x": 7}], "taps": [], "z": -1.5E-2})",
    // Refused by the rules.
    R"([])",
    R"("text")",
    R"(42)",
    R"({"panels": 3, "taps": []})",
    R"({"panels": []})",
    R"({"panels": [7], "taps": []})",
    R"({"panels": [{"addr": "26", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": null, "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "zz26", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": -1, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 0, "y": 200}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 1.5, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 1e0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": "1", "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 99999999999999999999999, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 0, "y": 0, "portrait": "yes"}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 0, "y": 0, "portrait": null}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 0, "y": 0}, {"addr": "0026", "x": 1, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 0, "y": 0}, {"addr": "0027", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [], "taps": [{"id": "1201", "x": 0, "y": 0}, {"id": "1202", "x": 0, "y": 0}]})",
    R"({"panels": [], "taps": [{"id": "1201", "x": 0, "y": 0}, {"id": "1201", "x": 1, "y": 0}]})",
    R"({"panels": [], "taps": [{"id": "12", "x": 0, "y": 0}]})",
    R"({"panels": [], "taps": ["1201"]})",
    R"({"panels": [{"addr": "0026", "barcode": 7, "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "04C0 5B", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "123456789012345678901234567890123", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "04C0\u00e95B", "x": 0, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "barcode": "04C0\ud83d\ude005B", "x": 0, "y": 0}], "taps": []})",
    "{\"panels\": [{\"addr\": \"0026\", \"barcode\": \"04C0\xc3\xa9" "5B\", \"x\": 0, \"y\": 0}], \"taps\": []}",
    R"({"panels": [{"addr": "0026", "barcode": "04C05B0000000001", "x": 0, "y": 0},
                   {"addr": "0027", "barcode": "04c05b0000000001", "x": 1, "y": 0}], "taps": []})",
    // Not JSON at all.
    "",
    "   ",
    R"({"panels": [)",
    R"({"panels": [], "taps": []} x)",
    R"({"panels": [], "taps": [],})",
    R"({"panels": [,], "taps": []})",
    R"({panels: [], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 01, "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": 1., "y": 0}], "taps": []})",
    R"({"panels": [{"addr": "0026", "x": +1, "y": 0}], "taps": []})",
    R"({"panels": [], "taps": [], "n": nul})",
    R"({"panels": [], "taps": [], "s": "\q"})",
    R"({"panels": [], "taps": [], "s": "\u12"})",
    R"({"panels": [], "taps": [], "s": "\ud83d"})",
    R"({"panels": [], "taps": [], "s": "\ude00"})",
    "{\"panels\": [], \"taps\": [], \"s\": \"a\tb\"}",
    "{\"panels\": [], \"taps\": [], \"s\": \"\xc3\"}",
    "{\"panels\": [], \"taps\": [], \"s\": \"\xff\"}",
    "{\"panels\": [], \"taps\": [], \"s\": \"\xed\xa0\x80\"}",
};

static void test_the_same_as_the_pi_s() {
  int n = 0;
  for (const char *in : INPUTS) {
    ref::ArrayLayout want;
    std::string want_err = ref_parse(in, want);
    ArrayLayout got;
    std::string got_err = array_from_text(in, std::strlen(in), got).c_str();
    bool ok = got_err == want_err && (!want_err.empty() || same(got, want));
    if (!ok) std::printf("  differs on %s\n    pi: \"%s\"\n    here: \"%s\"\n", in, want_err.c_str(), got_err.c_str());
    CHECK(ok);
    if (want_err.empty()) {
      Str text;
      array_write_json(text, got);
      CHECK(std::string(text.c_str()) == ref::array_to_json(want).dump());
    }
    n++;
  }
  std::printf("  %d inputs compared\n", n);
}

// A refused body leaves the layout as it was.
static void test_refused_leaves_the_layout() {
  ArrayLayout r;
  const char *good = R"({"panels": [{"addr": "0026", "x": 3, "y": 2}], "taps": []})";
  CHECK(array_from_text(good, std::strlen(good), r).empty());
  const char *bad = R"({"panels": [{"addr": "0026", "x": 3, "y": 2}, {"addr": "0026", "x": 4, "y": 2}], "taps": []})";
  CHECK(!array_from_text(bad, std::strlen(bad), r).empty());
  CHECK(r.panels.size() == 1 && r.panels[0].x == 3);
}

// More panels than ARRAY_MAX: refused before any is read.
static void test_too_many() {
  std::string s = "{\"panels\": [";
  for (size_t i = 0; i <= ARRAY_MAX; i++) s += std::string(i ? "," : "") + "{}";
  s += "], \"taps\": []}";
  ref::ArrayLayout want;
  ArrayLayout got;
  CHECK(ref_parse(s, want) == "too many panels");
  CHECK(std::string(array_from_text(s.data(), s.size(), got).c_str()) == "too many panels");
}

// Deep nesting in a skipped key is refused, not followed down the stack.
static void test_deep_nesting_is_refused() {
  std::string s = "{\"panels\": [], \"taps\": [], \"deep\": " + std::string(10000, '[') + std::string(10000, ']') + "}";
  ArrayLayout got;
  CHECK(std::string(array_from_text(s.data(), s.size(), got).c_str()) == "Expected a JSON object.");
}

int main() {
  test_the_same_as_the_pi_s();
  test_refused_leaves_the_layout();
  test_too_many();
  test_deep_nesting_is_refused();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
