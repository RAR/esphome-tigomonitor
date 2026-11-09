// Host test for components/tigo_monitor/tigo_layout.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/layout_test.cpp -o /tmp/layout_test && /tmp/layout_test
//
// Barcodes here are made up; none belong to a real installation.

#include "tigo_layout.h"

#include <climits>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::layout;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const char *BC[] = {"04C05B0000000A01", "04C05B0000000A02", "04C05B0000000A03", "04C05B0000000A04"};

static bool known(const std::string &bc) {
  for (const char *b : BC)
    if (bc == b) return true;
  return false;
}

// Inverter 1: MPPT 1 holds string A (A01, A02); MPPT 2 holds string B (A03, A04).
static Layout good() {
  Layout l;
  InverterSpec inv;
  inv.name = "Inverter 1";
  MpptSpec m1, m2;
  m1.label = "MPPT 1";
  m2.label = "MPPT 2";
  StringSpec a, b;
  a.label = "A";
  a.panels = {BC[0], BC[1]};
  b.label = "B";
  b.panels = {BC[2], BC[3]};
  m1.strings.push_back(a);
  m2.strings.push_back(b);
  inv.mppts = {m1, m2};
  l.inverters.push_back(inv);
  return l;
}

static void test_panel_names_follow_position() {
  std::printf("panel names follow position\n");
  StringSpec s;
  s.label = "A";
  CHECK(panel_name(s, 0) == "A1");
  CHECK(panel_name(s, 2) == "A3");
  s.display_label = "East";
  CHECK(panel_name(s, 0) == "East1");
}

static void test_validate_accepts_a_good_layout() {
  std::printf("validate accepts a good layout\n");
  CHECK(validate(good(), known).empty());
  Layout empty;
  CHECK(validate(empty, known).empty());
}

static bool mentions(const std::string &msg, const std::string &what) {
  return !msg.empty() && msg.find(what) != std::string::npos;
}

static void test_validate_rejects() {
  std::printf("validate rejects\n");
  {
    Layout l = good();
    l.inverters.push_back(l.inverters[0]);
    l.inverters[1].mppts.clear();
    CHECK(mentions(validate(l, known), "Inverter 1"));
  }
  {
    Layout l = good();
    InverterSpec two;
    two.name = "Inverter 2";
    MpptSpec m;
    m.label = "MPPT 1";
    two.mppts.push_back(m);
    l.inverters.push_back(two);
    CHECK(mentions(validate(l, known), "MPPT 1"));
  }
  {
    Layout l = good();
    l.inverters[0].mppts[1].strings[0].label = "A";
    CHECK(mentions(validate(l, known), "\"A\""));
  }
  {
    Layout l = good();
    l.inverters[0].mppts[1].strings[0].panels.push_back(BC[0]);
    CHECK(mentions(validate(l, known), BC[0]));
  }
  {
    Layout l = good();
    l.inverters[0].mppts[0].strings[0].panels.push_back("04C05B0000000FFF");
    CHECK(mentions(validate(l, known), "04C05B0000000FFF"));
  }
  {
    Layout l = good();
    l.inverters[0].name = "";
    CHECK(!validate(l, known).empty());
  }
  {
    Layout l = good();
    l.inverters[0].mppts[0].strings[0].display_label = "Ea|st";
    CHECK(mentions(validate(l, known), "Ea|st"));
  }
  {
    // 31 chars + "10" is 33 bytes: too long for the ESP32's node slot.
    Layout l = good();
    StringSpec &s = l.inverters[0].mppts[0].strings[0];
    s.display_label = std::string(31, 'x');
    s.panels.clear();
    for (int i = 0; i < 10; i++) s.panels.push_back("04C05B00000010" + std::to_string(10 + i));
    auto any = [](const std::string &) { return true; };
    CHECK(!validate(l, any).empty());
    s.panels.resize(9);  // "xxx…9" is 32 bytes: fine
    CHECK(validate(l, any).empty());
  }
}

static void test_assignments_carry_the_tree() {
  std::printf("assignments carry the tree\n");
  Layout l = good();
  l.inverters[0].name = "Inv 1";
  l.inverters[0].mppts[1].strings[0].display_label = "West";
  auto as = assignments(l);
  CHECK(as.size() == 4);
  bool found = false;
  for (const auto &a : as) {
    if (a.barcode != BC[3]) continue;
    found = true;
    CHECK(a.label == "West2");
    CHECK(a.string_label == "B");
    CHECK(a.mppt_label == "MPPT 2");
  }
  CHECK(found);
}

static void test_position_of() {
  std::printf("position_of\n");
  CHECK(position_of("A3") == 3);
  CHECK(position_of("East12") == 12);
  CHECK(position_of("Panel") == INT_MAX);
  CHECK(position_of("") == INT_MAX);
}

