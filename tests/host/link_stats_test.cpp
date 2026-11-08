// Host tests for link_stats.h: each optimizer's lost reports, counted from
// slot-counter gaps, and its RSSI over the last day.
#include "link_stats.h"

#include <cstdio>
#include <string>

using namespace esphome::tigo_monitor::link;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static const int DAY = 20261107;
static const int64_t T0 = 1794049200;

// The slot counter a report carries for a tick count: Hslot in the top 2
// bits, 5 ms ticks in the low 14.
static uint16_t slot_of(uint32_t ticks) { return static_cast<uint16_t>(((ticks / 12000) << 14) | (ticks % 12000)); }

// Reports at these tick counts, 2 s apart.
static void reports(LinkStats &s, const char *addr, std::initializer_list<uint32_t> ticks, int64_t t = T0) {
  for (uint32_t k : ticks) {
    s.on_report(addr, 0x1201, 120, slot_of(k), t, DAY);
    t += 2;
  }
}

static void test_slot_ticks() {
  CHECK(slot_ticks(slot_of(0)) == 0);
  CHECK(slot_ticks(slot_of(12400)) == 12400);
  CHECK(slot_ticks(slot_of(47999)) == 47999);
}

static void test_steady_reports_lose_nothing() {
  LinkStats s;
  reports(s, "0001", {0, 400, 800, 1200, 1600, 2000, 2400, 2800, 3200, 3600});
  CHECK(s.links().at("0001").reports_today == 10);
  CHECK(s.links().at("0001").lost_today == 0);
}

static void test_a_missed_bin_is_one_lost() {
  LinkStats s;
  reports(s, "0001", {0, 400, 1200});
  CHECK(s.links().at("0001").lost_today == 1);
}

static void test_cadence_is_the_smallest_gap() {
  LinkStats s;
  reports(s, "0001", {0, 800, 1600, 2400});
  s.on_report("0001", 0x1201, 120, slot_of(4000), T0 + 10, DAY);
  CHECK(s.links().at("0001").lost_today == 1);
}

static void test_a_duplicate_is_ignored() {
  LinkStats s;
  reports(s, "0001", {0, 400, 400});
  CHECK(s.links().at("0001").reports_today == 2);
  CHECK(s.links().at("0001").lost_today == 0);
}

static void test_out_of_order_is_not_loss() {
  LinkStats s;
  s.on_report("0001", 0x1201, 120, slot_of(800), T0, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(400), T0, DAY);
  CHECK(s.links().at("0001").lost_today == 0);
}

static void test_wrap_is_continuous() {
  LinkStats s;
  reports(s, "0001", {47200, 47600, 0});
  CHECK(s.links().at("0001").lost_today == 0);
  reports(s, "0002", {47200, 47600, 400});
  CHECK(s.links().at("0002").lost_today == 1);
}

static void test_a_long_silence_is_not_loss() {
  LinkStats s;
  s.on_report("0001", 0x1201, 120, slot_of(0), T0, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(400), T0 + 2, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(4000), T0 + 43200, DAY);
  CHECK(s.links().at("0001").lost_today == 0);
  CHECK(s.links().at("0001").reports_today == 3);
}

static void test_midnight_resets_today() {
  LinkStats s;
  reports(s, "0001", {0, 400, 1200});
  CHECK(s.links().at("0001").lost_today == 1);
  s.on_report("0001", 0x1201, 120, slot_of(1600), T0 + 6, DAY + 1);
  CHECK(s.links().at("0001").reports_today == 1);
  CHECK(s.links().at("0001").lost_today == 0);
}

static void test_rssi_ring_averages_five_minutes() {
  LinkStats s;
  int64_t t = (T0 / 300) * 300;  // the start of a bucket
  s.on_report("0001", 0x1201, 100, slot_of(0), t, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(400), t + 2, DAY);
  auto r = s.rssi_24h("0001", t + 10);
  CHECK(r.size() == BUCKETS);
  CHECK(r.back() == 110);
  auto later = s.rssi_24h("0001", t + 2 * BUCKET_S);
  CHECK(later[BUCKETS - 3] == 110);
  CHECK(later[BUCKETS - 2] == -1);
  CHECK(later[BUCKETS - 1] == -1);
  CHECK(s.links().at("0001").rssi == 120);
}

static void test_empty_ring_is_all_missing() {
  LinkStats s;
  auto r = s.rssi_24h("0009", T0);
  CHECK(r.size() == BUCKETS);
  bool all = true;
  for (int v : r) all = all && v == -1;
  CHECK(all);
}

static void test_ring_forgets_after_a_day() {
  LinkStats s;
  s.on_report("0001", 0x1201, 100, slot_of(0), T0, DAY);
  auto r = s.rssi_24h("0001", T0 + 86400);
  bool all = true;
  for (int v : r) all = all && v == -1;
  CHECK(all);
}

