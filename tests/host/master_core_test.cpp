// MasterCore against a fake port: what it tells on_tx about when a request
// went out.
//
//   g++ -std=c++17 -O2 -I components/tigo_monitor tests/host/master_core_test.cpp
//       components/tigo_master/master_core.cpp -lpthread -o master_core_test

#include "../../components/tigo_master/master_core.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace esphome::tigo_master;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

// A port whose drain returns late, as a Linux UART's does: tcdrain checks
// the transmitter once a scheduler tick (4 ms at HZ=250), so it can return
// well after the last byte left, with the TAP's reply already read in.
class LateDrainPort : public Transport {
 public:
  explicit LateDrainPort(int drain_ms) : drain_ms_(drain_ms) {}
  bool configured() const override { return true; }
  bool open(std::string &) override { return open_ = true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  bool write(const uint8_t *, size_t, std::string &) override { return true; }
  void drain() override { std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms_)); }
  int read(char *, size_t, int) override { return 0; }
  int baud() const override { return 38400; }
  std::string name() const override { return "fake"; }

 private:
  int drain_ms_;
  bool open_ = false;
};

// A port that keeps the commands written and answers nothing.
class RecordingPort : public Transport {
 public:
  bool configured() const override { return true; }
  bool open(std::string &) override { return open_ = true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  bool write(const uint8_t *data, size_t n, std::string &) override {
    for (size_t i = 0; i < n; i++)
      splitter_.feed(static_cast<char>(data[i]), [&](const std::string &body) {
        esphome::tigo_monitor::master::Frame f;
        if (esphome::tigo_monitor::master::decode_frame(body, f)) cmds.push_back(f.cmd);
      });
    return true;
  }
  void drain() override {}
  int read(char *, size_t, int) override { return 0; }
  int baud() const override { return 38400; }
  std::string name() const override { return "fake"; }
  std::vector<uint16_t> cmds;

 private:
  bool open_ = false;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
};

// Running the bus, the TAP's version is asked once at the start, before the
// first poll, so the Devices page has it without a bring-up.
static void test_the_tap_version_is_asked_once_at_start() {
  RecordingPort port;
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  CHECK(core.open());
  for (uint32_t t = 0; t < 2000; t += 100) core.tick(t);
  CHECK(!port.cmds.empty() && port.cmds[0] == 0x000A);
  int versions = 0, polls = 0;
  for (uint16_t c : port.cmds) {
    versions += c == 0x000A;
    polls += c == 0x0148;
  }
  CHECK(versions == 1 && polls > 0);
  core.close();
}

// on_tx is told how long after its last byte left the request is reported,
// so the Bus log's round trip starts there and not when drain came back.
static void test_a_late_drain_is_reported() {
  LateDrainPort port(20);
  int calls = 0;
  uint32_t late = 0;
  size_t bytes = 0;
  MasterCore core(&port, [](int, const std::string &) {}, nullptr,
                  [&](const char *, size_t n, uint32_t late_ms) {
                    calls++;
                    bytes = n;
                    late = late_ms;
                  },
                  0x1201, false, false);
  CHECK(core.open());
  core.tick(0);
  CHECK(calls >= 1);
  // 20 ms of drain, less the frame's own few ms on the wire at 38400.
  uint32_t wire_ms = static_cast<uint32_t>(bytes * 10 * 1000 / 38400);
  CHECK(late + wire_ms >= 18 && late + wire_ms <= 40);
  core.close();
}

static void test_a_prompt_drain_is_not_late() {
  LateDrainPort port(0);
  uint32_t late = 99;
  MasterCore core(&port, [](int, const std::string &) {}, nullptr,
                  [&](const char *, size_t, uint32_t late_ms) { late = late_ms; }, 0x1201, false, false);
  CHECK(core.open());
  core.tick(0);
  CHECK(late <= 1);
  core.close();
}

int main() {
  test_a_late_drain_is_reported();
  test_a_prompt_drain_is_not_late();
  test_the_tap_version_is_asked_once_at_start();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
