// Host test for the allocator hook in bus_watch.h and bus_book.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/bus_alloc_test.cpp -o /tmp/bus_alloc_test && /tmp/bus_alloc_test
//
// The ESP32 sets TIGO_BUS_ALLOCATOR to its PSRAM allocator: plain std types
// there come from the ~95 KB internal heap, and five minutes of poll marks
// alone ran the rig out (2026-11-19). This stands a counting allocator in
// for PSRAM, runs a roof's worth of answers through BusWatch and BusBook, and
// checks that what they keep went through it and that the ordinary heap
// holds nothing more afterwards. Compiling it at all also proves the two
// headers build with an allocator that isn't std::allocator, as the ESP32's.
//
// Addresses and barcodes here are made up; none belong to a real system.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

// Bytes live on the ordinary heap (operator new), and through the hook.
static long g_heap_live = 0;
static long g_hooked_live = 0;

void *operator new(size_t n) {
  size_t *p = static_cast<size_t *>(std::malloc(n + sizeof(size_t)));
  if (!p) throw std::bad_alloc();
  *p = n;
  g_heap_live += static_cast<long>(n);
  return p + 1;
}
void operator delete(void *q) noexcept {
  if (!q) return;
  size_t *p = static_cast<size_t *>(q) - 1;
  g_heap_live -= static_cast<long>(*p);
  std::free(p);
}
void operator delete(void *q, size_t) noexcept { operator delete(q); }

template <class T> struct CountingAllocator {
  using value_type = T;
  CountingAllocator() noexcept = default;
  template <class U> CountingAllocator(const CountingAllocator<U> &) noexcept {}
  T *allocate(size_t n) {
    g_hooked_live += static_cast<long>(n * sizeof(T));
    return static_cast<T *>(std::malloc(n * sizeof(T)));
  }
  void deallocate(T *p, size_t n) noexcept {
    g_hooked_live -= static_cast<long>(n * sizeof(T));
    std::free(p);
  }
  template <class U> bool operator==(const CountingAllocator<U> &) const noexcept { return true; }
  template <class U> bool operator!=(const CountingAllocator<U> &) const noexcept { return false; }
};

#define TIGO_BUS_ALLOCATOR CountingAllocator
#include "bus_book.h"

using namespace esphome::tigo_monitor::master;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static Frame poll(uint16_t tap) { return {tap, 0x0148, {0x00, 0x01, 0x6F, 0xC4, 0x04}}; }
static Frame answer(uint16_t node, const char *text, uint16_t tap) {
  Bytes p = {0x00, 0x1F, 0x00, 0x00, 0x00};
  size_t n = std::strlen(text);
  Bytes pkt = {0x07, static_cast<uint8_t>(node >> 8), static_cast<uint8_t>(node), 0x00, 0x00, 0x00,
               static_cast<uint8_t>(n)};
  pkt.insert(pkt.end(), text, text + n);
  p.insert(p.end(), pkt.begin(), pkt.end());
  return {static_cast<uint16_t>(tap | 0x8000), 0x0149, p};
}
// A tunnel request and its reply: an exchange BusWatch keeps.
static Frame tunnel(uint16_t tap, uint8_t seq) { return {tap, 0x0B0F, {0x00, 0x0E, 0x00, 0x2E, seq}}; }
static Frame tunnel_reply(uint16_t tap, uint8_t seq) {
  return {static_cast<uint16_t>(tap | 0x8000), 0x0B10,
          {0x00, 0x0E, 0x00, 0x2F, seq, 0x01, 0x03, 0x84, 0x00, 0x02, 0x00, 0x02, 0x00, 0x28}};
}

static void feed(BusWatch &w, BusBook &b, const Frame &f, int64_t &t) {
  w.on_frame(f, false, t += 25);
  b.absorb(w, t, false);
}

