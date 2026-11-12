// Host test for linux/src/pgw.h: which optimizers a TAP leans on for the
// far part of the array (the 0x41 PGW streams 01 and 02), by our own rule:
// the farthest from that TAP on the roof (neighbours only break ties); with
// no roof, the worst loss; a named list overrides both.
#include "pgw.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

// The owner's roof, in cells: B a far face of two columns (B1-B6 at x0,
// y0-5; B7-B10 at x1, y2-5), A two rows of five below, C to A's right,
// D two columns by the TAP at the far corner.
static Roof owners_roof(std::vector<PgwMember> &members) {
  Roof r;
  uint16_t t = 2;
  auto put = [&](const std::string &name, int x, int y) {
    char a[5];
    std::snprintf(a, sizeof(a), "%04X", t);
    r.panels.push_back({a, x, y, true});
    members.push_back({t, name, 0.02});
    t++;
  };
  for (int i = 0; i < 6; i++) put("B" + std::to_string(i + 1), 0, i);
  for (int i = 0; i < 4; i++) put("B" + std::to_string(i + 7), 1, i + 2);
  for (int i = 0; i < 5; i++) put("A" + std::to_string(i + 1), i, 8);
  for (int i = 0; i < 5; i++) put("A" + std::to_string(i + 6), i, 9);
  for (int i = 0; i < 6; i++) put("C" + std::to_string(i + 1), 6 + i, 8);
  for (int i = 0; i < 4; i++) put("C" + std::to_string(i + 7), 6 + i, 9);
  for (int i = 0; i < 5; i++) put("D" + std::to_string(i + 1), 13, 10 + i);
  for (int i = 0; i < 5; i++) put("D" + std::to_string(i + 6), 14, 10 + i);
  r.taps.push_back({"1201", 15, 15});
  return r;
}

static std::vector<std::string> names(const std::vector<uint16_t> &pool, const std::vector<PgwMember> &m) {
  std::vector<std::string> out;
  for (uint16_t t : pool)
    for (const auto &x : m)
      if (x.tuid == t) out.push_back(x.name);
  return out;
}

static void test_the_roof_picks_the_far_face() {
  std::vector<PgwMember> m;
  Roof r = owners_roof(m);
  std::string why;
  auto pool = pgw_pool(r, "1201", m, {}, why);
  CHECK(why == "roof");
  CHECK(pool.size() == PGW_POOL);
  // The four farthest, all of B: B1-B4, farthest first.
  CHECK((names(pool, m) == std::vector<std::string>{"B1", "B2", "B3", "B4"}));
}

// An A-frame: B's two columns on opposite faces, drawn with an empty column
// for the ridge. A dense block's corner (A1, A6) has more neighbours than
// any of B, but B is farther, and wins.
static void test_a_ridge_gap_doesnt_hand_it_to_a_dense_corner() {
  std::vector<PgwMember> m;
  Roof r;
  uint16_t t = 2;
  auto put = [&](const std::string &name, int x, int y) {
    char a[5];
    std::snprintf(a, sizeof(a), "%04X", t);
    r.panels.push_back({a, x, y, true});
    m.push_back({t++, name, 0.02});
  };
  for (int i = 0; i < 6; i++) put("B" + std::to_string(i + 1), 1, i);
  for (int i = 0; i < 4; i++) put("B" + std::to_string(i + 7), 3, i + 2);
  for (int i = 0; i < 5; i++) put("A" + std::to_string(i + 1), i, 8);
  for (int i = 0; i < 5; i++) put("A" + std::to_string(i + 6), i, 9);
  r.taps.push_back({"1201", 14, 11});
  std::string why;
  auto pool = pgw_pool(r, "1201", m, {}, why);
  CHECK((names(pool, m) == std::vector<std::string>{"B1", "B2", "B3", "B4"}));
}

static void test_only_this_taps_optimizers() {
  std::vector<PgwMember> m;
  Roof r = owners_roof(m);
  // A second TAP on top of B: B is now near it, and nothing of B reports
  // through 1201 any more.
  std::vector<PgwMember> through_1201;
  for (const auto &x : m)
    if (x.name[0] != 'B') through_1201.push_back(x);
  std::string why;
  auto pool = pgw_pool(r, "1201", through_1201, {}, why);
  CHECK(why == "roof");
  CHECK((names(pool, m) == std::vector<std::string>{"A1", "A6", "A2", "A7"}));  // A is now the far end for 1201
}

static void test_no_roof_falls_back_to_loss() {
  std::vector<PgwMember> m = {{2, "A1", 0.01}, {3, "A2", 0.30}, {4, "B1", 0.45}, {5, "B2", 0.20}, {6, "C1", -1}, {7, "C2", 0.25}};
  std::string why;
  auto pool = pgw_pool(Roof(), "1201", m, {}, why);
  CHECK(why == "loss");
  CHECK((pool == std::vector<uint16_t>{4, 3, 7, 5}));
  // A roof without this TAP placed is no roof for it.
  Roof r;
  r.panels.push_back({"0002", 0, 0, true});
  r.taps.push_back({"1202", 5, 5});
  pool = pgw_pool(r, "1201", m, {}, why);
  CHECK(why == "loss");
  // Too few placed to rank: loss again.
  r.taps.push_back({"1201", 9, 9});
  pool = pgw_pool(r, "1201", m, {}, why);
  CHECK(why == "loss");
}

static void test_named_overrides() {
  std::vector<PgwMember> m;
  Roof r = owners_roof(m);
  std::string why;
  auto pool = pgw_pool(r, "1201", m, {"b7", "B10", "0004", "Z9", "C1", "D1"}, why);
  CHECK(why == "named");
  // By name or address, case aside; one not reporting through this TAP is
  // skipped; at most four.
  CHECK((names(pool, m) == std::vector<std::string>{"B7", "B10", "B3", "C1"}));
}

static void test_nothing_to_pick() {
  std::string why;
  CHECK(pgw_pool(Roof(), "1201", {}, {}, why).empty());
  CHECK(why == "none");
  std::vector<PgwMember> unknown = {{2, "A1", -1}, {3, "A2", -1}};
  CHECK(pgw_pool(Roof(), "1201", unknown, {}, why).empty());
}

int main() {
  test_the_roof_picks_the_far_face();
  test_a_ridge_gap_doesnt_hand_it_to_a_dense_corner();
  test_only_this_taps_optimizers();
  test_no_roof_falls_back_to_loss();
  test_named_overrides();
  test_nothing_to_pick();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
