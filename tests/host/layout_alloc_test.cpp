// Host test for the allocator hook in components/tigo_monitor/tigo_layout.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/layout_alloc_test.cpp -o /tmp/layout_alloc_test && /tmp/layout_alloc_test
//
// The ESP32 sets TIGO_LAYOUT_ALLOCATOR to its PSRAM allocator: a plain
// std::vector there comes from the ~64 KB internal heap, and a Cloud import
// against a user-made layout aborted growing diff()'s list of changes. This
// stands a counting allocator in for PSRAM and checks the lists a layout
// builds go through it.
//
// Barcodes here are made up; none belong to a real installation.

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

static size_t g_hooked = 0;  // allocations through the hook

template <class T> struct CountingAllocator {
  using value_type = T;
  CountingAllocator() noexcept = default;
  template <class U> CountingAllocator(const CountingAllocator<U> &) noexcept {}
  T *allocate(size_t n) {
    g_hooked++;
    return std::allocator<T>().allocate(n);
  }
  void deallocate(T *p, size_t n) noexcept { std::allocator<T>().deallocate(p, n); }
  template <class U> bool operator==(const CountingAllocator<U> &) const noexcept { return true; }
  template <class U> bool operator!=(const CountingAllocator<U> &) const noexcept { return false; }
};

#define TIGO_LAYOUT_ALLOCATOR CountingAllocator
#include "tigo_layout.h"

using namespace esphome::tigo_monitor::layout;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static std::string bc(int i) {
  char b[17];
  std::snprintf(b, sizeof(b), "04C05B0000%06X", i);
  return b;
}

// 40 panels, as on the roof, named for string `s`.
static Assignments forty(const char *s) {
  Assignments out;
  for (int i = 0; i < 40; i++) out.push_back({bc(i), std::string(s) + std::to_string(i + 1), s, "MPPT 1"});
  return out;
}

static void test_diff_uses_the_hook() {
  std::printf("diff's list of changes comes from the hooked allocator\n");
  Assignments now = forty("A"), cloud = forty("B");
  g_hooked = 0;
  Changes c = diff(now, cloud);
  CHECK(c.size() == 40);
  CHECK(g_hooked > 0);
}

static void test_assignments_and_tree_use_the_hook() {
  std::printf("assignments() and build_tree() allocate through the hook\n");
  Layout l;
  InverterSpec inv;
  inv.name = "Inverter 1";
  MpptSpec m;
  m.label = "MPPT 1";
  StringSpec s;
  s.label = "A";
  for (int i = 0; i < 40; i++) s.panels.push_back(bc(i));
  m.strings.push_back(s);
  inv.mppts.push_back(m);
  l.inverters.push_back(inv);
  g_hooked = 0;
  Assignments a = assignments(l);
  CHECK(a.size() == 40);
  CHECK(g_hooked > 0);

  NodeViews nodes;
  for (int i = 0; i < 40; i++) nodes.push_back({bc(i), "00" + std::to_string(i), "A" + std::to_string(i + 1), "A", "MPPT 1"});
  g_hooked = 0;
  Tree t = build_tree(l, nodes, [](StringSpec &) {});
  CHECK(t.panels["A"].size() == 40);
  CHECK(g_hooked > 0);
}

static void test_differs_matches_diff() {
  std::printf("differs() answers what !diff().empty() does\n");
  Assignments now = forty("A");
  CHECK(!differs(now, now));
  CHECK(!differs(now, {}));
  Assignments renamed = now;
  renamed[7].label = "Roof 8";
  CHECK(differs(now, renamed));
  Assignments moved = now;
  moved[39].string_label = "B";
  CHECK(differs(now, moved));
  Assignments fresh = {{bc(99), "A41", "A", "MPPT 1"}};
  CHECK(differs(now, fresh));
  CHECK(differs({}, now));
  for (const auto *in : {&now, &renamed, &moved, &fresh})
    CHECK(differs(now, *in) == !diff(now, *in).empty());
}

int main() {
  test_diff_uses_the_hook();
  test_assignments_and_tree_use_the_hook();
  test_differs_matches_diff();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