static void test_diff_only_reports_real_changes() {
  std::printf("diff only reports real changes\n");
  auto cur = assignments(good());
  CHECK(diff(cur, cur).empty());
  auto moved = cur;
  moved[0].string_label = "B";
  moved[0].label = "B3";
  auto d = diff(cur, moved);
  CHECK(d.size() == 1);
  CHECK(d.size() == 1 && d[0].barcode == BC[0] && d[0].now.label == "A1" && d[0].then.label == "B3");
  std::vector<Assignment> fresh = {{"04C05B0000000A09", "C1", "C", "MPPT 3"}};
  d = diff(cur, fresh);
  CHECK(d.size() == 1 && d[0].now.label.empty() && d[0].now.string_label.empty() && d[0].then.label == "C1");
  std::vector<Assignment> partial(cur.begin(), cur.begin() + 1);
  CHECK(diff(cur, partial).empty());
}

static void test_fingerprint_is_order_independent() {
  std::printf("fingerprint is order independent\n");
  auto a = assignments(good());
  auto b = std::vector<Assignment>(a.rbegin(), a.rend());
  InverterMppts inv = {{"Inverter 1", {"MPPT 1", "MPPT 2"}}};
  CHECK(fingerprint(a, inv) == fingerprint(b, inv));
  b[0].label = "Z9";
  CHECK(fingerprint(a, inv) != fingerprint(b, inv));
  InverterMppts inv2 = {{"Inverter 1", {"MPPT 1"}}};
  CHECK(fingerprint(a, inv) != fingerprint(a, inv2));
}

static void test_decide() {
  std::printf("decide\n");
  CHECK(decide(false, true, 1, 0) == Offer::apply);
  CHECK(decide(false, false, 1, 0) == Offer::apply);
  CHECK(decide(true, false, 1, 0) == Offer::unchanged);
  CHECK(decide(true, true, 7, 7) == Offer::declined);
  CHECK(decide(true, true, 8, 7) == Offer::hold);
}

static void test_skeleton_round_trips() {
  std::printf("skeleton round trips\n");
  Layout l = good();
  l.inverters[0].display_name = "South Roof";
  l.inverters[0].mppts[0].strings[0].display_label = "East";
  l.inverters[0].mppts[0].strings[0].rating_w = 400;
  StringSpec empty_string;
  empty_string.label = "C";
  l.inverters[0].mppts[1].strings.push_back(empty_string);
  MpptSpec empty_mppt;
  empty_mppt.label = "MPPT 3";
  l.inverters[0].mppts.push_back(empty_mppt);
  InverterSpec bare;
  bare.name = "Inverter 2";
  l.inverters.push_back(bare);

  // One turned-down layout per source, so a recurring CCA sync and the
  // YAML read at every boot don't overwrite each other's answer.
  Declined declined = {{"cca", 0xDEADBEEF}, {"yaml", 0x12345678}};
  std::string enc = encode_skeleton(l, true, declined);
  CHECK(enc.size() <= kMaxSkeleton);
  Layout out;
  bool manual = false;
  Declined fp;
  CHECK(decode_skeleton(enc, out, manual, fp));
  CHECK(manual && fp == declined);
  Declined none;
  CHECK(decode_skeleton(encode_skeleton(l, false, none), out, manual, fp) && !manual && fp.empty());
  CHECK(decode_skeleton(encode_skeleton(l, true, declined), out, manual, fp));
  CHECK(out.inverters.size() == 2);
  if (out.inverters.size() == 2) {
    const auto &i0 = out.inverters[0];
    CHECK(i0.name == "Inverter 1" && i0.display_name == "South Roof");
    CHECK(i0.mppts.size() == 3);
    if (i0.mppts.size() == 3) {
      CHECK(i0.mppts[0].label == "MPPT 1" && i0.mppts[2].label == "MPPT 3" && i0.mppts[2].strings.empty());
      CHECK(i0.mppts[0].strings.size() == 1 && i0.mppts[0].strings[0].display_label == "East" &&
            i0.mppts[0].strings[0].rating_w == 400 && i0.mppts[0].strings[0].panels.empty());
      CHECK(i0.mppts[1].strings.size() == 2 && i0.mppts[1].strings[1].label == "C");
    }
    CHECK(out.inverters[1].name == "Inverter 2" && out.inverters[1].mppts.empty());
  }
  CHECK(!decode_skeleton("garbage", out, manual, fp));
  CHECK(!decode_skeleton("", out, manual, fp));
}

