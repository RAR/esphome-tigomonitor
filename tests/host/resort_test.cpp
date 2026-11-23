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

// ---- the job, against fake TAPs -------------------------------------------

// TAPs as the bench's answer: the radio block (read 0D 00 00, written 0D 01
// 00, the stored copy 0D 00 01: an H1 saves on that read, a G8 only with the
// Store), the table (26 pages, 29 writes, 2B erase), 0xE1 and 2D recorded.
struct FakeTaps {
  struct One {
    uint16_t id = 0;
    RadioBlock cfg, stored;
    bool g8 = false;
    std::vector<TableRecord> table;
    std::vector<Bytes> e1, windows;
    std::vector<uint8_t> pv;
    std::string silent_from;  // a label: from that frame on, it doesn't answer
    bool silent = false;
  };
  std::map<uint16_t, One> taps;
  std::vector<std::pair<uint16_t, std::string>> sent;  // (TAP, label), in order

  static std::string label(const Request &r) {
    if (r.cmd == CMD_SESSION) return r.payload.size() == 1 && r.payload[0] == 0 ? "pvoff" : "pvon";
    if (r.cmd == CMD_STORE) return "store";
    if (r.cmd != CMD_TUNNEL || r.payload.size() < 5) return "other";
    Bytes a(r.payload.begin() + 5, r.payload.end());
    switch (r.payload[3]) {
      case 0x26: return "read";
      case 0x0D: return a.size() > 2 ? "write" : a[1] == 0x01 ? "saveread" : "check";
      case 0xE1: return "e1";
      case 0x2B: return "erase";
      case 0x29: return "list";
      case 0x2D: return "window";
    }
    return "other";
  }
  static Frame tunnel_reply(uint16_t id, uint8_t sub, uint8_t seq, const Bytes &data) {
    Frame f;
    f.dest = static_cast<uint16_t>(id | 0x8000);
    f.cmd = CMD_TUNNEL_REPLY;
    f.payload = {0x00, 0x0E, 0x00, sub, seq};
    f.payload.insert(f.payload.end(), data.begin(), data.end());
    return f;
  }
  // false: no answer.
  bool answer(uint16_t id, const Request &r, Frame &out) {
    One &t = taps[id];
    std::string l = label(r);
    sent.push_back({id, l});
    if (!t.silent_from.empty() && l == t.silent_from) t.silent = true;
    if (t.silent) return false;
    if (r.cmd == CMD_SESSION) {
      t.pv.push_back(r.payload[0]);
      out.dest = static_cast<uint16_t>(id | 0x8000);
      out.cmd = CMD_SESSION + 1;
      out.payload = r.payload;
      return true;
    }
    if (r.cmd == CMD_STORE) {
      t.stored = t.cfg;
      out.dest = static_cast<uint16_t>(id | 0x8000);
      out.cmd = CMD_STORE + 1;
      return true;
    }
    uint8_t sub = r.payload[3], seq = r.payload[4];
    Bytes a(r.payload.begin() + 5, r.payload.end());
    if (sub == 0x26) {
      uint16_t from = static_cast<uint16_t>(a[0] << 8 | a[1]);
      std::vector<TableRecord> page;
      std::vector<TableRecord> sorted = t.table;
      std::sort(sorted.begin(), sorted.end(), [](const TableRecord &x, const TableRecord &y) { return x.id() < y.id(); });
      for (const auto &rec : sorted)
        if (rec.id() >= from && page.size() < 12) page.push_back(rec);
      uint16_t first = page.empty() ? from : page[0].id();
      Bytes d = {static_cast<uint8_t>(first >> 8), static_cast<uint8_t>(first), 0x00, static_cast<uint8_t>(page.size())};
      for (const auto &rec : page) {
        d.insert(d.end(), rec.mac.begin(), rec.mac.end());
        d.push_back(static_cast<uint8_t>(rec.tuid >> 8));
        d.push_back(static_cast<uint8_t>(rec.tuid));
      }
      out = tunnel_reply(id, 0x27, seq, d);
      return true;
    }
    if (sub == 0x0D) {
      RadioBlock shown = t.cfg;
      if (a.size() > 2) {
        if (!RadioBlock::decode(Bytes(a.begin() + 2, a.end()), t.cfg)) return false;
        shown = t.cfg;
      } else if (a[1] == 0x01) {
        if (!t.g8) t.stored = t.cfg;  // an H1 saves on the stored read
        shown = t.stored;
      }
      Bytes d = shown.encode();
      d.push_back(0x00);
      out = tunnel_reply(id, 0x0E, seq, d);
      return true;
    }
    if (sub == 0xE1) {
      t.e1.push_back(a);
      out = tunnel_reply(id, 0xE2, seq, {0x00, static_cast<uint8_t>(t.cfg.channel), 0x00, 0x00});
      return true;
    }
    if (sub == 0x2B) {
      t.table.clear();
      out = tunnel_reply(id, 0x2C, seq, {0x00});
      return true;
    }
    if (sub == 0x29) {
      for (size_t k = 0; k < a[0]; k++) {
        const uint8_t *p = &a[1 + 10 * k];
        t.table.push_back({Bytes(p, p + 8), static_cast<uint16_t>(p[8] << 8 | p[9])});
      }
      out = tunnel_reply(id, 0x2A, seq, {0x00});
      return true;
    }
    if (sub == 0x2D) {
      t.windows.push_back(a);
      out = tunnel_reply(id, 0x2F, seq, {0x01, 0x03, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
      return true;
    }
    return false;
  }
};

// Round robin over the TAPs, one request each turn, as the master serves
// them; the saver answers wants_save().
static void drive(ResortJob &job, FakeTaps &bus, const std::vector<uint16_t> &order, bool save_ok = true) {
  uint8_t seq = 0;
  for (int turn = 0; turn < 20000 && !job.finished(); turn++) {
    if (job.wants_save()) job.saved(save_ok);
    uint16_t id = order[turn % order.size()];
    Request r = job.due(id, static_cast<uint32_t>(turn), seq);
    if (!r.cmd) continue;
    Frame f;
    bool ok = bus.answer(id, r, f);
    job.on_reply(id, r, ok, f, static_cast<uint32_t>(turn));
  }
}

// Three TAPs: an H1 with A and B (20), a G8 with C and D (20), a new G8 with none.
static FakeTaps roof(std::vector<ResortTap> &taps) {
  FakeTaps bus;
  uint16_t ids[3] = {0x1201, 0x1202, 0x1203};
  uint16_t chans[3] = {15, 18, 20}, pans[3] = {0x7E21, 0x5A5A, 0x1A2B};
  for (int k = 0; k < 3; k++) {
    FakeTaps::One t;
    t.id = ids[k];
    t.cfg = t.stored = block(chans[k], pans[k], 0x30);
    t.g8 = k > 0;
    if (k < 2)
      for (int n = 0; n < 20; n++) t.table.push_back(rec(static_cast<uint8_t>(k * 20 + n), static_cast<uint16_t>(0x10 + k * 20 + n), true));
    bus.taps[ids[k]] = t;
    taps.push_back({ids[k], t.cfg, t.g8});
  }
  return bus;
}
static std::vector<RadioBlock> targets(const std::vector<ResortTap> &taps) {
  int n = 0;
  return resort_networks(taps, [&n]() { return static_cast<uint8_t>(0x21 + 0x17 * n++); });
}
static std::vector<std::string> labels(const FakeTaps &bus, bool reads = false) {
  std::vector<std::string> out;
  for (const auto &s : bus.sent)
    if (reads || s.second != "read") out.push_back(s.second);
  return out;
}
static std::vector<std::string> repeat(const std::vector<std::pair<std::string, int>> &runs) {
  std::vector<std::string> out;
  for (const auto &r : runs)
    for (int k = 0; k < r.second; k++) out.push_back(r.first);
  return out;
}

static void test_resort_order_three_taps() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  std::vector<RadioBlock> want = targets(taps);
  ResortJob job(taps, want, false, false);
  drive(job, bus, {0x1201, 0x1202, 0x1203});
  CHECK(job.finished() && job.ok());
  CHECK(job.records().size() == 40);
  // 40 records: 4 writes of 10 to each TAP, one TAP at a time.
  CHECK(labels(bus) == repeat({{"pvoff", 3}, {"write", 3}, {"pvon", 3}, {"saveread", 3}, {"check", 3}, {"store", 2},
                               {"saveread", 2}, {"e1", 3}, {"erase", 3}, {"list", 12}, {"window", 3}}));
  for (auto &kv : bus.taps) {
    const auto &t = kv.second;
    CHECK(t.table.size() == 40);
    for (const auto &r : t.table) CHECK(!r.joined());
    CHECK(t.windows.size() == 1 && t.windows[0].size() == 9 && t.windows[0][5] == 0x00 && t.windows[0][6] == 40 &&
          t.windows[0][7] == 0x01 && t.windows[0][8] == 0x01);
    CHECK(t.pv == (std::vector<uint8_t>{0x00, 0x01}));
  }
  for (size_t k = 0; k < 3; k++) {
    CHECK(bus.taps[taps[k].tap].cfg == want[k]);
    CHECK(bus.taps[taps[k].tap].stored.same_settings(want[k]));
  }
  CHECK(!job.pv_off_sent());
  CHECK(job.began());
  CHECK(job.turn_tap() == 0);
}

// Whose turn it is (the master's check for a TAP that is lost), and whether
// the first PV off went out (the file's keep-or-delete on a failure).
static void test_resort_turn_and_began() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  ResortJob job(taps, targets(taps), false, false);
  CHECK(job.turn_tap() == 0x1201);
  CHECK(!job.began());
  uint8_t seq = 0;
  for (int turn = 0; turn < 2000 && !job.wants_save(); turn++) {
    uint16_t id = taps[turn % 3].tap;
    Request r = job.due(id, 0, seq);
    if (!r.cmd) continue;
    Frame f;
    bool ok = bus.answer(id, r, f);
    job.on_reply(id, r, ok, f, 0);
  }
  CHECK(job.turn_tap() == 0);  // waiting on the save, no TAP's turn
  job.saved(true);
  CHECK(job.turn_tap() == 0x1201 && !job.began());
  Request r = job.due(0x1201, 0, seq);
  CHECK(r.cmd == CMD_SESSION && job.began());
  ResortJob none(taps, {}, false, false);
  CHECK(none.finished() && none.turn_tap() == 0);
}

