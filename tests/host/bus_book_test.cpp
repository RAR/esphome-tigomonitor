// Host tests for bus_book.h: what the bus says about each optimizer, kept by
// address, as /api/versions gives it on both builds, and the ESP32's saved
// copy read back the same.
#include "bus_book.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace esphome::tigo_monitor::master;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static Frame poll(uint16_t tap = 0x1201) { return {tap, 0x0148, {0x00, 0x01, 0x6F, 0xC4, 0x04}}; }
// A 0x0149 with one type-07 packet: an optimizer's text answer.
static Frame answer(uint16_t node, const char *text, uint16_t tap = 0x1201) {
  Bytes p = {0x00, 0x1F, 0x00, 0x00, 0x00};
  size_t n = std::strlen(text);
  Bytes pkt = {0x07, static_cast<uint8_t>(node >> 8), static_cast<uint8_t>(node), 0x00, 0x00, 0x00,
               static_cast<uint8_t>(n)};
  pkt.insert(pkt.end(), text, text + n);
  p.insert(p.end(), pkt.begin(), pkt.end());
  return {static_cast<uint16_t>(tap | 0x8000), 0x0149, p};
}

static void hear(BusWatch &w, BusBook &b, uint16_t node, const char *text, int64_t at, bool runs_bus = false) {
  w.on_frame(poll(), false, at);
  w.on_frame(answer(node, text), false, at + 10);
  b.absorb(w, at + 10, runs_bus);
}

static const char *SMRT = "!Smrt 0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 0190 0280 0009 00\r";
static const char *SMRT_CHANGED = "!Smrt 0FFF 0000 0FFF 0008 00C8 00C8 000B 0154 0154 S 00 0190 0280 0009 00\r";

static BusBook filled(bool runs_bus) {
  BusWatch w;
  BusBook b;
  b.set_barcode_of([](uint16_t a) { return a == 0x10 ? std::string("04C05B0000000010") : std::string(); });
  hear(w, b, 0x10, "Mnode Version K8.0120 (2D)\r", 1000, runs_bus);
  hear(w, b, 0x10, SMRT, 2000, runs_bus);
  hear(w, b, 0x10, "!Mppt 1 0 S M[100 0018 - 3/3] 0 12 40 08 12\r", 3000, runs_bus);
  hear(w, b, 0x10, "!ESO075V700W-H461.11.70S0.12\r", 4000, runs_bus);
  hear(w, b, 0x10, SMRT_CHANGED, 5000, runs_bus);
  return b;
}

static std::string json_of(const BusBook &b, bool with_pans) {
  std::string s;
  b.write_json(s, with_pans);
  return s;
}

// Each answer lands by address, and one heard changed is a change only while
// this monitor runs the bus.
static void test_answers_fill_the_book() {
  BusBook b = filled(true);
  CHECK(b.versions.find(false, 0x10) && b.versions.find(false, 0x10)->version == "K8.0120 (2D)");
  CHECK(b.smarts.count(0x10) && b.smarts[0x10].first.stored && b.smarts[0x10].second == 5010);
  CHECK(b.mppts.count(0x10) && b.mppts[0x10].first.params == "12 40 08 12");
  CHECK(b.cals.count(0x10) && b.cals[0x10].first.volts == 75 && b.cals[0x10].first.watts == 700);
  CHECK(b.changes.size() == 1 && b.changes[0].what == "settings" && b.changes[0].addr == 0x10);
  CHECK(b.owners.owners().count(0x10) && b.dirty);
  CHECK(filled(false).changes.empty());
}

// The JSON parses, carries what the Devices page reads, and never the PAN
// or the owners unless asked for a saved copy.
static void test_the_json_is_the_apis() {
  BusBook b = filled(true);
  TapRadio r;
  r.tap = 0x1201;
  r.channel = 15;
  r.slt = 30;
  r.slm = 2;
  r.pan = 0x1234;
  b.radios[0x1201] = {r, 6000};
  b.tap_ids[0x1201] = {"04C05B00000000AA", 6100};
  json j = json::parse(json_of(b, false), nullptr, false);
  CHECK(j.is_object());
  if (!j.is_object()) return;
  for (const char *k : {"taps", "optimizers", "radios", "ids", "polled", "settings", "mppt", "ratings", "changes"})
    CHECK(j.contains(k) && j[k].is_array());
  CHECK(!j.contains("owners"));
  CHECK(j["optimizers"].size() == 1 && j["optimizers"][0]["addr"] == "0010" &&
        j["optimizers"][0]["version"] == "K8.0120 (2D)" && j["optimizers"][0]["since_unix_ms"] == 1010);
  CHECK(j["radios"][0]["tap"] == "1201" && j["radios"][0]["channel"] == 15 && !j["radios"][0].contains("pan") &&
        !j["radios"][0].contains("profile"));
  CHECK(j["ids"][0]["mac"] == "04C05B00000000AA");
  CHECK(j["settings"][0]["stored"] == true && j["settings"][0]["heard_unix_ms"] == 5010);
  CHECK(j["mppt"][0]["answer"] == "1 0 S M[100 0018 - 3/3] 0 12 40 08 12");
  CHECK(j["ratings"][0]["cal"] == "ESO075V700W-H461.11.70S0.12");
  CHECK(j["changes"][0]["what"] == "settings" && j["changes"][0]["addr"] == "0010");
  json saved = json::parse(json_of(b, true), nullptr, false);
  CHECK(saved.is_object() && saved["radios"][0]["pan"] == "1234" && saved["owners"][0]["barcode"] == "04C05B0000000010");
}

