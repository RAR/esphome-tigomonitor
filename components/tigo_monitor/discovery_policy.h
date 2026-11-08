#pragma once

// When to keep the TAP's discovery (join) window open, the way the CCA
// decides it: on the bench (2026-11-06) the CCA opened its 900 s windows only
// while an optimizer it expected was missing, and with every one found sat
// with discovery off. Known optimizers rejoin with the window closed (about
// 40 s after a TAP reset, about 2 min after hours unpowered, and all 40 on the
// roof at sunrise), so the window matters only for one that hasn't come back.
//
// "Expected" here is every optimizer placed in the owner's layout; "missing"
// is not heard since local midnight. Pure and header-only, like
// tigo_protocol.h, so it builds on the host (tests/host/discovery_policy_test.cpp).

#include <cstdint>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace discovery {

// Some optimizer reported this recently: it's daytime on the roof.
static const int64_t DAYLIGHT_WITHIN_S = 600;

inline bool daylight(int64_t last_report_unix, int64_t now_unix) {
  return last_report_unix > 0 && now_unix - last_report_unix <= DAYLIGHT_WITHIN_S;
}

// expected_last_heard: when each optimizer in the layout last reported (0 =
// never). With none placed there is nothing to judge by, so the window is
// renewed as it always was. At night everything is unheard since midnight
// until sunrise, when they rejoin unaided: nothing is held open for that.
inline bool window_wanted(const std::vector<int64_t> &expected_last_heard, int64_t midnight_unix, bool is_daylight) {
  if (expected_last_heard.empty()) return true;
  if (!is_daylight) return false;
  for (int64_t t : expected_last_heard)
    if (t < midnight_unix) return true;
  return false;
}

}  // namespace discovery
}  // namespace tigo_monitor
}  // namespace esphome