static void test_resort_e1_names_the_others_new_networks() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  std::vector<RadioBlock> want = targets(taps);
  ResortJob job(taps, want, false, false);
  drive(job, bus, {0x1201, 0x1202, 0x1203});
  const auto &e1 = bus.taps[0x1201].e1;
  CHECK(e1.size() == 1);
  if (e1.size() != 1) return;
  std::vector<KnownNetwork> others;
  for (size_t k : {1u, 2u}) {
    KnownNetwork n;
    n.channel = static_cast<uint8_t>(want[k].channel);
    n.pan = want[k].pan;
    n.key = want[k].key;
    others.push_back(n);
  }
  CHECK(e1[0] == known_networks_args(others));
  CHECK(e1[0][0] == 2);
  CHECK(bus.taps[0x1203].e1.size() == 1 && bus.taps[0x1203].e1[0][4] == 15 && bus.taps[0x1203].e1[0][24] == 18);
}

static void test_resort_save_comes_before_the_first_network_frame() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  ResortJob job(taps, targets(taps), false, false);
  uint8_t seq = 0;
  // Read the tables, then nothing until saved.
  for (int turn = 0; turn < 2000 && !job.wants_save(); turn++) {
    uint16_t id = taps[turn % 3].tap;
    Request r = job.due(id, 0, seq);
    if (!r.cmd) continue;
    Frame f;
    bool ok = bus.answer(id, r, f);
    job.on_reply(id, r, ok, f, 0);
  }
  CHECK(job.wants_save());
  for (const auto &t : taps) CHECK(!job.due(t.tap, 0, seq).cmd);
  CHECK(labels(bus).empty());  // reads only
  job.saved(false);
  CHECK(job.finished() && !job.ok());
  CHECK(job.message() == "couldn't save the old networks; nothing was changed");
  for (const auto &t : taps) CHECK(!job.due(t.tap, 0, seq).cmd);
  CHECK(labels(bus).empty());
}

