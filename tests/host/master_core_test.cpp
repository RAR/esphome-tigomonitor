// MasterCore against a fake port: what it tells on_tx about when a request
// went out.
//
//   g++ -std=c++17 -O2 -I components/tigo_monitor tests/host/master_core_test.cpp
//       components/tigo_master/master_core.cpp -lpthread -o master_core_test

#include "../../components/tigo_master/master_core.h"

#include <algorithm>
#include <chrono>
#include <functional>
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

// PV held off: the switch tells the TAP 00 at the start.
static int sessions_sent(bool silent) {
  RecordingPort port;
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false, /*housekeeping=*/true);
  core.set_pv({true, false, 0}, nullptr, silent);
  CHECK(core.open());
  for (uint32_t t = 0; t < 3000; t += 100) core.tick(t);
  core.close();
  int n = 0;
  for (uint16_t c : port.cmds) n += c == 0x0B00;
  return n;
}

// A pv.json that couldn't be read says nothing to the TAP about PV, not
// even the hourly frame's 01: the TAP keeps what it was last told.
static void test_an_unreadable_pv_state_says_nothing() {
  CHECK(sessions_sent(false) >= 1);
  CHECK(sessions_sent(true) == 0);
}

// A port where another master is running the bus: every read returns one
// of its poll requests.
class OtherMasterPort : public Transport {
 public:
  explicit OtherMasterPort(uint16_t dest = 0x1201) : dest_(dest) {}
  bool configured() const override { return true; }
  bool open(std::string &) override { return open_ = true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  bool write(const uint8_t *, size_t, std::string &) override {
    writes++;
    return true;
  }
  void drain() override {}
  int read(char *buf, size_t n, int) override {
    auto f = esphome::tigo_monitor::master::encode_frame(dest_, 0x0148, {0x00, 0x00});
    size_t k = std::min(n, f.size());
    std::copy(f.begin(), f.begin() + k, buf);
    return static_cast<int>(k);
  }
  int baud() const override { return 38400; }
  std::string name() const override { return "fake"; }
  int writes = 0;

 private:
  uint16_t dest_;
  bool open_ = false;
};

// Stopped by another master, this one sends nothing more but goes on reading,
// so the panels the other polls still reach the Monitor (and Home Assistant)
// rather than going stale.
static void test_a_master_stopped_by_another_keeps_reading() {
  OtherMasterPort port;
  size_t rx = 0;
  MasterCore core(&port, [](int, const std::string &) {}, [&](const char *, size_t n) { rx += n; }, nullptr, 0x1201,
                  false, false);
  CHECK(core.open());
  for (uint32_t t = 0; t < 1000; t += 100) core.tick(t);
  CHECK(core.status().fault == Fault::second_master);
  int writes = port.writes;
  size_t before = rx;
  for (uint32_t t = 1000; t < 2000; t += 100) core.tick(t);
  CHECK(port.writes == writes);
  CHECK(rx > before);
  core.close();
}

namespace mp = esphome::tigo_monitor::master;

// A port with a TAP behind it: each request written is answered, or not, by
// answer(). Keeps what was sent, as dest/cmd.
class TapPort : public Transport {
 public:
  std::function<bool(const mp::Frame &, mp::Frame &)> answer;
  std::vector<std::pair<uint16_t, uint16_t>> sent;
  bool configured() const override { return true; }
  bool open(std::string &) override { return open_ = true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  bool write(const uint8_t *data, size_t n, std::string &) override {
    for (size_t i = 0; i < n; i++)
      splitter_.feed(static_cast<char>(data[i]), [&](const std::string &body) {
        mp::Frame f, r;
        if (!mp::decode_frame(body, f)) return;
        sent.push_back({f.dest, f.cmd});
        if (answer && answer(f, r)) {
          auto w = mp::encode_frame(r.dest, r.cmd, r.payload);
          pending_.insert(pending_.end(), w.begin(), w.end());
        }
      });
    return true;
  }
  void drain() override {}
  int read(char *buf, size_t n, int) override {
    size_t k = std::min(n, pending_.size());
    std::copy(pending_.begin(), pending_.begin() + k, buf);
    pending_.erase(pending_.begin(), pending_.begin() + k);
    return static_cast<int>(k);
  }
  int baud() const override { return 38400; }
  std::string name() const override { return "fake"; }
  bool sent_cmd(uint16_t cmd) const {
    for (auto &s : sent)
      if (s.second == cmd) return true;
    return false;
  }

 private:
  bool open_ = false;
  std::vector<uint8_t> pending_;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
};

static const uint16_t GW = 0x1201, FACTORY = 0x1235;
// GET_MAC_NOW's answer: the TAP's MAC (fake, last byte varied) and its id.
static mp::Frame mac_reply(uint16_t at, uint8_t which) {
  mp::Frame r;
  r.dest = at | 0x8000;
  r.cmd = 0x003B;
  r.payload = {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x00, which, static_cast<uint8_t>(at >> 8), static_cast<uint8_t>(at)};
  return r;
}
static bool readdressed(const TapPort &port) { return port.sent_cmd(0x003C) || port.sent_cmd(0x0014); }

// One missed answer at the TAP's own address is not a TAP gone back to the
// factory address: whatever answers there (another TAP) is left alone.
static void test_one_missed_probe_readdresses_nothing() {
  TapPort port;
  int asked = 0;
  port.answer = [&](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd != 0x003A) return false;
    if (f.dest == GW && asked++ == 0) return false;  // the first answer is lost
    r = mac_reply(f.dest, f.dest == GW ? 1 : 2);
    return true;
  };
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, /*startup=*/true);
  CHECK(core.open());
  for (uint32_t t = 0; t < 3000; t += 100) core.tick(t);
  CHECK(!readdressed(port));
  core.close();
}

