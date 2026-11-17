// Host test for linux/src/heard_taps.h: the TAPs a monitor heard another
// master run, for when it takes over the bus without a saved TAP list.
//
//   g++ -std=c++17 -Wall -Wextra -I linux/src tests/host/heard_taps_test.cpp -o /tmp/heard_taps_test
//
// MACs here are made up; none belong to a real TAP.

#include "heard_taps.h"

#include <cstdio>

using namespace tigod;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static const int64_t NOW = 1800000000;
static const std::string MAC_H1 = "04C05B3000000001", MAC_G8 = "04C05B3000000002";

static std::vector<uint16_t> ids(const std::vector<HeardTap> &t) {
  std::vector<uint16_t> out;
  for (const auto &x : t) out.push_back(x.id);
  return out;
}

// After a CCA ran two TAPs (2026-11-16 on the roof): both, in id order,
// each with the MAC it gave; the self-test's temporary ids, which carry
// MACs but no optimizers, are not TAPs to run.
static void test_two_taps_after_a_cca() {
  std::printf("two TAPs a CCA ran, not its temporary ids\n");
  std::vector<std::pair<uint16_t, int64_t>> heard = {{0x1202, NOW - 60}, {0x1201, NOW - 30}, {0x1201, NOW - 90}, {0x1202, NOW - 5}};
  std::map<uint16_t, std::string> macs = {{0x1201, MAC_H1}, {0x1202, MAC_G8}, {0x1203, MAC_G8}, {0x1204, MAC_H1}};
  auto t = heard_taps(heard, macs, NOW);
  CHECK((ids(t) == std::vector<uint16_t>{0x1201, 0x1202}));
  CHECK(t.size() == 2 && t[0].mac == MAC_H1 && t[1].mac == MAC_G8);
}

// One the other master moved to a new id today (a CCA re-addressed the
// roof's only TAP to 1202 on 11-14): the id it was heard through last.
static void test_a_tap_readdressed() {
  std::printf("a TAP re-addressed today: only its newest id\n");
  std::vector<std::pair<uint16_t, int64_t>> heard = {{0x1201, NOW - 7200}, {0x1202, NOW - 10}};
  std::map<uint16_t, std::string> macs = {{0x1201, MAC_G8}, {0x1202, MAC_G8}};
  CHECK((ids(heard_taps(heard, macs, NOW)) == std::vector<uint16_t>{0x1202}));
}

// Not heard in a day: not run. No MAC heard: run by id alone.
static void test_old_and_unnamed() {
  std::printf("a TAP silent for a day is left out; one with no MAC kept\n");
  std::vector<std::pair<uint16_t, int64_t>> heard = {{0x1205, NOW - 2 * 86400}, {0x1201, NOW - 100}, {0, NOW}};
  auto t = heard_taps(heard, {}, NOW);
  CHECK((ids(t) == std::vector<uint16_t>{0x1201}));
  CHECK(t.size() == 1 && t[0].mac.empty());
  CHECK(heard_taps({}, {{0x1201, MAC_H1}}, NOW).empty());
}

int main() {
  test_two_taps_after_a_cca();
  test_a_tap_readdressed();
  test_old_and_unnamed();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
