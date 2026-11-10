// Host test for components/tigo_monitor/barcode_bindings.h: moving a
// barcode-keyed (long_address:) sensor when the gateway renumbers panels.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/barcode_bindings_test.cpp -o /tmp/barcode_bindings_test && /tmp/barcode_bindings_test

#include "barcode_bindings.h"

#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

using Map = std::map<std::string, int *>;

// Two barcode-keyed panels trade addresses in one re-commissioning: A from
// 0001 to 0002, B from 0002 to 0001. Each must end up at its new address;
// the second move must not erase the slot the first one just took.
static void test_a_swap_keeps_both_panels() {
  int a = 1, b = 2;
  Map power = {{"0001", &a}, {"0002", &b}};
  esphome::tigo_monitor::move_barcode_sensor(power, &a, std::string("0001"), std::string("0002"));
  esphome::tigo_monitor::move_barcode_sensor(power, &b, std::string("0002"), std::string("0001"));
  CHECK(power.size() == 2);
  CHECK(power["0002"] == &a);
  CHECK(power["0001"] == &b);
}

// A plain move leaves the old address empty.
static void test_a_move_frees_the_old_address() {
  int a = 1;
  Map power = {{"0001", &a}};
  CHECK(esphome::tigo_monitor::move_barcode_sensor(power, &a, std::string("0001"), std::string("0005")) == nullptr);
  CHECK(power.count("0001") == 0 && power["0005"] == &a);
}

// First placement, with no old address.
static void test_a_first_placement() {
  int a = 1;
  Map power;
  CHECK(esphome::tigo_monitor::move_barcode_sensor(power, &a, std::string(), std::string("0003")) == nullptr);
  CHECK(power["0003"] == &a);
}

// Moving onto an address another sensor holds (say an address:-keyed one)
// says which it displaced, so the caller can warn.
static void test_a_move_onto_another_sensor_reports_it() {
  int a = 1, other = 9;
  Map power = {{"0001", &a}, {"0004", &other}};
  CHECK(esphome::tigo_monitor::move_barcode_sensor(power, &a, std::string("0001"), std::string("0004")) == &other);
  CHECK(power["0004"] == &a);
}

int main() {
  test_a_swap_keeps_both_panels();
  test_a_move_frees_the_old_address();
  test_a_first_placement();
  test_a_move_onto_another_sensor_reports_it();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