// The TAP polled goes quiet and one answers at the factory address: the same
// TAP (its address lost in a power cut) is brought up; a different one is not.
static bool readdresses_after_loss(uint8_t factory_mac) {
  TapPort port;
  bool lost = false;
  port.answer = [&](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd == 0x0148 && f.dest == GW) lost = true;  // running: from now on the TAP is gone from GW
    if (f.cmd != 0x003A) return false;
    if (f.dest == GW && !lost) {
      r = mac_reply(GW, 1);
      return true;
    }
    if (f.dest == FACTORY && lost) {
      r = mac_reply(FACTORY, factory_mac);
      return true;
    }
    return false;
  };
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, /*startup=*/true);
  CHECK(core.open());
  for (uint32_t t = 0; t < 20000; t += 100) core.tick(t);
  core.close();
  return readdressed(port);
}

static void test_only_the_same_tap_is_readdressed() {
  CHECK(readdresses_after_loss(1));
  CHECK(!readdresses_after_loss(2));
}

// A request to 0xFFFF is a broadcast, not a reply: a standby that hears only
// another master's broadcasts is hearing a master, and doesn't take over.
static void test_a_broadcast_is_a_master_heard() {
  OtherMasterPort port(0xFFFF);
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_standby(1, nullptr, nullptr);
  CHECK(core.open());
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  for (uint32_t t = 0; std::chrono::steady_clock::now() < end; t += 50) {
    core.tick(t);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(port.writes == 0);
  core.close();
}

// A master stopped by another sends nothing more, so it can't take a PV
// request: saying yes would show a switch that never reached the TAP.
static void test_a_stopped_master_refuses_pv() {
  OtherMasterPort port;
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  CHECK(core.open());
  for (uint32_t t = 0; t < 500; t += 100) core.tick(t);
  CHECK(core.status().fault == Fault::second_master);
  CHECK(!core.request_pv(true));
  core.close();
}

// A new PV request isn't confirmed until the TAP answers it.
static void test_a_pv_request_clears_the_last_confirmation() {
  TapPort port;
  port.answer = [](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd != 0x0B00) return false;
    r.dest = f.dest | 0x8000;
    r.cmd = 0x0B01;
    r.payload = f.payload;
    return true;
  };
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_pv({true, false, 0}, nullptr);
  CHECK(core.open());
  for (uint32_t t = 0; t < 3000; t += 100) core.tick(t);
  CHECK(core.status().pv_confirmed);
  CHECK(core.request_pv(false));
  CHECK(!core.status().pv_confirmed);
  core.close();
}