// A roof's worth: 40 optimizers on two TAPs, each answering the four
// queries, with tunnel exchanges between, and many polls.
static void run_roof(BusWatch &w, BusBook &b) {
  int64_t t = 1767225600000LL;
  char text[96];
  for (int round = 0; round < 2; round++)
    for (int i = 0; i < 40; i++) {
      uint16_t tap = i < 20 ? 0x1201 : 0x1202, node = static_cast<uint16_t>(0x10 + i);
      const char *answers[] = {"Mnode Version K8.0120 (2D)\r",
                               "!Smrt 0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 0190 0280 0009 00\r",
                               "!Mppt 1 0 S M[100 0018 - 3/3] 0 12 40 08 12\r", "!ESO075V700W-H461.11.70S0.12\r"};
      for (const char *a : answers) {
        std::snprintf(text, sizeof(text), "%s", a);
        feed(w, b, poll(tap), t);
        feed(w, b, answer(node, text, tap), t);
      }
      feed(w, b, tunnel(tap, static_cast<uint8_t>(i)), t);
      feed(w, b, tunnel_reply(tap, static_cast<uint8_t>(i)), t);
    }
}

// What BusWatch and BusBook keep goes through the hook; the ordinary heap
// holds nothing more once the roof's answers are in.
static void test_what_they_keep_goes_through_the_hook() {
  long heap_before = g_heap_live, hooked_before = g_hooked_live;
  {
    auto *w = new BusWatch(16, false);
    auto *b = new BusBook();
    b->set_barcode_of([](uint16_t a) {
      char s[17];
      std::snprintf(s, sizeof(s), "04C05B0000%06X", a);
      return std::string(s);
    });
    long heap_empty = g_heap_live;  // the two objects themselves (in PSRAM too on the ESP32: see tigo_monitor.cpp)
    run_roof(*w, *b);
    CHECK(b->versions.entries().size() == 40 && b->smarts.size() == 40 && b->mppts.size() == 40 &&
          b->cals.size() == 40 && b->owners.owners().size() == 40);
    long kept_hooked = g_hooked_live - hooked_before;
    long kept_heap = g_heap_live - heap_empty;
    std::printf("  40 optimizers: %ld bytes through the hook, %ld more on the ordinary heap\n", kept_hooked, kept_heap);
    CHECK(kept_hooked > 20000);
    // Only the last few requests, kept to pair with their replies: Frame's
    // payload is tigo_master.h's Bytes, a plain vector, and at most five
    // frames are held (open_req_ and recent_unanswered_).
    CHECK(kept_heap < 512);
    delete b;
    delete w;
  }
  CHECK(g_heap_live == heap_before);
  CHECK(g_hooked_live == hooked_before);
}

// The saved copy and the JSON can be written into a hooked string, as the
// ESP32 writes them into PSRAM.
static void test_writing_into_a_hooked_string() {
  BusWatch w(16, false);
  BusBook b;
  run_roof(w, b);
  Str text, json;
  b.write_text(text);
  b.write_json(json, false);
  CHECK(text.size() > 1000 && json.size() > 1000);
  BusBook c;
  size_t from = 0;
  int bad = 0;
  while (from < text.size()) {
    size_t nl = text.find('\n', from);
    bad += !c.restore_line(text.substr(from, nl - from));
    from = nl + 1;
  }
  Str again;
  c.write_json(again, false);
  CHECK(bad == 0 && again == json);
}

// With poll stats on, as the Bus log will want them, the marks go through
// the hook too.
static void test_poll_marks_go_through_the_hook() {
  BusWatch w;
  long before = g_heap_live;
  int64_t t = 0;
  for (int i = 0; i < 2000; i++, t += 40) {
    w.on_frame(poll(0x1201), true, t);
    w.on_frame(answer(0x10, "x", 0x1201), false, t + 10);
  }
  CHECK(w.stats(t).polls == 2000);
  CHECK(g_heap_live - before <= 0);
}

int main() {
  test_what_they_keep_goes_through_the_hook();
  test_writing_into_a_hooked_string();
  test_poll_marks_go_through_the_hook();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