static void test_resort_fails_after_pv_off_still_sends_pv_on() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  bus.taps[0x1202].silent_from = "write";
  ResortJob job(taps, targets(taps), false, false);
  drive(job, bus, {0x1201, 0x1202, 0x1203});
  CHECK(job.finished() && !job.ok());
  CHECK(job.failed_tap() == 0x1202);
  CHECK(job.message().find("TAP 1202") != std::string::npos);
  CHECK(labels(bus) == repeat({{"pvoff", 3}, {"write", 2}, {"pvon", 3}}));
  CHECK(bus.taps[0x1201].pv == (std::vector<uint8_t>{0x00, 0x01}));
  CHECK(bus.taps[0x1203].pv == (std::vector<uint8_t>{0x00, 0x01}));
  CHECK(!job.pv_off_sent());
}

static void test_resort_abort_after_pv_off_still_sends_pv_on() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  ResortJob job(taps, targets(taps), false, false);
  uint8_t seq = 0;
  bool aborted = false;
  for (int turn = 0; turn < 20000 && !job.finished(); turn++) {
    if (job.wants_save()) job.saved(true);
    uint16_t id = taps[turn % 3].tap;
    Request r = job.due(id, 0, seq);
    if (!r.cmd) continue;
    if (!aborted && FakeTaps::label(r) == "write") {  // the gate refuses the first network write
      aborted = true;
      bus.sent.push_back({id, "refused"});
      job.abort("refused to send command 2831");
      continue;
    }
    Frame f;
    bool ok = bus.answer(id, r, f);
    job.on_reply(id, r, ok, f, 0);
  }
  CHECK(aborted);
  CHECK(job.finished() && !job.ok());
  CHECK(job.message() == "refused to send command 2831");
  CHECK(labels(bus) == repeat({{"pvoff", 3}, {"refused", 1}, {"pvon", 3}}));
}

