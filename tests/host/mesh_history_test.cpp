// Host test for mesh_history.h: a day's reports and losses kept per
// optimizer, a past join's relay found from the join log, and when an
// optimizer's settings answer has really changed.
#include <cstdio>
#include <string>
#include <vector>

#include "mesh_history.h"

using namespace esphome::tigo_monitor::mesh_history;

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
      failures++;                                                 \
    }                                                             \
  } while (0)

// The day's counts are overwritten as they grow; an earlier day keeps its
// last ones; only the newest days are kept.
static void test_loss_book_keeps_each_days_last_counts() {
  LossBook b;
  b.note("2026-11-08", "0002", 100, 2);
  b.note("2026-11-08", "0002", 900, 9);
  b.note("2026-11-09", "0002", 50, 1);
  CHECK(b.days().size() == 2);
  CHECK(b.days().at("2026-11-08").at("0002").first == 900);
  CHECK(b.days().at("2026-11-08").at("0002").second == 9);
  CHECK(b.days().at("2026-11-09").at("0002").first == 50);
  for (int d = 10; d < 50; d++) b.note("2026-11-" + std::to_string(d), "0002", 1, 0);
  b.prune(30);
  CHECK(b.days().size() == 30);
  CHECK(b.days().count("2026-11-08") == 0);
  CHECK(b.days().begin()->first == "2026-11-20");
}

// A count that goes down (the day restarted) isn't kept over a higher one.
static void test_loss_book_keeps_the_higher_count() {
  LossBook b;
  b.note("2026-11-09", "0002", 900, 9);
  b.note("2026-11-09", "0002", 10, 0);
  CHECK(b.days().at("2026-11-09").at("0002").first == 900);
}

// A past join's relay: the optimizer that joined with the parent's NWID
// nearest in time, within a day, before or after. NWIDs are reused, so the
// nearest wins.
static void test_a_past_joins_relay_is_found_in_the_log() {
  std::vector<JoinEntry> log = {
      {"0006", 1000, "0001", "0018"},        // the relay joins
      {"0003", 1010, "0018", "0019"},        // through it
      {"0026", 2000, "005A", "005B"},        // its relay joins after it
      {"0017", 2030, "0001", "005A"},
      {"0040", 1000 + 3 * 86400, "0001", "0018"},  // 0018 again, days on
      {"0041", 1010 + 3 * 86400, "0018", "0042"},
      {"0002", 5000, "0044", "0044"},        // its own NWID: no relay
      {"0005", 6000, "0099", "0070"},        // nobody holds 0099
  };
  auto r = resolve_relays(log);
  CHECK(r.size() == log.size());
  CHECK(r[0] == "");        // through the TAP
  CHECK(r[1] == "0006");
  CHECK(r[2] == "0017");
  CHECK(r[4] == "");
  CHECK(r[5] == "0040");    // not 0006, three days back
  CHECK(r[6] == "");
  CHECK(r[7] == "");
}

// A settings answer has changed only if a whole field other than the
// stored flag has; an H9's placeholder never changes.
static void test_a_settings_change_is_a_field_not_the_flag() {
  const std::string k8 = "0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01";
  CHECK(!settings_changed(k8, k8));
  CHECK(!settings_changed(k8, "0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 N 00 01"));
  CHECK(settings_changed(k8, "0FFF 0000 0E00 0008 00C8 00C8 000A 0154 0154 S 00 01"));
  CHECK(settings_changed(k8, "0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 01 01"));
  // The K8's cut-off tail isn't a field.
  CHECK(!settings_changed(k8, "0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 02"));
  const std::string h9n = "0000 0000 0000 0000 0000 0000 0000 0000 0000 N 00 0000 0000 0000 00";
  const std::string h9s = "0000 0000 0000 0000 0000 0000 0000 0000 0000 S 00 0000 0000 0000 00";
  CHECK(!settings_changed(h9n, h9s));
  CHECK(placeholder(h9n) && !placeholder(k8));
  CHECK(!settings_changed("", k8));  // the first answer is no change
}

int main() {
  test_loss_book_keeps_each_days_last_counts();
  test_loss_book_keeps_the_higher_count();
  test_a_past_joins_relay_is_found_in_the_log();
  test_a_settings_change_is_a_field_not_the_flag();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