// The TAP's radio settings: read once at the start (tunnel 0x0D 00 00, the
// CCA's own bring-up read) only when asked; off, no 0x0D goes out.
static int config_reads(const std::vector<std::vector<uint8_t>> &tunnels) {
  int n = 0;
  for (auto &p : tunnels) n += p.size() >= 7 && p[3] == 0x0D && p[5] == 0x00 && p[6] == 0x00;
  return n;
}
static void run_opening(bool read_config, std::vector<std::vector<uint8_t>> &tunnels, bool housekeeping = true) {
  TapPort port;
  // The TAP answers the startup probe at its own address, as on the roof.
  port.answer = [&](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd == 0x0B0F) tunnels.push_back(f.payload);
    if (f.cmd != 0x003A || f.dest != GW) return false;
    r = mac_reply(GW, 1);
    return true;
  };
  // Startup on, as the read is a bring-up frame the send gate allows only then.
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, true, housekeeping);
  if (read_config) core.set_read_tap_config(true);
  CHECK(core.open());
  for (uint32_t t = 0; t < 20000; t += 50) core.tick(t);
  core.close();
}
static void test_the_tap_config_is_not_read_unless_asked() {
  std::vector<std::vector<uint8_t>> tunnels;
  run_opening(false, tunnels);
  CHECK(config_reads(tunnels) == 0);
}
static void test_the_tap_config_is_read_once_when_asked() {
  std::vector<std::vector<uint8_t>> tunnels;
  run_opening(true, tunnels);
  CHECK(config_reads(tunnels) == 1);
}
// The switch says "once each time the master starts", housekeeping or not.
static void test_the_tap_config_is_read_without_housekeeping() {
  std::vector<std::vector<uint8_t>> tunnels;
  run_opening(true, tunnels, false);
  CHECK(config_reads(tunnels) == 1);
  // Nothing of housekeeping's comes with it: only the window's own reads.
  for (auto &p : tunnels) CHECK(p.size() >= 4 && (p[3] == 0x0D || p[3] == 0x2E));
}

// Dropped before it went out (the port reopened, as when the TAP is lost),
// the read isn't lost: the next opening sends it, still only once.
static void test_the_tap_config_read_survives_a_drop_before_it_is_sent() {
  std::vector<std::vector<uint8_t>> tunnels;
  TapPort port;
  port.answer = [&](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd == 0x0B0F) tunnels.push_back(f.payload);
    if (f.cmd != 0x003A || f.dest != GW) return false;
    r = mac_reply(GW, 1);
    return true;
  };
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, true, false);
  core.set_read_tap_config(true);
  CHECK(core.open());
  uint32_t t = 0;
  while (t < 20000 && tunnels.empty()) core.tick(t += 50);  // the opening has begun
  CHECK(!tunnels.empty());
  CHECK(config_reads(tunnels) == 0);
  core.close();
  CHECK(core.open());
  for (uint32_t end = t + 20000; t < end; t += 50) core.tick(t);
  core.close();
  CHECK(config_reads(tunnels) == 1);
}

