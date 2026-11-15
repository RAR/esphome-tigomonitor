// Host test for linux/src/array_layout.h: the array layout, where each panel and TAP
// physically sits, as Topology > Array saves it.
#include "array_layout.h"

#include <cstdio>
#include <map>

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

using nlohmann::json;

static void test_round_trip() {
  ArrayLayout r;
  json in = json::parse(R"({"panels": [{"addr": "0026", "x": 3, "y": 2, "portrait": true},
                                        {"addr": "000a", "x": 4, "y": 2, "portrait": false}],
                            "taps": [{"id": "1201", "x": 9, "y": 7}]})");
  CHECK(array_from_json(in, r).empty());
  CHECK(r.panels.size() == 2 && r.taps.size() == 1);
  CHECK(r.panels[1].addr == "000A");  // upper case, as everywhere else
  CHECK(!r.panels[1].portrait);
  json out = array_to_json(r);
  CHECK(out["panels"][0]["addr"] == "0026" && out["panels"][0]["x"] == 3 && out["panels"][0]["portrait"] == true);
  CHECK(out["taps"][0]["id"] == "1201" && out["taps"][0]["y"] == 7);
  // Orientation defaults to portrait.
  CHECK(array_from_json(json::parse(R"({"panels": [{"addr": "0002", "x": 0, "y": 0}], "taps": []})"), r).empty());
  CHECK(r.panels[0].portrait);
  // A TAP can share a panel's square: it's often mounted under one.
  CHECK(array_from_json(json::parse(R"({"panels": [{"addr": "0026", "x": 5, "y": 5}], "taps": [{"id": "1201", "x": 5, "y": 5}]})"), r).empty());
  CHECK(r.taps.size() == 1 && r.taps[0].x == 5);
  // An empty array is an array.
  CHECK(array_from_json(json::parse(R"({"panels": [], "taps": []})"), r).empty());
  CHECK(r.panels.empty());
}

static void test_refused() {
  ArrayLayout r;
  for (const char *bad : {
           R"([])",
           R"({"panels": 3, "taps": []})",
           R"({"panels": [{"addr": "26", "x": 0, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "zz26", "x": 0, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": -1, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": 0, "y": 200}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": 1.5, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": 0, "y": 0, "portrait": "yes"}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": 0, "y": 0}, {"addr": "0026", "x": 1, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "x": 0, "y": 0}, {"addr": "0027", "x": 0, "y": 0}], "taps": []})",
           R"({"panels": [], "taps": [{"id": "1201", "x": 0, "y": 0}, {"id": "1202", "x": 0, "y": 0}]})",
           R"({"panels": [], "taps": [{"id": "1201", "x": 0, "y": 0}, {"id": "1201", "x": 1, "y": 0}]})",
           R"({"panels": [], "taps": [{"id": "12", "x": 0, "y": 0}]})",
           R"({"panels": [{"addr": "0026", "barcode": 7, "x": 0, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "barcode": "04C0 5B", "x": 0, "y": 0}], "taps": []})",
           R"({"panels": [{"addr": "0026", "barcode": "04C05B0000000001", "x": 0, "y": 0},
                          {"addr": "0027", "barcode": "04c05b0000000001", "x": 1, "y": 0}], "taps": []})",
       }) {
    std::string err = array_from_json(json::parse(bad), r);
    if (err.empty()) std::printf("  accepted: %s\n", bad);
    CHECK(!err.empty());
  }
}

static void test_barcode_round_trip() {
  ArrayLayout r;
  CHECK(array_from_json(json::parse(R"({"panels": [{"addr": "0026", "barcode": "04c05b0000000001", "x": 3, "y": 2},
                                                   {"addr": "0027", "x": 4, "y": 2}], "taps": []})"), r).empty());
  CHECK(r.panels[0].barcode == "04C05B0000000001");  // upper case
  CHECK(r.panels[1].barcode.empty());
  json out = array_to_json(r);
  CHECK(out["panels"][0]["barcode"] == "04C05B0000000001");
  CHECK(!out["panels"][1].contains("barcode"));  // none saved, none written
}

// A renumbering (a CCA taking over, a rediscovery) moves a panel's address,
// not where it sits: the layout follows the barcode (roof, 2026-11-14).
static void test_follows_a_renumbering() {
  ArrayLayout r;
  CHECK(array_from_json(json::parse(R"({"panels": [
      {"addr": "0002", "barcode": "04C05B0000000001", "x": 0, "y": 0},
      {"addr": "0003", "barcode": "04C05B0000000002", "x": 1, "y": 0},
      {"addr": "0009", "x": 2, "y": 0},
      {"addr": "0004", "x": 3, "y": 0},
      {"addr": "0005", "barcode": "04C05B00000000FF", "x": 4, "y": 0}], "taps": []})"), r).empty());
  std::map<std::string, std::string> now = {{"04C05B0000000001", "002B"}, {"04C05B0000000002", "0004"}};
  array_follow_barcodes(r, now);
  auto at = [&](int x) -> std::string {
    for (auto &p : r.panels) if (p.x == x) return p.addr;
    return "-";
  };
  CHECK(at(0) == "002B");
  CHECK(at(1) == "0004");
  CHECK(at(2) == "0009");  // no barcode saved: left as it was
  CHECK(at(3) == "-");     // no barcode, and its address is another panel's now: dropped
  CHECK(at(4) == "0005");  // a barcode not in the table (yet): left as it was
}

static void test_fills_in_barcodes() {
  ArrayLayout r;
  CHECK(array_from_json(json::parse(R"({"panels": [{"addr": "0002", "x": 0, "y": 0},
                                                   {"addr": "0003", "barcode": "04C05B0000000009", "x": 1, "y": 0},
                                                   {"addr": "0004", "x": 2, "y": 0}], "taps": []})"), r).empty());
  std::map<std::string, std::string> by_addr = {{"0002", "04C05B0000000001"}, {"0003", "04C05B0000000002"}};
  array_fill_barcodes(r, by_addr);
  CHECK(r.panels[0].barcode == "04C05B0000000001");
  CHECK(r.panels[1].barcode == "04C05B0000000009");  // a saved barcode is kept
  CHECK(r.panels[2].barcode.empty());                // an address not in the table
}

int main() {
  test_barcode_round_trip();
  test_follows_a_renumbering();
  test_fills_in_barcodes();
  test_round_trip();
  test_refused();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