static void test_join_is_recorded() {
  LinkStats s;
  s.on_join("0012", 1794049920);
  CHECK(s.links().at("0012").last_join_s == 1794049920);
  CHECK(s.links().at("0012").reports_today == 0);
}

static void test_tap_and_last_heard() {
  LinkStats s;
  s.on_report("0001", 0x1202, 90, slot_of(0), T0, DAY);
  CHECK(s.links().at("0001").tap == 0x1202);
  CHECK(s.links().at("0001").last_heard_s == T0);
}

// Review fixes, 2026-11-07.
static void test_a_late_report_fills_its_hole() {
  LinkStats s;
  reports(s, "0001", {0, 400});
  s.on_report("0001", 0x1201, 120, slot_of(1200), T0 + 4, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(800), T0 + 4, DAY);
  CHECK(s.links().at("0001").lost_today == 0);
  CHECK(s.links().at("0001").reports_today == 4);
}

static void test_a_short_silence_keeps_counting() {
  LinkStats s;
  reports(s, "0001", {0, 400});
  // 150 s quiet: a modular gap past half the wrap, but no wrap.
  s.on_report("0001", 0x1201, 120, slot_of(30400), T0 + 152, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(30800), T0 + 154, DAY);
  CHECK(s.links().at("0001").reports_today == 4);
  CHECK(s.links().at("0001").lost_today == 0);
  CHECK(s.links().at("0001").last_heard_s == T0 + 154);
}

static void test_a_duplicate_is_still_heard() {
  LinkStats s;
  s.on_report("0001", 0x1201, 120, slot_of(400), T0, DAY);
  s.on_report("0001", 0x1201, 120, slot_of(400), T0 + 1, DAY);
  CHECK(s.links().at("0001").last_heard_s == T0 + 1);
  CHECK(s.links().at("0001").reports_today == 1);
}

static void test_a_silent_optimizer_reads_zero_after_midnight() {
  LinkStats s;
  reports(s, "0001", {0, 400, 1200});
  s.roll(DAY + 1);
  CHECK(s.links().at("0001").reports_today == 0);
  CHECK(s.links().at("0001").lost_today == 0);
  s.roll(DAY + 1);  // again on the same day: nothing changes
  s.on_report("0001", 0x1201, 120, slot_of(1600), T0 + 8, DAY + 1);
  CHECK(s.links().at("0001").reports_today == 1);
}

static void test_a_deleted_optimizer_is_forgotten() {
  LinkStats s;
  reports(s, "0001", {0, 400});
  reports(s, "0002", {0, 400});
  s.erase("0001");
  CHECK(s.links().count("0001") == 0);
  CHECK(s.links().count("0002") == 1);
  s.erase("0009");  // unknown: nothing happens
  CHECK(s.links().size() == 1);
}

// The roof at sunrise, 2026-11-08: a 600-tick cadence, and reports that
// arrive up to 3,000 ticks behind the newest. Each late one fills a hole the
// jump over it counted, and the next report is measured from the newest.
static void test_a_report_far_behind_fills_its_hole() {
  LinkStats s;
  reports(s, "0001", {0, 600, 1200, 1800, 2400, 6000, 3000, 3600, 4200, 4800, 5400, 6600});
  CHECK(s.links().at("0001").lost_today == 0);
  CHECK(s.links().at("0001").reports_today == 12);
}

static void test_a_late_report_keeps_the_newest() {
  LinkStats s;
  reports(s, "0001", {0, 600, 1200, 1800, 2400, 4800, 3000, 3600, 4200, 5400});
  CHECK(s.links().at("0001").lost_today == 0);
}

// A late report heard twice fills its hole once, and counts once.
static void test_a_late_report_heard_twice_counts_once() {
  LinkStats s;
  // 3600 to 5400 never come: 4 lost, however often 3000 is heard.
  reports(s, "0001", {0, 600, 1200, 1800, 2400, 6000, 3000, 3000, 3000, 6600});
  CHECK(s.links().at("0001").lost_today == 4);
  CHECK(s.links().at("0001").reports_today == 8);
}

int main() {
  test_slot_ticks();
  test_steady_reports_lose_nothing();
  test_a_missed_bin_is_one_lost();
  test_cadence_is_the_smallest_gap();
  test_a_duplicate_is_ignored();
  test_out_of_order_is_not_loss();
  test_wrap_is_continuous();
  test_a_long_silence_is_not_loss();
  test_midnight_resets_today();
  test_rssi_ring_averages_five_minutes();
  test_empty_ring_is_all_missing();
  test_ring_forgets_after_a_day();
  test_join_is_recorded();
  test_tap_and_last_heard();
  test_a_late_report_fills_its_hole();
  test_a_short_silence_keeps_counting();
  test_a_duplicate_is_still_heard();
  test_a_silent_optimizer_reads_zero_after_midnight();
  test_a_deleted_optimizer_is_forgotten();
  test_a_report_far_behind_fills_its_hole();
  test_a_late_report_keeps_the_newest();
  test_a_late_report_heard_twice_counts_once();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