// Automatic discovery: the window is renewed only while wanted, and the
// owner's "let it close" still wins. The TAP here says its window has run
// out (0 s left), which is when a renewal goes.
static int renewals_sent(bool owner_renew, bool wanted) {
  TapPort port;
  int renewals = 0;
  port.answer = [&](const mp::Frame &f, mp::Frame &r) {
    if (f.cmd != 0x0B0F || f.payload.size() < 5) return false;
    uint8_t sub = f.payload[3], seq = f.payload[4];
    if (sub == 0x2D) renewals++;
    if (sub != 0x2E && sub != 0x2D) return false;
    r.dest = GW | 0x8000;
    r.cmd = 0x0B10;
    r.payload = {0x00, 0x0E, 0x00, 0x2F, seq, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x28, 0x00, 0x2C};
    return true;
  };
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_discovery(owner_renew, nullptr);
  core.set_window_wanted(wanted);
  CHECK(core.open());
  for (uint32_t t = 0; t < 8000; t += 50) core.tick(t);
  core.close();
  return renewals;
}
static void test_discovery_renews_only_while_wanted() {
  CHECK(renewals_sent(true, true) > 0);
  CHECK(renewals_sent(true, false) == 0);
}
static void test_the_owner_switch_beats_wanted() {
  CHECK(renewals_sent(false, true) == 0);
}

// The table add (table_edit.h) through the core: a TAP with two optimizers
// listed, that takes one record and says its window has run out.
struct TableTap {
  TapPort port;
  std::vector<mp::Bytes> records = {
      {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x02},
      {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x03}};
  std::vector<mp::Bytes> writes;
  int renewals = 0;
  int erases = 0;
  int silent_after_erase = 0;  // requests unanswered after an erase: the TAP restarting
  int silent_left = 0;
  TableTap() {
    port.answer = [this](const mp::Frame &f, mp::Frame &r) {
      if (silent_left > 0) {
        silent_left--;
        return false;
      }
      if (f.cmd == 0x0148 && f.payload.size() == 5) {  // empty, as the bench TAP's: 00 1F, the cursor, a byte
        r.dest = GW | 0x8000;
        r.cmd = 0x0149;
        r.payload = {0x00, 0x1F, f.payload[2], f.payload[3], 0x00};
        return true;
      }
      if (f.cmd != 0x0B0F || f.payload.size() < 5) return false;
      uint8_t sub = f.payload[3], seq = f.payload[4];
      mp::Bytes args(f.payload.begin() + 5, f.payload.end());
      r.dest = GW | 0x8000;
      r.cmd = 0x0B10;
      r.payload = {0x00, 0x0E, 0x00, static_cast<uint8_t>(sub + 1), seq};
      if (sub == 0x2E || sub == 0x2D) {
        renewals += sub == 0x2D;
        r.payload[3] = 0x2F;
        mp::Bytes w = {0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, static_cast<uint8_t>(records.size())};
        r.payload.insert(r.payload.end(), w.begin(), w.end());
        return true;
      }
      if (sub == 0x26) {
        // From the id asked for, as the roof's TAP pages (ids here ascend).
        uint16_t asked = static_cast<uint16_t>(args[0] << 8 | args[1]);
        std::vector<mp::Bytes> page;
        for (const auto &rec : records)
          if (((rec[8] & 0x7F) << 8 | rec[9]) >= asked) page.push_back(rec);
        uint16_t first = page.empty() ? asked : static_cast<uint16_t>((page[0][8] & 0x7F) << 8 | page[0][9]);
        mp::Bytes d = {static_cast<uint8_t>(first >> 8), static_cast<uint8_t>(first), 0x00,
                       static_cast<uint8_t>(page.size())};
        for (const auto &rec : page) d.insert(d.end(), rec.begin(), rec.end());
        r.payload.insert(r.payload.end(), d.begin(), d.end());
        return true;
      }
      if (sub == 0x2B) {
        erases++;
        if (erases == 1) silent_left = silent_after_erase;
        records.clear();
        r.payload.push_back(0x00);
        return true;
      }
      if (sub == 0x29) {
        writes.push_back(args);
        for (size_t i = 0; i < args[0]; i++) records.push_back(mp::Bytes(args.begin() + 1 + 10 * i, args.begin() + 11 + 10 * i));
        r.payload.push_back(0x00);
        return true;
      }
      return false;
    };
  }
};
static const mp::Bytes NEW_MAC = {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x12, 0x34};