// Text heard on the bus goes into the JSON escaped.
static void test_odd_text_is_escaped() {
  BusBook b;
  CalSeen c;
  c.addr = 0x22;
  c.cal = "a\"b\\c\x01";
  b.cals[0x22] = {c, 1};
  json j = json::parse(json_of(b, false), nullptr, false);
  CHECK(j.is_object() && j["ratings"][0]["cal"] == "a\"b\\c\x01");
}

// The saved copy reads back to the same book, line by line.
static void test_the_saved_copy_reads_back() {
  BusBook a = filled(true);
  TapRadio r;
  r.tap = 0x1202;
  r.channel = 20;
  r.slt = 31;
  r.slm = 3;
  r.profile = 75;
  r.pan = 0x0ABC;
  a.radios[0x1202] = {r, 7000};
  a.tap_ids[0x1202] = {"04C05B00000000BB", 7100};
  a.versions.record({true, 0x1202, "Mgate", "G8.59"}, 7200);
  a.hear_poll_reply(0x9202, 7300);
  std::string text;
  a.write_text(text);
  BusBook b;
  size_t from = 0, lines = 0, bad = 0;
  while (from < text.size()) {
    size_t nl = text.find('\n', from);
    lines++;
    bad += !b.restore_line(text.substr(from, nl - from));
    from = nl + 1;
  }
  CHECK(lines == 10 && bad == 0);
  CHECK(json_of(a, true) == json_of(b, true));
}

// A TAP answering polls is listed by the time it last answered, before any
// master has read its firmware: a monitor that only listens sees the read
// only when a master brings a TAP up. Requests, and the addresses no TAP
// keeps (none, the factory's, the first-run 0x1234), list nothing.
static void test_a_tap_answering_polls_is_listed() {
  BusBook b;
  b.hear_poll_reply(0x9201, 1000);
  CHECK(b.dirty);
  b.dirty = false;
  b.hear_poll_reply(0x9201, 2000);
  CHECK(!b.dirty);  // only a new TAP is worth saving for
  for (uint16_t dest : {0x1202, 0x8000, 0x9234, 0x9235, 0xFFFF}) b.hear_poll_reply(dest, 3000);
  CHECK(!b.dirty);
  json j = json::parse(json_of(b, false), nullptr, false);
  CHECK(j.is_object() && j["polled"].is_array() && j["polled"].size() == 1);
  if (!j.is_object() || j["polled"].size() != 1) return;
  CHECK(j["polled"][0]["tap"] == "1201" && j["polled"][0]["heard_unix_ms"] == 2000);
  CHECK(j["taps"].empty());
}

// A tab or newline inside a field can't split a record: it becomes a space.
static void test_a_tab_in_a_field_stays_in_its_field() {
  BusBook a;
  MpptSeen m;
  m.addr = 0x30;
  m.params = "12 40 08 12";
  m.answer = "one\ttwo\nthree";
  a.mppts[0x30] = {m, 9};
  std::string text;
  a.write_text(text);
  BusBook b;
  CHECK(b.restore_line(text));
  CHECK(b.mppts.count(0x30) && b.mppts[0x30].first.answer == "one two three");
}

// A line that isn't a record is skipped, and says so.
static void test_a_bad_line_is_skipped() {
  BusBook b;
  CHECK(!b.restore_line(""));
  CHECK(!b.restore_line("Z\t1\t2"));
  CHECK(!b.restore_line("S\t0010"));            // too few fields
  CHECK(!b.restore_line("S\t0010\t\t1\t5"));    // no settings
  CHECK(!b.restore_line("I\t1201\tSHORT\t5"));  // not a MAC
  CHECK(b.smarts.empty() && b.tap_ids.empty());
}

int main() {
  test_answers_fill_the_book();
  test_the_json_is_the_apis();
  test_odd_text_is_escaped();
  test_the_saved_copy_reads_back();
  test_a_tap_answering_polls_is_listed();
  test_a_tab_in_a_field_stays_in_its_field();
  test_a_bad_line_is_skipped();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