static bool has_hex_run(const std::string &s, const Bytes &b) {
  // Any four bytes of b in a row, as hex, upper or lower case, with or without spaces.
  for (size_t k = 0; k + 2 <= b.size(); k++) {
    char up[8], lo[8], sp[8];
    std::snprintf(up, sizeof(up), "%02X%02X", b[k], b[k + 1]);
    std::snprintf(lo, sizeof(lo), "%02x%02x", b[k], b[k + 1]);
    std::snprintf(sp, sizeof(sp), "%02X %02X", b[k], b[k + 1]);
    if (s.find(up) != std::string::npos || s.find(lo) != std::string::npos || s.find(sp) != std::string::npos) return true;
  }
  return false;
}

static void test_resort_dry_run_has_no_key() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  std::vector<RadioBlock> want = targets(taps);
  ResortJob job(taps, want, true, false);
  drive(job, bus, {0x1201, 0x1202, 0x1203});
  CHECK(job.finished() && job.ok());
  CHECK(labels(bus).empty());  // read only
  CHECK(job.message().find("dry run") == 0);
  const auto &d = job.dry_text();
  CHECK(d.size() == 11);  // two G8s: two Store lines
  if (d.size() == 11) {
    CHECK(d[0] == "PV off (0B00 00) to 3 TAPs");
    CHECK(d[1] == "0D 01 00 + block (channel 15, a new network ID, the new shared key) to TAP 1201");
    CHECK(d[5] == "0D 00 01 (the save) and 0D 00 00 (the check) to each TAP");
    CHECK(d[6] == "0012 (the Store), then 0D 00 01, to TAP 1202");
    CHECK(d[8] == "0xE1 to each TAP, naming the 2 others");
    CHECK(d[7] == "0012 (the Store), then 0D 00 01, to TAP 1203");
    CHECK(d[9] == "2B erase, then 40 listed (not joined) on each TAP");
    CHECK(d[10] == "2D window ending 01 01 on each TAP");
  }
  std::string all = job.message();
  for (const auto &l : d) all += "\n" + l;
  CHECK(!has_hex_run(all, Bytes(want[0].key.begin(), want[0].key.end())));
  for (const auto &b : want) CHECK(!has_hex_run(all, {static_cast<uint8_t>(b.pan >> 8), static_cast<uint8_t>(b.pan)}));
  for (const auto &t : taps) CHECK(!has_hex_run(all, {static_cast<uint8_t>(t.now.pan >> 8), static_cast<uint8_t>(t.now.pan)}));
}

static void test_resort_put_back_skips_the_save() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  std::vector<RadioBlock> old;
  for (const auto &t : taps) old.push_back(t.now);
  ResortJob job(taps, old, false, true);
  bool asked = false;
  uint8_t seq = 0;
  for (int turn = 0; turn < 20000 && !job.finished(); turn++) {
    asked = asked || job.wants_save();
    uint16_t id = taps[turn % 3].tap;
    Request r = job.due(id, 0, seq);
    if (!r.cmd) continue;
    Frame f;
    bool ok = bus.answer(id, r, f);
    job.on_reply(id, r, ok, f, 0);
  }
  CHECK(!asked);
  CHECK(job.finished() && job.ok());
  CHECK(job.message().find("old networks are back") != std::string::npos);
  CHECK(labels(bus).front() == "pvoff");
}

static void test_resort_list_conflict_stops_before_anything() {
  std::vector<ResortTap> taps;
  FakeTaps bus = roof(taps);
  bus.taps[0x1203].table.push_back(rec(0x99, 0x10, true));  // 0010 is 1201's under another MAC
  ResortJob job(taps, targets(taps), false, false);
  drive(job, bus, {0x1201, 0x1202, 0x1203});
  CHECK(job.finished() && !job.ok());
  CHECK(job.message().find("lists 0010") != std::string::npos);
  CHECK(labels(bus).empty());
}

int main() {
  test_resort_networks_keep_all_but_pan_and_key();
  test_resort_networks_no_free_pan();
  test_resort_list_merges_and_marks_listed();
  test_resort_list_conflicts();
  test_resort_trim();
  test_resort_missing();
  test_resort_order_three_taps();
  test_resort_turn_and_began();
  test_resort_e1_names_the_others_new_networks();
  test_resort_save_comes_before_the_first_network_frame();
  test_resort_fails_after_pv_off_still_sends_pv_on();
  test_resort_abort_after_pv_off_still_sends_pv_on();
  test_resort_dry_run_has_no_key();
  test_resort_put_back_skips_the_save();
  test_resort_list_conflict_stops_before_anything();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