static void test_a_table_add_needs_the_setting() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  CHECK(core.open());
  std::string why;
  CHECK(!core.request_table_add(NEW_MAC, 0, false, why));
  CHECK(why.find("master_table_writes") != std::string::npos);
  for (uint32_t t = 0; t < 5000; t += 50) core.tick(t);
  CHECK(tap.writes.empty());
  core.close();
}

static void test_a_dry_run_sends_no_write() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  CHECK(core.open());
  std::string why;
  CHECK(core.request_table_add(NEW_MAC, 0, true, why));
  for (uint32_t t = 0; t < 5000; t += 50) core.tick(t);
  CHECK(tap.writes.empty());
  MasterStatus s = core.status();
  CHECK(s.table_edit.finished && s.table_edit.ok && s.table_edit.dry_run);
  CHECK(s.table_edit.dry_frames.size() == 1);
  CHECK(s.table_edit.dry_frames[0] == "29" "01" "04C05B0000001234" "8004");
  CHECK(s.table_edit.kind == "add");
  core.close();
}

// With the setting on: one write, and the window held open for the join
// even though automatic discovery wants it closed.
static void test_a_table_add_writes_once_and_holds_the_window() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  core.set_window_wanted(false);
  CHECK(core.open());
  std::string why;
  CHECK(core.request_table_add(NEW_MAC, 0, false, why));
  CHECK(!core.request_table_add(NEW_MAC, 0, false, why));  // one change at a time
  for (uint32_t t = 0; t < 30000; t += 50) core.tick(t);
  CHECK(tap.writes.size() == 1);
  CHECK(tap.writes.size() == 1 && tap.writes[0] == (mp::Bytes{0x01, 0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x12, 0x34, 0x80, 0x04}));
  MasterStatus s = core.status();
  // The add is done once listed; the join is waited for apart from it.
  CHECK(s.table_edit.finished && s.table_edit.ok && s.table_edit.id == 4);
  CHECK(s.table_edit.message == "listed at 0004; waiting for it to join");
  CHECK(s.table_waiting.size() == 1 && s.table_waiting[0].id == 4 && s.table_waiting[0].mac == "04C05B0000001234");
  CHECK(tap.renewals > 0);
  // It joins: the TAP clears the bit; the next read finds it, and the
  // window goes back to what discovery wants.
  tap.records.back()[8] &= 0x7F;
  for (uint32_t t = 30000; t < 120000; t += 50) core.tick(t);
  s = core.status();
  CHECK(s.table_waiting.empty());
  CHECK(s.table_joins.size() == 1 && s.table_joins[0].joined && s.table_joins[0].text == "joined at 0004");
  int renewals = tap.renewals;
  for (uint32_t t = 120000; t < 160000; t += 50) core.tick(t);
  CHECK(tap.renewals == renewals);
  core.close();
}

// Several added back to back, waited for together; one given up on by the
// owner, one removed: the window is held until none is left.
static void test_several_adds_are_waited_for_together() {
  TableTap tap;
  int saves = 0;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  core.set_window_wanted(false);
  core.set_table_saved([&](const mp::Bytes &, const std::vector<mp::TableRecord> *) { saves++; return true; });
  CHECK(core.open());
  std::string why;
  mp::Bytes a = NEW_MAC, b = NEW_MAC, c = NEW_MAC;
  b[7] = 0x35;
  c[7] = 0x36;
  uint32_t t = 0;
  for (const mp::Bytes &m : {a, b, c}) {
    CHECK(core.request_table_add(m, 0, false, why));
    for (uint32_t end = t + 3000; t < end; t += 50) core.tick(t);
  }
  CHECK(tap.writes.size() == 3);
  MasterStatus s = core.status();
  CHECK(s.table_waiting.size() == 3);
  CHECK(!core.request_table_stop_waiting({0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x99, 0x99}));
  CHECK(core.request_table_stop_waiting(b));
  CHECK(core.request_table_remove(c, false, why));
  for (uint32_t end = t + 5000; t < end; t += 50) core.tick(t);
  s = core.status();
  CHECK(s.table_waiting.size() == 1 && s.table_waiting[0].mac == "04C05B0000001234");
  CHECK(tap.erases == 1);
  int renewals = tap.renewals;
  for (uint32_t end = t + 20000; t < end; t += 50) core.tick(t);
  CHECK(tap.renewals > renewals);  // still held for a
  CHECK(core.request_table_stop_waiting(a));
  for (uint32_t end = t + 3000; t < end; t += 50) core.tick(t);
  renewals = tap.renewals;
  for (uint32_t end = t + 40000; t < end; t += 50) core.tick(t);
  CHECK(tap.renewals == renewals);
  CHECK(core.status().table_waiting.empty());
  core.close();
}