static void test_build_tree() {
  std::printf("build_tree\n");
  // Skeleton: Inverter 1 / MPPT 1 / strings A (has panels) and B (empty).
  Layout sk;
  InverterSpec inv;
  inv.name = "Inverter 1";
  MpptSpec m1;
  m1.label = "MPPT 1";
  StringSpec a, b;
  a.label = "A";
  b.label = "B";
  m1.strings = {a, b};
  inv.mppts = {m1};
  sk.inverters = {inv};
  std::vector<NodeView> nodes = {
      {BC[1], "0003", "A2", "A", "MPPT 1"},
      {BC[0], "0002", "A1", "A", "MPPT 1"},
      {BC[2], "0004", "W1", "West", "MPPT 7"},  // a string and MPPT the skeleton lacks
      {BC[3], "0005", "", "", ""},              // in no string
      {"", "0006", "", "", ""},                 // no barcode yet: left out
      {"04C05B0000000A05", "0007", "Q", "Q", ""},  // a string with no MPPT
  };
  int filled = 0;
  Tree t = build_tree(sk, nodes, [&filled](StringSpec &s) {
    if (s.label == "West") s.display_label = "Back";
    filled++;
  });
  CHECK(t.layout.inverters.size() == 1);
  CHECK(t.layout.inverters[0].mppts.size() == 1);
  CHECK(t.layout.inverters[0].mppts[0].strings.size() == 2);
  CHECK(t.panels["A"].size() == 2 && t.panels["A"][0].barcode == BC[0] && t.panels["A"][1].barcode == BC[1]);
  CHECK(t.panels["B"].empty());
  CHECK(t.unplaced.size() == 2);
  if (t.unplaced.size() == 2) {
    CHECK(t.unplaced[0].label == "MPPT 7" && t.unplaced[0].strings.size() == 1 &&
          t.unplaced[0].strings[0].label == "West" && t.unplaced[0].strings[0].display_label == "Back");
    CHECK(t.unplaced[1].label == "" && t.unplaced[1].strings.size() == 1 && t.unplaced[1].strings[0].label == "Q");
  }
  CHECK(filled == 2);  // only strings the skeleton didn't already describe
  CHECK(t.unassigned.size() == 1 && t.unassigned[0].barcode == BC[3]);
}

// Each inverter numbers its own MPPTs: the label stays unique across the site
// (nodes carry it), and the name shown need only be unique in its inverter.
static void test_mppt_display_names() {
  std::printf("mppt display names\n");
  Layout l = good();
  InverterSpec two;
  two.name = "Inverter 2";
  MpptSpec m3;
  m3.label = "MPPT 3";
  m3.display_label = "MPPT 1";
  two.mppts.push_back(m3);
  l.inverters.push_back(two);
  CHECK(validate(l, known).empty());
  CHECK(mppt_name(l.inverters[1].mppts[0]) == "MPPT 1");
  CHECK(mppt_name(l.inverters[0].mppts[1]) == "MPPT 2");
  {
    Layout bad = l;
    bad.inverters[0].mppts[1].display_label = "MPPT 1";  // shown twice in Inverter 1
    CHECK(mentions(validate(bad, known), "MPPT 1"));
  }
  {
    Layout bad = l;
    bad.inverters[1].mppts[0].display_label = "MP|PT";
    CHECK(mentions(validate(bad, known), "MP|PT"));
  }
  {
    Layout bad = l;
    bad.inverters[1].mppts[0].label = "MPPT 1";  // labels still unique site-wide
    CHECK(!validate(bad, known).empty());
  }
  // The name rides the skeleton; a skeleton saved before it still reads.
  Layout out;
  bool manual = false;
  Declined fp;
  CHECK(decode_skeleton(encode_skeleton(l, true, {}), out, manual, fp));
  CHECK(out.inverters.size() == 2 && out.inverters[1].mppts.size() == 1 &&
        out.inverters[1].mppts[0].label == "MPPT 3" && out.inverters[1].mppts[0].display_label == "MPPT 1");
  CHECK(decode_skeleton("v1|1|\nI|Inverter 1|\nM|MPPT 1\nS|A||0\n", out, manual, fp));
  CHECK(out.inverters.size() == 1 && out.inverters[0].mppts.size() == 1 &&
        out.inverters[0].mppts[0].label == "MPPT 1" && out.inverters[0].mppts[0].display_label.empty() &&
        out.inverters[0].mppts[0].strings.size() == 1);
}

int main() {
  test_panel_names_follow_position();
  test_validate_accepts_a_good_layout();
  test_validate_rejects();
  test_assignments_carry_the_tree();
  test_position_of();
  test_diff_only_reports_real_changes();
  test_fingerprint_is_order_independent();
  test_decide();
  test_skeleton_round_trips();
  test_build_tree();
  test_mppt_display_names();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
