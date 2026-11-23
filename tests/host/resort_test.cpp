// Host test for resort.h: the re-sort that lets every optimizer choose its TAP
// (docs/superpowers/specs/2026-11-23-tap-resort-design.md). Its new networks,
// its list of every optimizer, the trim after the joins, and who is missing.
//
// Keys, PANs and MACs are made up. No test prints a key or a PAN: a failure
// names the line, never the bytes.

#include "resort.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::master;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static RadioBlock block(uint16_t channel, uint16_t pan, uint8_t key0) {
  RadioBlock b;
  b.channel = channel;
  b.pan = pan;
  b.map = {0x14, 0x02, 0x02, 0x01, 0x1C, 0x00, 0x00, 0x00, 0x01, 0x00};
  b.slt = 0x5A;
  for (size_t i = 0; i < b.key.size(); i++) b.key[i] = static_cast<uint8_t>(key0 + i);
  b.slm = 0x3C;
  return b;
}

static Bytes mac(uint8_t last) { return {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x02, last}; }
static TableRecord rec(uint8_t last, uint16_t id, bool joined) {
  return {mac(last), static_cast<uint16_t>(joined ? id : (0x8000 | id))};
}

// ---- the new networks ----------------------------------------------------

static void test_resort_networks_keep_all_but_pan_and_key() {
  std::vector<ResortTap> taps = {{0x1201, block(15, 0x7E21, 0x30), false},
                                 {0x1202, block(18, 0x5A5A, 0x30), true},
                                 {0x1203, block(20, 0x1A2B, 0x30), true}};
  taps[1].now.wide = true;
  taps[1].now.extra = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  taps[2].now.slt = 0x50;
  int n = 0;
  auto draw = [&n]() { return static_cast<uint8_t>(0x11 + 0x13 * n++); };
  std::vector<RadioBlock> got = resort_networks(taps, draw);
  CHECK(got.size() == 3);
  if (got.size() != 3) return;
  std::set<uint16_t> pans;
  for (size_t i = 0; i < 3; i++) {
    const RadioBlock &was = taps[i].now, &now = got[i];
    CHECK(now.channel == was.channel);
    CHECK(now.map == was.map);
    CHECK(now.slt == was.slt && now.slm == was.slm && now.optimization == was.optimization);
    CHECK(now.wide == was.wide && now.extra == was.extra);
    CHECK(now.pan != 0x0000 && now.pan != 0xFFFF);
    CHECK(now.pan != 0x7E21 && now.pan != 0x5A5A && now.pan != 0x1A2B);
    pans.insert(now.pan);
    CHECK(now.key == got[0].key);
    CHECK(now.key != was.key);
    for (uint8_t b : now.key) CHECK(b != 0x00 && b != 0xFF);
  }
  CHECK(pans.size() == 3);
}

// pan_for steps 16 times by 0x1000 from its seed; a draw that always gives
// the same seed, with all 16 of those in use, leaves no PAN.
static void test_resort_networks_no_free_pan() {
  // The seed is {0x42, 0x42}: pan_for starts at 0x4242. Every step of that
  // ladder is some TAP's PAN now.
  std::vector<ResortTap> taps;
  for (uint16_t i = 0; i < 16; i++)
    taps.push_back({static_cast<uint16_t>(0x1201 + i), block(15, static_cast<uint16_t>(0x4242 + 0x1000 * i), 0x30), false});
  auto draw = []() { return static_cast<uint8_t>(0x42); };
  CHECK(resort_networks(taps, draw).empty());
}

// ---- the list of every optimizer -----------------------------------------

static void test_resort_list_merges_and_marks_listed() {
  std::map<uint16_t, std::vector<TableRecord>> tables;
  tables[0x1201] = {rec(1, 0x10, true), rec(2, 0x11, true)};
  tables[0x1202] = {rec(3, 0x12, true), rec(2, 0x11, false)};
  std::vector<TableRecord> out;
  std::string why;
  CHECK(resort_list(tables, out, why));
  CHECK(why.empty());
  CHECK(out.size() == 3);
  if (out.size() != 3) return;
  CHECK(out[0].mac == mac(1) && out[0].id() == 0x10);
  CHECK(out[1].mac == mac(2) && out[1].id() == 0x11);
  CHECK(out[2].mac == mac(3) && out[2].id() == 0x12);
  for (const auto &r : out) CHECK(!r.joined());
}

static void test_resort_list_conflicts() {
  std::vector<TableRecord> out;
  std::string why;
  {  // one id, two MACs
    std::map<uint16_t, std::vector<TableRecord>> t;
    t[0x1201] = {rec(1, 0x10, true)};
    t[0x1202] = {rec(2, 0x10, true)};
    CHECK(!resort_list(t, out, why));
    CHECK(why.find("1201") != std::string::npos && why.find("1202") != std::string::npos);
    CHECK(why.find("0010") != std::string::npos);
  }
  {  // one MAC, two ids
    std::map<uint16_t, std::vector<TableRecord>> t;
    t[0x1201] = {rec(1, 0x10, true)};
    t[0x1202] = {rec(1, 0x11, false)};
    why.clear();
    CHECK(!resort_list(t, out, why));
    CHECK(!why.empty());
  }
  {  // nothing listed anywhere
    std::map<uint16_t, std::vector<TableRecord>> t;
    t[0x1201] = {};
    t[0x1202] = {};
    why.clear();
    CHECK(!resort_list(t, out, why));
    CHECK(!why.empty());
  }
}

// ---- the trim and who is missing ------------------------------------------

static bool has(const std::vector<Bytes> &v, const Bytes &m) { return std::find(v.begin(), v.end(), m) != v.end(); }

static void test_resort_trim() {
  std::map<uint16_t, std::vector<TableRecord>> t;
  t[0x1201] = {rec(1, 0x10, true), rec(2, 0x11, false), rec(3, 0x12, true)};
  t[0x1202] = {rec(1, 0x10, false), rec(2, 0x11, true), rec(3, 0x12, true)};
  t[0x1203] = {rec(1, 0x10, false), rec(2, 0x11, false)};
  auto trim = resort_trim(t);
  CHECK(trim[0x1201].size() == 1 && has(trim[0x1201], mac(2)));
  CHECK(trim[0x1202].size() == 1 && has(trim[0x1202], mac(1)));
  CHECK(trim[0x1203].size() == 2 && has(trim[0x1203], mac(1)) && has(trim[0x1203], mac(2)));
  // mac 3, joined on two TAPs, is taken off neither.
  for (auto &kv : trim) CHECK(!has(kv.second, mac(3)));
}

static void test_resort_missing() {
  std::map<uint16_t, std::vector<TableRecord>> t;
  t[0x1201] = {rec(1, 0x10, true), rec(2, 0x11, false)};
  t[0x1202] = {rec(2, 0x11, false), rec(3, 0x12, false)};
  auto miss = resort_missing(t, {mac(1), mac(2), mac(3)});
  CHECK(miss.size() == 2);
  if (miss.size() != 2) return;
  CHECK(miss[0].mac == mac(2) && miss[0].id() == 0x11);
  CHECK(miss[1].mac == mac(3) && miss[1].id() == 0x12);
  // A waited MAC on no table at all is missing too, at id 0.
  auto gone = resort_missing(t, {mac(9)});
  CHECK(gone.size() == 1 && gone[0].mac == mac(9) && gone[0].id() == 0);
}

int main() {
  test_resort_networks_keep_all_but_pan_and_key();
  test_resort_networks_no_free_pan();
  test_resort_list_merges_and_marks_listed();
  test_resort_list_conflicts();
  test_resort_trim();
  test_resort_missing();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