// A removal through the core: what stays is saved before the erase and
// cleared once it is written back; without a place to save it, refused.
static void test_a_removal_saves_then_erases() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  std::vector<std::string> events;
  std::string why;
  CHECK(!core.request_table_remove(tap.records[0], false, why));  // nowhere to save yet
  mp::Bytes first(tap.records[0].begin(), tap.records[0].begin() + 8);
  core.set_table_saved([&](const mp::Bytes &mac, const std::vector<mp::TableRecord> *stay) {
    CHECK(mac == first);
    events.push_back(stay ? "saved " + std::to_string(stay->size()) + " at erase " + std::to_string(tap.erases) : "cleared");
    return true;
  });
  CHECK(core.open());
  CHECK(core.request_table_remove(first, false, why));
  for (uint32_t t = 0; t < 20000; t += 50) core.tick(t);
  CHECK(tap.erases == 1);
  CHECK(tap.records.size() == 1 && tap.records[0][7] == 0x02);
  CHECK(events.size() == 2 && events[0] == "saved 1 at erase 0" && events[1] == "cleared");
  MasterStatus s = core.status();
  CHECK(s.table_edit.kind == "remove" && s.table_edit.finished && s.table_edit.ok && !s.table_edit.interrupted);
  core.close();
}

// A saved list from a removal that stopped: finished at the start with the
// setting on; with it off, nothing is sent and every change is refused.
static void test_an_interrupted_removal_is_finished() {
  std::vector<mp::TableRecord> stay = {{{0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x01, 0x02}, 0x0003}};
  mp::Bytes gone = {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x01, 0x01};
  {
    TableTap tap;
    tap.records.clear();  // stopped after the erase
    MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
    bool cleared = false;
    core.set_table_saved([&](const mp::Bytes &, const std::vector<mp::TableRecord> *s) {
      if (!s) cleared = true;
      return true;
    }, gone, &stay);
    CHECK(core.open());
    for (uint32_t t = 0; t < 20000; t += 50) core.tick(t);
    CHECK(tap.erases == 0);
    CHECK(tap.writes.empty());
    MasterStatus s = core.status();
    CHECK(s.table_edit.interrupted && !s.table_edit.finished);
    std::string why;
    CHECK(!core.request_table_add(NEW_MAC, 0, true, why));
    CHECK(!cleared);
    core.close();
  }
  {
    TableTap tap;
    tap.records.clear();
    MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
    core.set_table_writes(true);
    bool cleared = false;
    core.set_table_saved([&](const mp::Bytes &, const std::vector<mp::TableRecord> *s) {
      if (!s) cleared = true;
      return true;
    }, gone, &stay);
    CHECK(core.open());
    for (uint32_t t = 0; t < 20000; t += 50) core.tick(t);
    CHECK(tap.writes.size() == 1);
    CHECK(tap.records.size() == 1 && tap.records[0][7] == 0x02 && tap.records[0][9] == 0x03);
    CHECK(cleared);
    MasterStatus s = core.status();
    CHECK(s.table_edit.finished && s.table_edit.ok && !s.table_edit.interrupted);
    core.close();
  }
}

