// Host tests for discovery_policy.h: when the CCA-style automatic discovery
// wants the TAP's join window open.
#include "discovery_policy.h"

#include <cstdio>
#include <vector>

using namespace esphome::tigo_monitor::discovery;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static const int64_t MIDNIGHT = 1794110400;  // a local midnight
static const int64_t NOON = MIDNIGHT + 12 * 3600;

// Nothing placed in the layout: nothing to judge by, so keep the window as
// before (renewed), never shut it for good.
static void test_no_layout_keeps_renewing() {
  CHECK(window_wanted({}, MIDNIGHT, true));
  CHECK(window_wanted({}, MIDNIGHT, false));
}

static void test_all_heard_today_closes_it() {
  CHECK(!window_wanted({NOON - 5, NOON - 60, MIDNIGHT + 1}, MIDNIGHT, true));
}

static void test_one_unheard_today_opens_it() {
  CHECK(window_wanted({NOON - 5, MIDNIGHT - 30, NOON - 60}, MIDNIGHT, true));
}

static void test_never_heard_opens_it() {
  CHECK(window_wanted({NOON - 5, 0}, MIDNIGHT, true));
}

// At night every optimizer is unheard today until sunrise, and they rejoin
// at sunrise with the window closed: night holds nothing open.
static void test_night_holds_nothing_open() {
  CHECK(!window_wanted({MIDNIGHT - 3600, MIDNIGHT - 7200}, MIDNIGHT, false));
}

static void test_daylight_from_recent_reports() {
  CHECK(daylight(NOON - 30, NOON));
  CHECK(daylight(NOON - DAYLIGHT_WITHIN_S + 1, NOON));
  CHECK(!daylight(NOON - DAYLIGHT_WITHIN_S - 1, NOON));
  CHECK(!daylight(0, NOON));
}

int main() {
  test_no_layout_keeps_renewing();
  test_all_heard_today_closes_it();
  test_one_unheard_today_opens_it();
  test_never_heard_opens_it();
  test_night_holds_nothing_open();
  test_daylight_from_recent_reports();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