// A TAP that restarts between the erase and the write back: the removal
// stops with what stays saved, and is finished once the TAP answers again,
// without waiting for a restart of the master.
static void test_a_removal_cut_off_by_a_tap_restart_is_finished() {
  TableTap tap;
  tap.silent_after_erase = 40;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  int clears = 0;
  core.set_table_saved([&](const mp::Bytes &, const std::vector<mp::TableRecord> *stay) {
    if (!stay) clears++;
    return true;
  });
  CHECK(core.open());
  mp::Bytes first(tap.records[0].begin(), tap.records[0].begin() + 8);
  std::string why;
  CHECK(core.request_table_remove(first, false, why));
  uint32_t t = 0;
  for (; t < 30000; t += 50) core.tick(t);
  MasterStatus s = core.status();
  CHECK(tap.erases == 1 && tap.writes.empty());
  CHECK(s.table_edit.interrupted);
  CHECK(!core.request_table_add(NEW_MAC, 0, true, why));  // nothing else meanwhile
  CHECK(clears == 0);
  for (; t < 200000; t += 50) core.tick(t);
  s = core.status();
  CHECK(s.table_edit.finished && s.table_edit.ok && !s.table_edit.interrupted);
  CHECK(tap.writes.size() == 1);
  CHECK(tap.records.size() == 1 && tap.records[0][7] == 0x02);
  CHECK(clears == 1);
  core.close();
}

// A replace through the core: one erase, the new optimizer listed at the
// old one's id, then waited for like an add.
static void test_a_replace_lists_the_new_one_and_waits() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  core.set_window_wanted(false);
  core.set_table_saved([](const mp::Bytes &, const std::vector<mp::TableRecord> *) { return true; });
  CHECK(core.open());
  mp::Bytes old(tap.records[1].begin(), tap.records[1].begin() + 8);  // at 0003
  std::string why;
  CHECK(core.request_table_replace(old, NEW_MAC, false, why));
  uint32_t t = 0;
  for (; t < 10000; t += 50) core.tick(t);
  MasterStatus s = core.status();
  CHECK(s.table_edit.kind == "replace" && s.table_edit.ok && s.table_edit.id == 3);
  CHECK(tap.erases == 1);
  CHECK(tap.records.size() == 2 && tap.records[1][7] == 0x34 && tap.records[1][8] == 0x80 && tap.records[1][9] == 0x03);
  CHECK(s.table_waiting.size() == 1 && s.table_waiting[0].mac == "04C05B0000001234" && s.table_waiting[0].id == 3);
  int renewals = tap.renewals;
  for (; t < 40000; t += 50) core.tick(t);
  CHECK(tap.renewals > renewals);
  core.close();
}

int main() {
  test_a_replace_lists_the_new_one_and_waits();
  test_a_removal_cut_off_by_a_tap_restart_is_finished();
  test_a_removal_saves_then_erases();
  test_an_interrupted_removal_is_finished();
  test_a_table_add_needs_the_setting();
  test_a_dry_run_sends_no_write();
  test_a_table_add_writes_once_and_holds_the_window();
  test_several_adds_are_waited_for_together();
  test_discovery_renews_only_while_wanted();
  test_the_owner_switch_beats_wanted();
  test_the_tap_config_is_not_read_unless_asked();
  test_the_tap_config_is_read_once_when_asked();
  test_the_tap_config_is_read_without_housekeeping();
  test_the_tap_config_read_survives_a_drop_before_it_is_sent();
  test_a_late_drain_is_reported();
  test_a_prompt_drain_is_not_late();
  test_the_tap_version_is_asked_once_at_start();
  test_an_unreadable_pv_state_says_nothing();
  test_a_master_stopped_by_another_keeps_reading();
  test_one_missed_probe_readdresses_nothing();
  test_only_the_same_tap_is_readdressed();
  test_a_broadcast_is_a_master_heard();
  test_a_stopped_master_refuses_pv();
  test_a_pv_request_clears_the_last_confirmation();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
