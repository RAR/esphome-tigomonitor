// MasterCore against a fake port: what it tells on_tx about when a request
// went out.
//
//   g++ -std=c++17 -O2 -I components/tigo_monitor tests/host/master_core_test.cpp
//       components/tigo_master/master_core.cpp -lpthread -o master_core_test

#include "../../components/tigo_master/master_core.h"

#include <algorithm>
#include <atomic>
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
  // Instead of answer: any number of replies (two TAPs answering one GET_MAC).
  std::function<std::vector<mp::Frame>(const mp::Frame &)> answer_all;
  std::vector<std::pair<uint16_t, uint16_t>> sent;
  std::vector<mp::Frame> frames;  // every request, whole
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
        frames.push_back(f);
        if (answer_all) {
          for (const auto &a : answer_all(f)) {
            auto w = mp::encode_frame(a.dest, a.cmd, a.payload);
            pending_.insert(pending_.end(), w.begin(), w.end());
          }
        } else if (answer && answer(f, r)) {
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
  std::vector<std::string> removed;
  core.set_on_removed([&](const std::string &mac) { removed.push_back(mac); });
  CHECK(core.open());
  CHECK(core.request_table_remove(first, true, why));  // a dry run removes nothing
  for (uint32_t t = 0; t < 5000; t += 50) core.tick(t);
  CHECK(removed.empty());
  CHECK(core.request_table_remove(first, false, why));
  for (uint32_t t = 5000; t < 25000; t += 50) core.tick(t);
  CHECK(removed.size() == 1 && removed[0] == "04C05B0000000101");
  CHECK(tap.erases == 1);
  CHECK(tap.records.size() == 1 && tap.records[0][7] == 0x02);
  CHECK(events.size() == 2 && events[0] == "saved 1 at erase 0" && events[1] == "cleared");
  MasterStatus s = core.status();
  CHECK(s.table_edit.kind == "remove" && s.table_edit.finished && s.table_edit.ok && !s.table_edit.interrupted);
  core.close();
}

// Several in one removal: each one taken off is told, once.
static void test_a_removal_of_several() {
  TableTap tap;
  MasterCore core(&tap.port, [](int, const std::string &) {}, nullptr, nullptr, GW, false, false);
  core.set_table_writes(true);
  core.set_table_saved([](const mp::Bytes &, const std::vector<mp::TableRecord> *) { return true; });
  std::vector<std::string> removed;
  core.set_on_removed([&](const std::string &mac) { removed.push_back(mac); });
  CHECK(core.open());
  std::vector<mp::Bytes> both;
  for (const auto &r : tap.records) both.emplace_back(r.begin(), r.begin() + 8);
  std::string why;
  CHECK(core.request_table_remove(both, false, why));
  for (uint32_t t = 0; t < 20000; t += 50) core.tick(t);
  CHECK(tap.erases == 1 && tap.records.empty());
  CHECK(removed.size() == 2);
  CHECK(core.status().table_edit.ok);
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
  std::vector<std::string> replaced;
  core.set_on_replaced([&](const std::string &o, const std::string &n) { replaced.push_back(o + ">" + n); });
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
  CHECK(replaced.empty());  // not until it joins
  tap.records[1][8] &= 0x7F;
  for (; t < 130000; t += 50) core.tick(t);
  CHECK(replaced.size() == 1 && replaced[0] == "04C05B0000000102>04C05B0000001234");
  core.close();
}

// ---- more than one TAP (docs/superpowers/specs/2026-11-10-multi-tap-design.md)

// A bus with several TAPs, each answering at its own id with its own table:
// polls (empty), discovery reads and renewals, VERSION, GET_MAC_NOW, table
// pages, writes and erases. A TAP with up false answers nothing.
struct MultiTap {
  struct One {
    One(uint16_t i, uint8_t m, bool u = true) : id(i), mac_last(m), up(u), stored(i) {}
    uint16_t id;
    uint8_t mac_last;
    bool up;
    uint16_t stored;  // where a RESET puts it; 0: the factory address (a new TAP)
    bool window_closed = false;  // its discovery window reads closed, 0 s left
    // Its radio settings (GatewayCfg): read with 0D 00 00, written with
    // 0D 01 00, the stored copy read with 0D 00 01 and saved by the Store.
    bool has_cfg = false, cfg_silent = false;
    mp::RadioBlock cfg, cfg_stored;
    int cfg_reads = 0;
    // The 0xE1s it was sent (their args), and the status it answers with.
    std::vector<mp::Bytes> e1;
    uint8_t e1_status = 0;
    int cfg_silent_reads = 0;  // this many 0D 00 00 reads go unanswered first
    int e1_silent = 0;         // this many 0xE1s go unanswered first; -1: all
    std::vector<mp::Bytes> records;
    std::vector<mp::Bytes> writes;
  };
  TapPort port;
  std::vector<One> taps;
  std::vector<uint16_t> dests;  // each request's dest, in order
  One *at(uint16_t id) {
    for (auto &t : taps)
      if (t.id == id) return &t;
    return nullptr;
  }
  MultiTap() {
    port.answer_all = [this](const mp::Frame &f) {
      std::vector<mp::Frame> out;
      // The bring-up's, to every TAP: as SimBus in master_protocol_test.
      if (f.cmd == 0x0014 && f.dest == 0) {
        for (auto &t : taps)
          if (t.up) t.id = 0x1235;
        mp::Frame a;
        a.dest = 0x8000;
        a.cmd = 0x0015;
        out.push_back(a);
        return out;
      }
      if (f.cmd == 0x0010 && f.dest == 0) {
        for (auto &t : taps) t.id = t.stored ? t.stored : 0x1235;
        return out;
      }
      if (f.cmd == 0x0038) {
        for (auto &t : taps)
          if (t.up && t.id == f.dest) {
            mp::Frame a = mac_reply(t.id, t.mac_last);
            a.cmd = 0x0039;
            out.push_back(a);
          }
        return out;
      }
      if (f.cmd == 0x003C && f.payload.size() == 14) {
        for (auto &t : taps)
          if (t.up && t.id == f.dest && f.payload[11] == t.mac_last) {
            mp::Frame a;
            a.dest = t.id | 0x8000;
            a.cmd = 0x003D;
            out.push_back(a);
            t.id = static_cast<uint16_t>(f.payload[12] << 8 | f.payload[13]);
            break;
          }
        return out;
      }
      if (f.cmd == 0x0012) {
        for (auto &t : taps)
          if (t.up && t.id == f.dest) {
            t.stored = t.id;
            t.cfg_stored = t.cfg;
            mp::Frame a;
            a.dest = t.id | 0x8000;
            a.cmd = 0x0013;
            out.push_back(a);
          }
        return out;
      }
      mp::Frame r;
      if (one(f, r)) out.push_back(r);
      return out;
    };
  }
  bool one(const mp::Frame &f, mp::Frame &r) {
    {
      dests.push_back(f.dest);
      One *t = at(f.dest);
      if (!t || !t->up) return false;
      r.dest = f.dest | 0x8000;
      if (f.cmd == 0x003A) {
        r = mac_reply(f.dest, t->mac_last);
        return true;
      }
      if (f.cmd == 0x000A) {
        r.cmd = 0x000B;
        std::string v = "Mgate Version G8.59";
        r.payload.assign(v.begin(), v.end());
        return true;
      }
      if (f.cmd == 0x0148 && f.payload.size() == 5) {
        r.cmd = 0x0149;
        r.payload = {0x00, 0x1F, f.payload[2], f.payload[3], 0x00};
        return true;
      }
      if (f.cmd != 0x0B0F || f.payload.size() < 5) return false;
      uint8_t sub = f.payload[3], seq = f.payload[4];
      mp::Bytes args(f.payload.begin() + 5, f.payload.end());
      r.cmd = 0x0B10;
      r.payload = {0x00, 0x0E, 0x00, static_cast<uint8_t>(sub + 1), seq};
      if (sub == 0x2E || sub == 0x2D) {
        r.payload[3] = 0x2F;
        mp::Bytes w = {0x01, 0x03, 0x84, 0x00, 0x02, 0x00, 0x02, 0x00, static_cast<uint8_t>(t->records.size())};
        if (t->window_closed) w[0] = w[1] = w[2] = 0x00;
        r.payload.insert(r.payload.end(), w.begin(), w.end());
        return true;
      }
      if (sub == 0x26) {
        uint16_t asked = static_cast<uint16_t>(args[0] << 8 | args[1]);
        std::vector<mp::Bytes> page;
        for (const auto &rec : t->records)
          if (((rec[8] & 0x7F) << 8 | rec[9]) >= asked) page.push_back(rec);
        uint16_t first = page.empty() ? asked : static_cast<uint16_t>((page[0][8] & 0x7F) << 8 | page[0][9]);
        mp::Bytes d = {static_cast<uint8_t>(first >> 8), static_cast<uint8_t>(first), 0x00,
                       static_cast<uint8_t>(page.size())};
        for (const auto &rec : page) d.insert(d.end(), rec.begin(), rec.end());
        r.payload.insert(r.payload.end(), d.begin(), d.end());
        return true;
      }
      if (sub == 0xE1) {
        t->e1.push_back(args);
        if (t->e1_silent < 0) return false;
        if (t->e1_silent > 0) {
          t->e1_silent--;
          return false;
        }
        r.payload[3] = 0xE2;
        r.payload.push_back(t->e1_status);
        r.payload.push_back(static_cast<uint8_t>(t->cfg.channel));
        r.payload.push_back(static_cast<uint8_t>(t->cfg.pan >> 8));
        r.payload.push_back(static_cast<uint8_t>(t->cfg.pan));
        return true;
      }
      if (sub == 0x0D && t->has_cfg && !t->cfg_silent) {
        r.payload[3] = 0x0E;
        mp::RadioBlock shown = t->cfg;
        if (args == mp::Bytes{0x00, 0x00}) {
          t->cfg_reads++;
          if (t->cfg_silent_reads > 0) {
            t->cfg_silent_reads--;
            return false;
          }
        } else if (args == mp::Bytes{0x00, 0x01}) {
          shown = t->cfg_stored;
        } else if (args.size() > 2 && args[0] == 0x01 && args[1] == 0x00) {
          mp::RadioBlock w;
          if (!mp::RadioBlock::decode(mp::Bytes(args.begin() + 2, args.end()), w)) return false;
          t->cfg = shown = w;
        } else {
          return false;
        }
        mp::Bytes b = shown.encode();
        r.payload.insert(r.payload.end(), b.begin(), b.end());
        r.payload.push_back(0x00);
        return true;
      }
      if (sub == 0x29) {
        t->writes.push_back(args);
        for (size_t i = 0; i < args[0]; i++)
          t->records.push_back(mp::Bytes(args.begin() + 1 + 10 * i, args.begin() + 11 + 10 * i));
        r.payload.push_back(0x00);
        return true;
      }
      return false;
    }
  }
  int sent_to(uint16_t id, uint16_t cmd) const {
    int n = 0;
    for (auto &s : port.sent) n += s.first == id && s.second == cmd;
    return n;
  }
};

static mp::Bytes rec(uint8_t last, uint16_t id) {
  return {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x02, last, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
}

static void run_for(MasterCore &core, uint32_t from, uint32_t to, uint32_t step = 20) {
  for (uint32_t t = from; t < to; t += step) core.tick(t);
}

// Two TAPs: polled one each in turn, as the CCA alternates them, and each
// gets its own discovery reads.
static void test_two_taps_are_polled_in_turn() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 4000);
  int a = bus.sent_to(0x1201, 0x0148), b = bus.sent_to(0x1202, 0x0148);
  CHECK(a > 20 && b > 20 && (a > b ? a - b : b - a) <= 2);
  // No run of three polls to one TAP.
  int run = 0, longest = 0;
  uint16_t prev = 0;
  for (auto &s : bus.port.sent) {
    if (s.second != 0x0148) continue;
    run = s.first == prev ? run + 1 : 1;
    prev = s.first;
    longest = std::max(longest, run);
  }
  CHECK(longest <= 2);
  CHECK(bus.sent_to(0x1201, 0x0B0F) > 0 && bus.sent_to(0x1202, 0x0B0F) > 0);
  CHECK(bus.sent_to(0x1201, 0x000A) == 1 && bus.sent_to(0x1202, 0x000A) == 1);
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.taps[0].id == 0x1201 && m.taps[1].id == 0x1202);
  CHECK(m.polls == m.taps[0].polls + m.taps[1].polls);
  CHECK(m.taps[1].version == "Mgate Version G8.59" && m.tap_version == m.taps[0].version);
  CHECK(m.taps[0].state == "running" && m.taps[1].state == "running");
  core.close();
}

// With more than one TAP, no 0x41: its content is copied from a CCA (or
// built in) and names the TAP the CCA chose, so through another TAP it could
// steer an optimizer onto the wrong one (owner's analysis, 2026-11-10). A node
// that gets none keeps its TAP. The rest of housekeeping goes on; one TAP
// still gets its 0x41s.
static void test_no_0x41_with_more_than_one_tap() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 20000);
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.taps[0].sent_41 == 0 && m.taps[1].sent_41 == 0 && m.sent_41 == 0);
  CHECK(m.taps[0].sent_session > 0 && m.taps[1].sent_session > 0);
  core.close();

  MultiTap one;
  one.taps = {{0x1201, 1}};
  MasterCore single(&one.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false, true);
  CHECK(single.open());
  run_for(single, 0, 20000);
  CHECK(single.status().sent_41 > 0);
  single.close();
}

// One of two TAPs goes quiet: the other goes on being polled at full rate,
// and the quiet one is asked once every LOST_PROBE_MS, then, after
// LOST_MISSES of those, once every REMOVED_PROBE_MS. Back, it is polled again.
static void test_a_quiet_tap_does_not_stop_the_other() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 2000);
  bus.at(0x1202)->up = false;
  run_for(core, 2000, 4000);
  MasterStatus m = core.status();
  CHECK(m.taps[1].state == "lost" && m.taps[0].state == "running");
  CHECK(m.fault == Fault::none);
  int before = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F);
  int first_before = bus.sent_to(0x1201, 0x0148);
  // One try every LOST_PROBE_MS: in 3 of them, 3 tries (a step of 50 ms here).
  run_for(core, 4000, 4000 + 3 * MasterCore::LOST_PROBE_MS, 50);
  int tries = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F) - before;
  CHECK(tries >= 2 && tries <= 4);
  CHECK(bus.sent_to(0x1201, 0x0148) - first_before > 1000);
  // Unanswered LOST_MISSES times: removed, asked every REMOVED_PROBE_MS.
  uint32_t t = 4000 + 3 * MasterCore::LOST_PROBE_MS;
  uint32_t until = t + (MasterCore::LOST_MISSES + 1) * MasterCore::LOST_PROBE_MS;
  for (; t < until; t += 1000) core.tick(t);
  CHECK(core.status().taps[1].state == "removed");
  before = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F);
  for (uint32_t end = t + MasterCore::REMOVED_PROBE_MS; t < end; t += 1000) core.tick(t);
  tries = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F) - before;
  CHECK(tries >= 1 && tries <= 2);
  // Back.
  bus.at(0x1202)->up = true;
  for (uint32_t end = t + MasterCore::REMOVED_PROBE_MS + 2000; t < end; t += 1000) core.tick(t);
  CHECK(core.status().taps[1].state == "running");
  before = bus.sent_to(0x1202, 0x0148);
  run_for(core, t, t + 1000);
  CHECK(bus.sent_to(0x1202, 0x0148) - before > 10);
  core.close();
}

// Every TAP goes quiet at once, as when the bus is cut to wire in another
// TAP: that is the bus, not the TAPs, so each is still asked every
// LOST_PROBE_MS however long it lasts, never REMOVED_PROBE_MS, and polling
// is back within one of those once the bus is. (On the roof, 2026-11-22, the
// 300 s rule left 30 min with no data, until a restart.)
static void test_a_quiet_bus_is_still_asked_often() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 4000);
  CHECK(core.status().taps[0].state == "running" && core.status().taps[1].state == "running");
  bus.at(0x1201)->up = bus.at(0x1202)->up = false;
  uint32_t t = 4000;
  for (uint32_t end = t + (MasterCore::LOST_MISSES + 5) * MasterCore::LOST_PROBE_MS; t < end; t += 1000) core.tick(t);
  MasterStatus m = core.status();
  CHECK(m.taps[0].state != "removed" && m.taps[1].state != "removed");
  int before1 = bus.sent_to(0x1201, 0x0148) + bus.sent_to(0x1201, 0x0B0F);
  int before2 = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F);
  for (uint32_t end = t + 3 * MasterCore::LOST_PROBE_MS; t < end; t += 1000) core.tick(t);
  CHECK(bus.sent_to(0x1201, 0x0148) + bus.sent_to(0x1201, 0x0B0F) - before1 >= 2);
  CHECK(bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F) - before2 >= 2);
  // The bus is back: both polled again within one LOST_PROBE_MS.
  bus.at(0x1201)->up = bus.at(0x1202)->up = true;
  for (uint32_t end = t + MasterCore::LOST_PROBE_MS + 2000; t < end; t += 1000) core.tick(t);
  m = core.status();
  CHECK(m.taps[0].state == "running" && m.taps[1].state == "running");
  before1 = bus.sent_to(0x1201, 0x0148);
  before2 = bus.sent_to(0x1202, 0x0148);
  run_for(core, t, t + 1000);
  CHECK(bus.sent_to(0x1201, 0x0148) - before1 > 5 && bus.sent_to(0x1202, 0x0148) - before2 > 5);
  core.close();
}

// Quiet together, then one comes back: the other is a TAP gone, not the
// bus, and drops to REMOVED_PROBE_MS once it has missed LOST_MISSES tries.
static void test_one_tap_still_quiet_after_the_bus_is_back_is_removed() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 4000);
  bus.at(0x1201)->up = bus.at(0x1202)->up = false;
  uint32_t t = 4000;
  for (uint32_t end = t + (MasterCore::LOST_MISSES + 2) * MasterCore::LOST_PROBE_MS; t < end; t += 1000) core.tick(t);
  bus.at(0x1201)->up = true;
  for (uint32_t end = t + 3 * MasterCore::LOST_PROBE_MS; t < end; t += 1000) core.tick(t);
  MasterStatus m = core.status();
  CHECK(m.taps[0].state == "running" && m.taps[1].state == "removed");
  core.close();
}

// A TAP with no optimizers listed (one just added) reads its window closed
// on every check. Nothing renews it (no renewal carries a count of 0), so
// the log says so once, naming the TAP, not "renewing" every 20 s. (On the
// roof, 2026-11-22: 26 of those lines in an hour for the new TAP.)
static void test_an_empty_tap_s_closed_window_is_logged_once() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  bus.at(0x1201)->records = {rec(0x11, 0x0011)};
  bus.at(0x1202)->window_closed = true;
  std::vector<std::string> lines;
  MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                  false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 5 * mp::WindowKeeper::CHECK_EVERY_MS, 50);
  int renewing = 0, stays = 0;
  for (const auto &l : lines) {
    renewing += l.find("window has closed") != std::string::npos && l.find("renewing") != std::string::npos;
    stays += l.find("1202") != std::string::npos && l.find("stays closed") != std::string::npos;
  }
  CHECK(renewing == 0);
  CHECK(stays == 1);
  // Its window is still read on every check.
  CHECK(bus.sent_to(0x1202, 0x0B0F) >= 4);
  core.close();
}

// ---- one network for every TAP (2026-11-22) ----
// Made-up radio settings: a channel, a PAN, and a key of first, first+1, ...
static mp::RadioBlock radio(uint16_t ch, uint16_t pan, uint8_t key_first, bool wide = false) {
  mp::RadioBlock b;
  b.channel = ch;
  b.pan = pan;
  mp::map_for_profile(20, b.map);
  for (int i = 0; i < 16; i++) b.key[i] = static_cast<uint8_t>(key_first + i);
  b.wide = wide;
  if (wide) b.extra = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA};
  return b;
}
static void give_radio(MultiTap &bus, uint16_t id, const mp::RadioBlock &b) {
  bus.at(id)->has_cfg = true;
  bus.at(id)->cfg = bus.at(id)->cfg_stored = b;
}
// No made-up key's first four bytes, spaced or not, in any log line.
static bool no_key_in(const std::vector<std::string> &lines) {
  for (const auto &l : lines)
    for (const char *k : {"30313233", "30 31 32 33", "40414243", "40 41 42 43", "50515253", "50 51 52 53",
                          "99999999", "99 99 99 99"})
      if (l.find(k) != std::string::npos) return false;
  return true;
}
static mp::Bytes tap_mac(uint8_t which) { return {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x00, which}; }
static mp::RadioBlock rekey_block() {
  mp::RadioBlock b;  // what a re-key request carries: placeholders, the key filled in by the master
  b.channel = 11;
  b.pan = 1;
  mp::map_for_profile(12, b.map);
  b.keep_network = b.keep_tap_profile = b.shared_key = true;
  for (auto &k : b.key) k = 0x99;
  return b;
}

// Each TAP's settings are read at start when it may send known networks,
// and each TAP says whether its key is the one the others share. 1203 is a
// 46-byte TAP, read the same way.
static void test_radio_reads_give_key_words() {
  {
    MultiTap bus;
    bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}};
    give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
    give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
    give_radio(bus, 0x1203, radio(11, 0x1A2B, 0x50, /*wide=*/true));
    std::vector<std::string> lines;
    MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                    true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    MasterStatus m = core.status();
    CHECK(m.taps.size() == 3);
    CHECK(m.taps[0].key == "same" && m.taps[1].key == "same" && m.taps[2].key == "different");
    CHECK(bus.at(0x1203)->cfg_reads >= 1);
    CHECK(no_key_in(lines));
    core.close();
  }
  {
    MultiTap bus;
    bus.taps = {{0x1201, 1}, {0x1202, 2}};
    give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
    give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}});
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    CHECK(bus.at(0x1201)->cfg_reads == 0 && bus.at(0x1202)->cfg_reads == 0);
    CHECK(core.status().taps[0].key == "unread" && core.status().taps[1].key == "unread");
    core.close();
  }
}

// A provision asking for the shared key gets the one the others have, and
// a re-key keeps the TAP's own channel and PAN.
static void test_provision_takes_the_shared_key() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}};
  give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
  give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
  give_radio(bus, 0x1203, radio(11, 0x1A2B, 0x50));
  std::vector<std::string> lines;
  MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                  true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  std::string why;
  CHECK(core.request_provision(tap_mac(3), rekey_block(), false, false, why));
  CHECK(why.empty());
  run_for(core, 20000, 60000, 50);
  const auto *t = bus.at(0x1203);
  CHECK(t->cfg.key[0] == 0x30 && t->cfg.key[15] == 0x3F);
  CHECK(t->cfg_stored.key[0] == 0x30);
  CHECK(t->cfg.channel == 11 && t->cfg.pan == 0x1A2B);
  CHECK(core.status().taps[2].key == "same");
  CHECK(no_key_in(lines));
  core.close();
}

// Refused when the others disagree, or when one that answers hasn't been
// read; alone, the request's own key stands.
static void test_provision_shared_key_refusals() {
  {
    MultiTap bus;
    bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}};
    give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
    give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x40));
    give_radio(bus, 0x1203, radio(11, 0x1A2B, 0x50));
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    std::string why;
    CHECK(!core.request_provision(tap_mac(3), rekey_block(), false, false, why));
    CHECK(why == "your other TAPs don't share a key");
    core.close();
  }
  {
    MultiTap bus;
    bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}};
    give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
    give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
    give_radio(bus, 0x1203, radio(11, 0x1A2B, 0x50));
    bus.at(0x1202)->cfg_silent = true;  // answers polls, never its settings
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    CHECK(core.status().taps[1].state == "running");
    std::string why;
    CHECK(!core.request_provision(tap_mac(3), rekey_block(), false, false, why));
    CHECK(why.find("1202") != std::string::npos && why.find("haven't been read") != std::string::npos);
    core.close();
  }
  {
    MultiTap bus;
    bus.taps = {{0x1201, 1}};
    give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    CHECK(core.status().taps[0].key == "alone");
    std::string no;
    CHECK(!core.request_provision(tap_mac(1), rekey_block(), false, false, no));  // a re-key with no one to share with
    CHECK(no == "there's no other TAP to share a key with");
    mp::RadioBlock b = rekey_block();
    b.keep_network = false;  // Add a TAP's: a new network, its own key
    b.channel = 20;
    b.pan = 0x4C3D;
    std::string why;
    CHECK(core.request_provision(tap_mac(1), b, false, false, why));
    run_for(core, 20000, 60000, 50);
    CHECK(bus.at(0x1201)->cfg.key[0] == 0x99 && bus.at(0x1201)->cfg.channel == 20);
    core.close();
  }
}

// ---- 0xE1: each TAP told the others' networks ----
static void three_on_one_key(MultiTap &bus) {
  bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}};
  give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
  give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
  give_radio(bus, 0x1203, radio(11, 0x1A2B, 0x50));
}
// The channels an 0xE1's records carry, in order.
static std::vector<int> e1_channels(const mp::Bytes &args) {
  std::vector<int> ch;
  for (size_t i = 0; args.size() >= 4 && i < args[0] && 4 + 20 * i < args.size(); i++) ch.push_back(args[4 + 20 * i]);
  return ch;
}

// At start, once every TAP has been read: one each, naming the other two in
// the master's order, and not again without a reason.
static void test_known_networks_at_start() {
  MultiTap bus;
  three_on_one_key(bus);
  std::vector<std::string> lines;
  MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                  true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
  if (bus.at(0x1201)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1201)->e1[0]) == std::vector<int>{18, 11}));
  if (bus.at(0x1202)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1202)->e1[0]) == std::vector<int>{15, 11}));
  if (bus.at(0x1203)->e1.size() == 1) {
    CHECK((e1_channels(bus.at(0x1203)->e1[0]) == std::vector<int>{15, 18}));
    CHECK(bus.at(0x1203)->e1[0].size() == 44);
  }
  for (const auto &t : core.status().taps) CHECK(t.known_networks == "accepted (2 other TAPs)");
  run_for(core, 20000, 140000, 50);
  CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
  CHECK(no_key_in(lines));
  core.close();
}

// Without Allow changes to the TAPs nothing goes out, and each TAP says why;
// turned on, they go.
static void test_known_networks_waits_for_writes() {
  MultiTap bus;
  three_on_one_key(bus);
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(bus.at(0x1201)->e1.empty() && bus.at(0x1202)->e1.empty() && bus.at(0x1203)->e1.empty());
  for (const auto &t : core.status().taps) CHECK(t.known_networks == "waiting: Allow changes to the TAPs is off");
  core.set_table_writes(true);
  run_for(core, 20000, 30000, 50);
  CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
  core.close();
}

// A TAP's network changed (a provision): every TAP is told again.
static void test_known_networks_after_provision() {
  MultiTap bus;
  three_on_one_key(bus);
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  std::string why;
  CHECK(core.request_provision(tap_mac(3), rekey_block(), false, false, why));
  run_for(core, 20000, 60000, 50);
  CHECK(core.status().table_edit.ok);
  CHECK(bus.at(0x1201)->e1.size() == 2 && bus.at(0x1202)->e1.size() == 2 && bus.at(0x1203)->e1.size() == 2);
  core.close();
}

// While a table change runs, no 0xE1 goes out; after it, they do. With the
// writes off, every TAP is read and waiting; a check-only add starts and
// the writes go on at the same moment, so each 0xE1 is due while it runs.
static void test_known_networks_waits_for_a_table_job() {
  MultiTap bus;
  three_on_one_key(bus);
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(core.status().taps[2].known_networks == "waiting: Allow changes to the TAPs is off");
  std::string why;
  CHECK(core.request_table_add(NEW_MAC, 0, /*dry_run=*/true, why, 0x1201));
  core.set_table_writes(true);
  bool ran = false, sent_during = false;
  for (uint32_t t = 20000; t < 40000; t += 50) {
    core.tick(t);
    if (core.status().table_edit.active) {
      ran = true;
      for (uint16_t id : {0x1201, 0x1202, 0x1203}) sent_during = sent_during || !bus.at(id)->e1.empty();
    }
  }
  CHECK(ran);
  CHECK(!sent_during);
  CHECK(core.status().table_edit.finished);
  CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
  core.close();
}

// A lost TAP that was read is still named; it isn't sent one while lost,
// and is when it's back. One lost before it was ever read is named nowhere.
static void test_known_networks_lost_and_unread() {
  {
    MultiTap bus;
    three_on_one_key(bus);
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    bus.at(0x1202)->up = false;
    run_for(core, 20000, 30000, 50);
    CHECK(core.status().taps[1].state == "lost");
    std::string why;
    CHECK(core.request_provision(tap_mac(3), rekey_block(), false, false, why));  // a change: all due again
    run_for(core, 30000, 70000, 50);
    CHECK(bus.at(0x1201)->e1.size() == 2 && bus.at(0x1203)->e1.size() == 2 && bus.at(0x1202)->e1.size() == 1);
    if (bus.at(0x1201)->e1.size() == 2) CHECK((e1_channels(bus.at(0x1201)->e1[1]) == std::vector<int>{18, 11}));
    bus.at(0x1202)->up = true;
    run_for(core, 70000, 110000, 50);
    CHECK(core.status().taps[1].state == "running");
    CHECK(bus.at(0x1202)->e1.size() == 2 && bus.at(0x1201)->e1.size() == 2 && bus.at(0x1203)->e1.size() == 2);
    core.close();
  }
  {
    // Lost and back with nothing else happening (a power cut may have
    // cleared its list): it alone is told again.
    MultiTap bus;
    three_on_one_key(bus);
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    bus.at(0x1202)->up = false;
    run_for(core, 20000, 30000, 50);
    CHECK(core.status().taps[1].state == "lost");
    bus.at(0x1202)->up = true;
    run_for(core, 30000, 70000, 50);
    CHECK(core.status().taps[1].state == "running");
    CHECK(bus.at(0x1202)->e1.size() == 2 && bus.at(0x1201)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
    core.close();
  }
  {
    MultiTap bus;
    three_on_one_key(bus);
    bus.at(0x1203)->up = false;  // gone before its first read
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 30000, 50);
    CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.empty());
    if (bus.at(0x1201)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1201)->e1[0]) == std::vector<int>{18}));
    if (bus.at(0x1202)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1202)->e1[0]) == std::vector<int>{15}));
    core.close();
  }
}

// A refusal is said, by name, once; not retried in a loop.
static void test_known_networks_refusal_is_said_once() {
  MultiTap bus;
  three_on_one_key(bus);
  bus.at(0x1203)->e1_status = 3;
  std::vector<std::string> lines;
  MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                  true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 140000, 50);
  CHECK(bus.at(0x1203)->e1.size() == 1);
  CHECK(core.status().taps[2].known_networks == "refused: no room for another network");
  int said = 0;
  for (const auto &l : lines) said += l.find("no room for another network") != std::string::npos;
  CHECK(said == 1);
  CHECK(no_key_in(lines));
  core.close();
}

// A TAP still on its factory settings (channel 0: a new one, not yet given
// its own) is no network to join: named in nobody's 0xE1, and nothing the
// gate would refuse goes out.
static void test_known_networks_skip_a_tap_on_factory_settings() {
  MultiTap bus;
  three_on_one_key(bus);
  mp::RadioBlock factory = radio(0, 0x1234, 0x00);
  for (auto &k : factory.key) k = 0;
  give_radio(bus, 0x1203, factory);
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(core.status().fault != Fault::refused);
  CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1);
  if (bus.at(0x1201)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1201)->e1[0]) == std::vector<int>{18}));
  if (bus.at(0x1202)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1202)->e1[0]) == std::vector<int>{15}));
  // The factory TAP itself is told the two real networks.
  CHECK(bus.at(0x1203)->e1.size() == 1);
  core.close();
}

// One TAP: nothing to share and no one to tell, so no settings read is
// added to its start; it says "alone" all the same.
static void test_one_tap_is_alone_without_a_read() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}};
  give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(bus.at(0x1201)->cfg_reads == 0);
  CHECK(core.status().taps[0].key == "alone");
  CHECK(bus.at(0x1201)->e1.empty());
  core.close();
}

// ---- review fixes (2026-11-22) ----
static mp::RadioBlock factory_radio() {
  mp::RadioBlock b = radio(0, 0x1234, 0x00);
  for (auto &k : b.key) k = 0;
  return b;
}

// C1: a TAP on its factory settings holds no network: its zero key counts
// for nothing. One real TAP beside it is "alone" and can't be re-keyed
// (to zeros); the factory one says so and can't be re-keyed either; Add a
// TAP's provision for it takes the real TAP's key.
static void test_a_factory_tap_holds_no_key() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
  give_radio(bus, 0x1202, factory_radio());
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(core.status().taps[0].key == "alone");
  CHECK(core.status().taps[1].key == "factory");
  std::string why;
  CHECK(!core.request_provision(tap_mac(1), rekey_block(), false, false, why));
  CHECK(why == "there's no other TAP to share a key with");
  mp::RadioBlock add = rekey_block();
  add.keep_network = false;
  add.channel = 20;
  add.pan = 0x4C3D;
  CHECK(core.request_provision(tap_mac(2), add, false, false, why));
  run_for(core, 20000, 60000, 50);
  CHECK(bus.at(0x1202)->cfg.key[0] == 0x30 && bus.at(0x1202)->cfg.channel == 20);
  CHECK(bus.at(0x1201)->cfg.key[0] == 0x30);  // untouched
  CHECK(core.status().taps[0].key == "same" && core.status().taps[1].key == "same");
  core.close();
}

// I5: two real TAPs on one key and two new ones on factory settings: the
// real ones read "same", and adding a new one takes their key.
static void test_two_new_taps_on_factory_settings() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1203, 3}, {0x1204, 4}};
  give_radio(bus, 0x1201, radio(15, 0x7E21, 0x30));
  give_radio(bus, 0x1202, radio(18, 0x5A5A, 0x30));
  give_radio(bus, 0x1203, factory_radio());
  give_radio(bus, 0x1204, factory_radio());
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}, {0x1204, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 30000, 50);
  MasterStatus m = core.status();
  CHECK(m.taps[0].key == "same" && m.taps[1].key == "same" && m.taps[2].key == "factory" && m.taps[3].key == "factory");
  mp::RadioBlock add = rekey_block();
  add.keep_network = false;
  add.channel = 20;
  add.pan = 0x4C3D;
  std::string why;
  CHECK(core.request_provision(tap_mac(3), add, false, false, why));
  CHECK(why.empty());
  run_for(core, 30000, 70000, 50);
  CHECK(bus.at(0x1203)->cfg.key[0] == 0x30);
  core.close();
}

// I1: a settings read that goes unanswered is tried again (once a minute),
// and the 0xE1s follow once it's read. One that never answers is given up
// on after a few tries, said once, and left out, so the others are told.
static void test_a_missed_settings_read_is_tried_again() {
  {
    MultiTap bus;
    three_on_one_key(bus);
    bus.at(0x1202)->cfg_silent_reads = 1;
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    CHECK(core.status().taps[1].key == "unread");
    run_for(core, 20000, 100000, 50);
    CHECK(bus.at(0x1202)->cfg_reads >= 2);
    CHECK(core.status().taps[1].key == "same");
    CHECK(bus.at(0x1201)->e1.size() == 1 && bus.at(0x1202)->e1.size() == 1 && bus.at(0x1203)->e1.size() == 1);
    if (bus.at(0x1201)->e1.size() == 1) CHECK((e1_channels(bus.at(0x1201)->e1[0]) == std::vector<int>{18, 11}));
    core.close();
  }
  {
    MultiTap bus;
    three_on_one_key(bus);
    bus.at(0x1202)->cfg_silent_reads = 1000;
    std::vector<std::string> lines;
    MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                    true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 400000, 100);
    int said = 0;
    for (const auto &l : lines) said += l.find("1202's radio settings couldn't be read") != std::string::npos;
    CHECK(said == 1);
    CHECK(bus.at(0x1202)->cfg_reads <= 6);
    CHECK(!bus.at(0x1201)->e1.empty() && !bus.at(0x1203)->e1.empty());
    if (!bus.at(0x1201)->e1.empty()) CHECK((e1_channels(bus.at(0x1201)->e1.back()) == std::vector<int>{11}));
    core.close();
  }
}

// I2: a TAP read for the first time after start (it was down) is named to
// the others too, not only told about them.
static void test_a_tap_read_late_is_named_to_the_others() {
  MultiTap bus;
  three_on_one_key(bus);
  bus.at(0x1203)->up = false;
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 30000, 50);
  bus.at(0x1203)->up = true;
  run_for(core, 30000, 100000, 50);
  CHECK(core.status().taps[2].key != "unread");
  CHECK(!bus.at(0x1201)->e1.empty() && !bus.at(0x1202)->e1.empty());
  if (!bus.at(0x1201)->e1.empty()) CHECK((e1_channels(bus.at(0x1201)->e1.back()) == std::vector<int>{18, 11}));
  if (!bus.at(0x1202)->e1.empty()) CHECK((e1_channels(bus.at(0x1202)->e1.back()) == std::vector<int>{15, 11}));
  core.close();
}

// I3: an 0xE1 with no answer (a frame lost on the wire) is tried again, a
// few times; one never answered says so.
static void test_an_unanswered_e1_is_tried_again() {
  {
    MultiTap bus;
    three_on_one_key(bus);
    bus.at(0x1203)->e1_silent = 1;
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 120000, 50);
    CHECK(bus.at(0x1203)->e1.size() == 2);
    CHECK(core.status().taps[2].known_networks == "accepted (2 other TAPs)");
    core.close();
  }
  {
    MultiTap bus;
    three_on_one_key(bus);
    bus.at(0x1203)->e1_silent = -1;
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
    core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
    core.set_known_networks(true);
    core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 400000, 100);
    CHECK(bus.at(0x1203)->e1.size() == 3);
    CHECK(core.status().taps[2].known_networks == "no answer");
    core.close();
  }
}

// I6: why nothing is sent is logged, once, not only put in the status.
static void test_waiting_for_writes_is_logged_once() {
  MultiTap bus;
  three_on_one_key(bus);
  std::vector<std::string> lines;
  MasterCore core(&bus.port, [&](int, const std::string &l) { lines.push_back(l); }, nullptr, nullptr, 0x1201, false,
                  true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  CHECK(core.open());
  run_for(core, 0, 60000, 50);
  int said = 0;
  for (const auto &l : lines) said += l.find("Allow changes to the TAPs is off") != std::string::npos;
  CHECK(said == 1);
  core.close();
}

// I4: the settings a provision reads on another thread are written under
// the lock (run under -fsanitize=thread to see a race; here it only runs).
static void test_provision_reads_settings_across_threads() {
  MultiTap bus;
  three_on_one_key(bus);
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}, {0x1203, {}}});
  core.set_known_networks(true);
  CHECK(core.open());
  std::atomic<bool> stop{false};
  std::thread asker([&] {
    while (!stop) {
      std::string why;
      core.request_provision(tap_mac(3), rekey_block(), false, /*dry_run=*/true, why);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  run_for(core, 0, 20000, 50);
  stop = true;
  asker.join();
  core.close();
}

// Retiring a TAP that has gone for good: refused while it answers, for the
// only TAP, and for one the master doesn't have; a lost one is dropped at the
// next tick, no longer polled, and the shorter list is saved.
static void test_a_lost_tap_can_be_retired() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  std::vector<std::vector<MasterCore::TapEntry>> saves;
  core.set_adopt_taps(false, [&](const std::vector<MasterCore::TapEntry> &t) { saves.push_back(t); });
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 2000);
  std::string why;
  CHECK(!core.request_forget_tap(0x1202, why) && why.find("answering") != std::string::npos);
  CHECK(!core.request_forget_tap(0x1209, why) && why.find("no TAP") != std::string::npos);
  bus.at(0x1202)->up = false;
  run_for(core, 2000, 4000);
  CHECK(core.status().taps[1].state == "lost");
  CHECK(core.request_forget_tap(0x1202, why));
  core.tick(4001);
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 1 && m.taps[0].id == 0x1201);
  CHECK(saves.size() == 1 && saves[0].size() == 1 && saves[0][0].id == 0x1201);
  int before = bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F);
  run_for(core, 4001, 4001 + 2 * MasterCore::LOST_PROBE_MS, 50);
  CHECK(bus.sent_to(0x1202, 0x0148) + bus.sent_to(0x1202, 0x0B0F) == before);
  CHECK(m.fault == Fault::none);
  // Now the only TAP: refused.
  CHECK(!core.request_forget_tap(0x1201, why) && why.find("only TAP") != std::string::npos);
  core.close();
}

// Both quiet: the master's fault says no TAP answers, as with one.
static void test_no_tap_answering_is_a_fault() {
  MultiTap bus;
  bus.taps = {{0x1201, 1, false}, {0x1202, 2, false}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 3000);
  CHECK(core.status().fault == Fault::no_reply);
  core.close();
}

// A table add runs on the TAP it names, and the id it picks skips those
// listed on the other TAP: optimizer ids are system-wide.
static void test_a_table_add_runs_on_its_tap() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  bus.at(0x1201)->records = {rec(1, 0x0002), rec(2, 0x0003)};
  bus.at(0x1202)->records = {rec(3, 0x0004)};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  core.set_table_writes(true);
  CHECK(core.open());
  run_for(core, 0, 1000);
  std::string why;
  CHECK(core.request_table_add(NEW_MAC, 0, false, why, 0x1202, {0x0002, 0x0003}));
  run_for(core, 1000, 8000);
  CHECK(bus.at(0x1201)->writes.empty());
  CHECK(bus.at(0x1202)->writes.size() == 1);
  // 0002 and 0003 are on 1201, 0004 on 1202: the lowest free is 0005.
  CHECK(core.status().table_edit.ok && core.status().table_edit.id == 0x0005);
  CHECK(core.status().table_tap == 0x1202);
  CHECK(core.status().table_waiting.size() == 1);
  // An id listed on the other TAP is refused if asked for.
  CHECK(core.request_table_add({0x04, 0xC0, 0x5B, 0, 0, 0, 0x12, 0x35}, 0x0003, false, why, 0x1202, {0x0002, 0x0003}));
  run_for(core, 8000, 14000);
  CHECK(!core.status().table_edit.ok && core.status().table_edit.message.find("another TAP") != std::string::npos);
  // A TAP that isn't on the bus is refused.
  CHECK(!core.request_table_add(NEW_MAC, 0, true, why, 0x1209));
  core.close();
}

// Startup with two TAPs, both at their ids: each is read there, then the
// scan at the factory address, then both are polled. Nothing is re-addressed.
static void test_startup_finds_two_taps_at_their_ids() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 10000);
  CHECK(bus.sent_to(0x1201, 0x003A) == 1 && bus.sent_to(0x1202, 0x003A) == 1);
  CHECK(bus.sent_to(0x1235, 0x0038) == 5);
  CHECK(!readdressed(bus.port));
  CHECK(bus.sent_to(0x1201, 0x0148) > 20 && bus.sent_to(0x1202, 0x0148) > 20);
  auto taps = core.taps();
  CHECK(taps.size() == 2 && taps[1].mac.size() == 8 && taps[1].mac[7] == 2);
  core.close();
}

// Startup with one of two TAPs missing: the other is polled, the missing
// one is lost, asked again now and then.
static void test_startup_without_one_tap_polls_the_other() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2, false}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 10000);
  CHECK(bus.sent_to(0x1201, 0x0148) > 20);
  CHECK(core.status().taps[1].state == "lost" && core.status().phase == Phase::running);
  CHECK(!readdressed(bus.port));
  core.close();
}

// PV off reaches every TAP's switch, and is confirmed only once each answered.
static void test_pv_off_goes_to_every_tap() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 1000);
  CHECK(core.request_pv(true));
  run_for(core, 1000, 3000);
  CHECK(bus.sent_to(0x1201, 0x0B00) >= 1 && bus.sent_to(0x1202, 0x0B00) >= 1);
  core.close();
}

static mp::Bytes multi_mac(uint8_t last) { return {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x00, last}; }

// Adopting on: a new TAP at the factory address starts the herd. The known
// TAP keeps 1201, the new one gets 1202 and, keeping no id through the
// RESET, a STORE in the second round; both are saved and polled.
static void test_adopting_herds_a_new_tap() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1235, 2}};
  bus.at(0x1235)->stored = 0;
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}});
  std::vector<MasterCore::TapEntry> saved;
  core.set_adopt_taps(true, [&](const std::vector<MasterCore::TapEntry> &t) { saved = t; });
  CHECK(core.open());
  run_for(core, 0, 60000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 10);  // two rounds of five
  CHECK(bus.sent_to(0x1202, 0x0012) == 1);   // the STORE, to the new one only
  CHECK(bus.sent_to(0x1201, 0x0012) == 0);
  CHECK(saved.size() == 2 && saved[0].id == 0x1201 && saved[1].id == 0x1202 && saved[1].mac == multi_mac(2));
  CHECK(bus.taps[0].id == 0x1201 && bus.taps[1].id == 0x1202 && bus.taps[1].stored == 0x1202);
  int a = bus.sent_to(0x1201, 0x0148), b = bus.sent_to(0x1202, 0x0148);
  CHECK(a > 20 && b > 20);
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.fault == Fault::none && m.phase == Phase::running);
  core.close();
}

// Adopting off, as before: the new TAP is reported and nothing is herded.
static void test_not_adopting_leaves_a_new_tap_alone() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1235, 2}};
  bus.at(0x1235)->stored = 0;
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 0);
  CHECK(core.status().fault == Fault::extra_tap && core.status().extra_taps.size() == 1);
  CHECK(bus.sent_to(0x1201, 0x0148) > 20);
  std::string why;
  CHECK(!core.request_herd(why) && why.find("master_adopt_taps") != std::string::npos);
  core.close();
}

// The owner's herd, two TAPs already known: both keep their ids, one round.
static void test_the_owners_herd_keeps_known_ids() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  std::vector<MasterCore::TapEntry> saved;
  core.set_adopt_taps(true, [&](const std::vector<MasterCore::TapEntry> &t) { saved = t; });
  CHECK(core.open());
  run_for(core, 0, 10000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 0);  // both found at their ids: no herd
  std::string why;
  CHECK(core.request_herd(why));
  run_for(core, 10000, 40000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 5 && bus.sent_to(0x0000, 0x0010) == 1);
  CHECK(saved.size() == 2 && saved[0].id == 0x1201 && saved[1].id == 0x1202);
  CHECK(bus.taps[0].id == 0x1201 && bus.taps[1].id == 0x1202);
  CHECK(core.status().phase == Phase::running);
  core.close();
}

// After the herd's reset broadcast the tunnel sequence starts again at 01,
// as the CCA's does. (The TAP only echoes it: 0x0E's status byte turned out
// to be the join window's state, bench 2026-11-11.)
static void test_the_tunnel_seq_starts_at_01_after_a_reset() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}});
  core.set_adopt_taps(true, [](const std::vector<MasterCore::TapEntry> &) {});
  CHECK(core.open());
  run_for(core, 0, 60000, 50);  // running, its seq well past 01
  std::string why;
  CHECK(core.request_herd(why));
  run_for(core, 60000, 100000, 50);
  size_t reset = 0;
  for (size_t i = 0; i < bus.port.frames.size(); i++)
    if (bus.port.frames[i].dest == 0x0000 && bus.port.frames[i].cmd == 0x0010) reset = i;
  CHECK(reset > 0);
  int first = -1;
  for (size_t i = reset + 1; i < bus.port.frames.size() && first < 0; i++)
    if (bus.port.frames[i].cmd == mp::CMD_TUNNEL && bus.port.frames[i].payload.size() > 4) first = bus.port.frames[i].payload[4];
  CHECK(first == 0x01);
  core.close();
}

// Giving a TAP its radio settings: on the bench, or on a live system with
// "Allow changes to the TAPs" (master_table_writes) on, as Add a TAP's step
// does; refused with neither.
static void test_radio_settings_need_the_bench_or_table_writes() {
  mp::RadioBlock b;
  b.channel = 21;
  b.pan = 0x4C3D;
  mp::map_for_profile(12, b.map);
  const mp::Bytes mac = multi_mac(1);
  for (int mode = 0; mode < 3; mode++) {
    MultiTap bus;
    bus.taps = {{0x1201, 1}};
    MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, /*bench=*/mode == 1, true);
    core.set_taps({{0x1201, mac}});
    if (mode == 2) core.set_table_writes(true);
    CHECK(core.open());
    run_for(core, 0, 20000, 50);
    std::string why;
    // A check (a dry run) only reads: allowed either way, as other checks are,
    // and it finishes; it doesn't hang refused, blocking every table change.
    CHECK(core.request_provision(mac, b, false, true, why));
    run_for(core, 20000, 30000, 50);
    CHECK(core.status().table_edit.finished && !core.status().table_edit.active);
    CHECK(core.status().fault != Fault::refused);
    bool ok = core.request_provision(mac, b, false, false, why);
    if (mode == 0) CHECK(!ok && why.find("Allow changes to the TAPs") != std::string::npos);
    else CHECK(ok);
    // Skipping the MAC check, or a reset, only on the bench: on a live
    // system a wrong MAC could otherwise re-network the TAP in service.
    run_for(core, 30000, 40000, 50);
    mp::RadioBlock skip = b;
    skip.skip_identify = true;
    CHECK(core.request_provision(mac, skip, false, false, why) == (mode == 1));
    run_for(core, 40000, 50000, 50);
    CHECK(core.request_provision(mac, b, /*reset=*/true, false, why) == (mode == 1));
    run_for(core, 50000, 60000, 50);
    // The gateway Store, too: the bench only.
    mp::RadioBlock store = b;
    store.store = true;
    CHECK(core.request_provision(mac, store, false, false, why) == (mode == 1));
    run_for(core, 60000, 70000, 50);
    core.close();
  }
}

// A master replacing another (a settings save) carries on its tunnel count,
// so the TAP sees no break.
static void test_the_tunnel_seq_carries_over() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}});
  core.set_seq(0x40);
  CHECK(core.open());
  run_for(core, 0, 60000, 50);
  int first = -1;
  for (const auto &f : bus.port.frames)
    if (f.cmd == mp::CMD_TUNNEL && f.payload.size() > 4) { first = f.payload[4]; break; }
  CHECK(first == 0x41);
  CHECK(core.seq() > 0x41);
  core.close();
}

// A herd that hears a saved TAP missing keeps it, lost, at its id.
static void test_a_herd_keeps_a_missing_tap() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2, false}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  std::vector<MasterCore::TapEntry> saved;
  core.set_adopt_taps(true, [&](const std::vector<MasterCore::TapEntry> &t) { saved = t; });
  CHECK(core.open());
  run_for(core, 0, 10000, 50);
  std::string why;
  CHECK(core.request_herd(why));
  run_for(core, 10000, 40000, 50);
  CHECK(saved.size() == 2 && saved[1].id == 0x1202 && saved[1].mac == multi_mac(2));
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.taps[0].state == "running" && m.taps[1].state == "lost");
  core.close();
}

// A lost TAP back at the factory address (a power cut, and it keeps no id):
// in startup mode its tries also ask there, and finding it, the master
// herds, as it re-addresses one of several TAPs (see the next test). Both
// end at their ids.
// Both TAPs miss together (the bus for a second, on the roof 2026-11-17):
// the master looks for them again, finds one at its id and the other later.
// Each shows as running again; before, the one found at its id was left
// "lost" while it was polled.
static void test_both_quiet_at_once_then_back_each_shows_running() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  CHECK(core.open());
  run_for(core, 0, 8000, 50);
  CHECK(core.status().taps[0].state == "running" && core.status().taps[1].state == "running");
  bus.at(0x1201)->up = false;
  bus.at(0x1202)->up = false;
  // As on the roof: 1202 is back by the time the master looks again, 1201
  // a little later, so the look finds 1202 at its id and 1201 lost again.
  uint32_t t = 8000;
  for (; t < 12000 && core.status().fault != Fault::no_reply; t += 50) core.tick(t);
  CHECK(core.status().fault == Fault::no_reply);
  bus.at(0x1202)->up = true;
  run_for(core, t, t + MasterCore::LOST_PROBE_MS / 2, 50);
  bus.at(0x1201)->up = true;
  run_for(core, t + MasterCore::LOST_PROBE_MS / 2, t + 2 * MasterCore::LOST_PROBE_MS, 50);
  t += 2 * MasterCore::LOST_PROBE_MS;
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.taps[0].state == "running" && m.taps[1].state == "running");
  CHECK(m.fault == Fault::none);
  int a = bus.sent_to(0x1201, 0x0148), b = bus.sent_to(0x1202, 0x0148);
  run_for(core, t, t + 2000, 50);
  CHECK(bus.sent_to(0x1201, 0x0148) - a > 10 && bus.sent_to(0x1202, 0x0148) - b > 10);
  core.close();
}

static void test_a_lost_tap_at_the_factory_address_is_brought_back() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  CHECK(core.open());
  run_for(core, 0, 8000, 50);
  CHECK(core.status().taps.size() == 2 && core.status().taps[1].state == "running");
  bus.at(0x1202)->id = 0x1235;  // back from a power cut at the factory address
  uint32_t t = 8000;
  for (uint32_t end = t + 3 * MasterCore::LOST_PROBE_MS; t < end; t += 50) core.tick(t);
  CHECK(bus.taps[1].id == 0x1202);
  CHECK(core.status().taps[1].state == "running");
  CHECK(bus.sent_to(0x0000, 0x0014) == 5 && bus.taps[0].id == 0x1201);  // one round of the herd
  int before = bus.sent_to(0x1202, 0x0148);
  for (uint32_t end = t + 2000; t < end; t += 50) core.tick(t);
  CHECK(bus.sent_to(0x1202, 0x0148) - before > 10);
  core.close();
}

// Startup, two known TAPs, one at the factory address: a herd, not the
// one-TAP bring-up, whose SET_485_ID moves both there and whose GET_MAC,
// hearing both, could give the first to answer the other's id.
static void test_startup_with_one_of_two_at_the_factory_address_herds() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1235, 2}};
  bus.at(0x1235)->stored = 0x1202;
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  CHECK(core.open());
  run_for(core, 0, 40000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 5);
  CHECK(bus.taps[0].id == 0x1201 && bus.taps[1].id == 0x1202);
  MasterStatus m = core.status();
  CHECK(m.taps.size() == 2 && m.taps[0].state == "running" && m.taps[1].state == "running");
  CHECK(bus.sent_to(0x1201, 0x0148) > 20 && bus.sent_to(0x1202, 0x0148) > 20);
  core.close();
}

// Not adopting, a stranger at the factory address is no reason to herd:
// a lost TAP's tries see it there, and leave it; the other TAP is polled
// throughout, and the stranger is reported.
static void test_a_stranger_at_the_factory_address_starts_no_herd() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2}, {0x1235, 9}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  CHECK(core.open());
  run_for(core, 0, 10000, 50);
  bus.at(0x1202)->up = false;  // lost; the stranger stays at the factory address
  uint32_t t = 10000;
  for (uint32_t end = t + 4 * MasterCore::LOST_PROBE_MS; t < end; t += 50) core.tick(t);
  CHECK(bus.sent_to(0x0000, 0x0014) == 0);
  CHECK(core.status().taps[1].state == "lost" && core.status().fault == Fault::extra_tap);
  int before = bus.sent_to(0x1201, 0x0148);
  for (uint32_t end = t + 2000; t < end; t += 50) core.tick(t);
  CHECK(bus.sent_to(0x1201, 0x0148) - before > 10);
  core.close();
}

// Startup, two known TAPs, the second missing and a stranger at the factory
// address, not adopting: the stranger is reported, the first is polled.
static void test_startup_with_a_stranger_polls_ours() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2, false}, {0x1235, 9}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, /*startup=*/true);
  core.set_taps({{0x1201, multi_mac(1)}, {0x1202, multi_mac(2)}});
  CHECK(core.open());
  run_for(core, 0, 20000, 50);
  CHECK(bus.sent_to(0x0000, 0x0014) == 0);
  CHECK(core.status().phase == Phase::running && core.status().taps[1].state == "lost");
  CHECK(bus.sent_to(0x1201, 0x0148) > 20);
  core.close();
}

// A change aimed at a TAP that isn't answering is refused, not left waiting.
static void test_a_table_change_on_a_lost_tap_is_refused() {
  MultiTap bus;
  bus.taps = {{0x1201, 1}, {0x1202, 2, false}};
  MasterCore core(&bus.port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, {}}, {0x1202, {}}});
  CHECK(core.open());
  run_for(core, 0, 3000);
  CHECK(core.status().taps[1].state == "lost");
  std::string why;
  CHECK(!core.request_table_add(NEW_MAC, 0, true, why, 0x1202));
  CHECK(why.find("isn't answering") != std::string::npos);
  CHECK(core.request_table_add(NEW_MAC, 0, true, why, 0x1201));
  core.close();
}

// The bus as a standby hears it: another master polling some TAPs, until
// `quiet` stops it; then the TAPs (MultiTap) answer whatever this one sends.
class OtherMasterThenTaps : public Transport {
 public:
  explicit OtherMasterThenTaps(MultiTap &bus) : bus_(bus) {}
  std::vector<uint16_t> polled;  // the other master's poll targets, in turn
  bool quiet = false;
  bool configured() const override { return true; }
  bool open(std::string &e) override { return bus_.port.open(e); }
  void close() override { bus_.port.close(); }
  bool is_open() const override { return bus_.port.is_open(); }
  bool write(const uint8_t *d, size_t n, std::string &e) override {
    writes++;
    return bus_.port.write(d, n, e);
  }
  void drain() override {}
  int read(char *buf, size_t n, int t) override {
    if (quiet || polled.empty()) return bus_.port.read(buf, n, t);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto f = mp::encode_frame(polled[next_++ % polled.size()], 0x0148, {0x00, 0x00, 0x00, 0x00, 0x00});
    size_t k = std::min(n, f.size());
    std::copy(f.begin(), f.begin() + k, buf);
    return static_cast<int>(k);
  }
  int baud() const override { return 38400; }
  std::string name() const override { return "fake"; }
  int writes = 0;

 private:
  MultiTap &bus_;
  size_t next_ = 0;
};

static void tick_for_real(MasterCore &core, int ms, uint32_t &t) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    core.tick(t += 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

static std::vector<uint16_t> tap_ids(const MasterCore &core) {
  std::vector<uint16_t> ids;
  for (const auto &t : core.taps()) ids.push_back(t.id);
  return ids;
}

// A standby that starts from a guess (gateway_id, nothing saved) learns the
// TAPs the other master polls, drops the guess, saves them, and on taking
// over runs them, and not the guess.
static void test_a_standby_learns_the_taps_another_master_polls() {
  MultiTap bus;
  bus.taps = {{0x1203, 3}, {0x1204, 4}};
  OtherMasterThenTaps port(bus);
  port.polled = {0x1203, 0x1204};
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_standby(1, nullptr, nullptr);
  std::vector<MasterCore::TapEntry> saved;
  int saves = 0;
  core.set_learn_taps(
      [&](const std::vector<MasterCore::TapEntry> &t) {
        saved = t;
        saves++;
      },
      /*guessed=*/true);
  CHECK(core.open());
  uint32_t t = 0;
  tick_for_real(core, 500, t);
  CHECK(port.writes == 0);
  CHECK((tap_ids(core) == std::vector<uint16_t>{0x1203, 0x1204}));
  CHECK(saves == 2 && saved.size() == 2 && saved[0].id == 0x1203 && saved[1].id == 0x1204);
  port.quiet = true;
  tick_for_real(core, 2500, t);
  CHECK(core.status().standby == Standby::active);
  CHECK(bus.sent_to(0x1203, 0x0148) > 0 && bus.sent_to(0x1204, 0x0148) > 0);
  CHECK(bus.sent_to(0x1201, 0x0148) == 0);
  core.close();
}

// The guess, once the other master polls it too, is a TAP like the rest:
// the next one heard joins it rather than replacing it.
static void test_a_guess_the_master_polls_is_kept() {
  MultiTap bus;
  OtherMasterThenTaps port(bus);
  port.polled = {0x1201, 0x1202};
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_standby(1, nullptr, nullptr);
  std::vector<MasterCore::TapEntry> saved;
  core.set_learn_taps([&](const std::vector<MasterCore::TapEntry> &t) { saved = t; }, /*guessed=*/true);
  CHECK(core.open());
  uint32_t t = 0;
  tick_for_real(core, 400, t);
  CHECK((tap_ids(core) == std::vector<uint16_t>{0x1201, 0x1202}));
  CHECK(saved.size() == 2);
  core.close();
}

// A saved list is kept, MACs and all; a TAP the other master polls that
// isn't on it is added after it.
static void test_a_learned_tap_joins_a_saved_list() {
  MultiTap bus;
  OtherMasterThenTaps port(bus);
  port.polled = {0x1201, 0x1202};
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_taps({{0x1201, multi_mac(1)}});
  core.set_standby(1, nullptr, nullptr);
  int saves = 0;
  core.set_learn_taps([&](const std::vector<MasterCore::TapEntry> &) { saves++; }, /*guessed=*/false);
  CHECK(core.open());
  uint32_t t = 0;
  tick_for_real(core, 400, t);
  auto taps = core.taps();
  CHECK(taps.size() == 2 && taps[0].id == 0x1201 && taps[0].mac == multi_mac(1) && taps[1].id == 0x1202);
  CHECK(saves == 1);
  core.close();
}

// Polls to the factory addresses are a bring-up, not a TAP to run.
static void test_polls_to_the_factory_address_are_not_learned() {
  MultiTap bus;
  OtherMasterThenTaps port(bus);
  port.polled = {0x1234, 0x1235, 0x0000};
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_standby(1, nullptr, nullptr);
  int saves = 0;
  core.set_learn_taps([&](const std::vector<MasterCore::TapEntry> &) { saves++; }, /*guessed=*/true);
  CHECK(core.open());
  uint32_t t = 0;
  tick_for_real(core, 300, t);
  CHECK((tap_ids(core) == std::vector<uint16_t>{0x1201}) && saves == 0);
  core.close();
}

// Without learning (the Pi's default), the list is what it was given.
static void test_without_learning_the_list_stays() {
  MultiTap bus;
  OtherMasterThenTaps port(bus);
  port.polled = {0x1202};
  MasterCore core(&port, [](int, const std::string &) {}, nullptr, nullptr, 0x1201, false, false);
  core.set_standby(1, nullptr, nullptr);
  CHECK(core.open());
  uint32_t t = 0;
  tick_for_real(core, 300, t);
  CHECK((tap_ids(core) == std::vector<uint16_t>{0x1201}));
  core.close();
}

int main() {
  test_a_standby_learns_the_taps_another_master_polls();
  test_a_learned_tap_joins_a_saved_list();
  test_a_guess_the_master_polls_is_kept();
  test_polls_to_the_factory_address_are_not_learned();
  test_without_learning_the_list_stays();
  test_a_lost_tap_at_the_factory_address_is_brought_back();
  test_both_quiet_at_once_then_back_each_shows_running();
  test_a_table_change_on_a_lost_tap_is_refused();
  test_startup_with_one_of_two_at_the_factory_address_herds();
  test_a_stranger_at_the_factory_address_starts_no_herd();
  test_startup_with_a_stranger_polls_ours();
  test_adopting_herds_a_new_tap();
  test_not_adopting_leaves_a_new_tap_alone();
  test_the_owners_herd_keeps_known_ids();
  test_the_tunnel_seq_starts_at_01_after_a_reset();
  test_the_tunnel_seq_carries_over();
  test_radio_settings_need_the_bench_or_table_writes();
  test_a_herd_keeps_a_missing_tap();
  test_two_taps_are_polled_in_turn();
  test_no_0x41_with_more_than_one_tap();
  test_a_quiet_tap_does_not_stop_the_other();
  test_a_quiet_bus_is_still_asked_often();
  test_an_empty_tap_s_closed_window_is_logged_once();
  test_radio_reads_give_key_words();
  test_provision_takes_the_shared_key();
  test_provision_shared_key_refusals();
  test_known_networks_at_start();
  test_known_networks_waits_for_writes();
  test_known_networks_after_provision();
  test_known_networks_waits_for_a_table_job();
  test_known_networks_lost_and_unread();
  test_known_networks_refusal_is_said_once();
  test_known_networks_skip_a_tap_on_factory_settings();
  test_one_tap_is_alone_without_a_read();
  test_a_factory_tap_holds_no_key();
  test_two_new_taps_on_factory_settings();
  test_a_missed_settings_read_is_tried_again();
  test_a_tap_read_late_is_named_to_the_others();
  test_an_unanswered_e1_is_tried_again();
  test_waiting_for_writes_is_logged_once();
  test_provision_reads_settings_across_threads();
  test_one_tap_still_quiet_after_the_bus_is_back_is_removed();
  test_a_lost_tap_can_be_retired();
  test_no_tap_answering_is_a_fault();
  test_a_table_add_runs_on_its_tap();
  test_startup_finds_two_taps_at_their_ids();
  test_startup_without_one_tap_polls_the_other();
  test_pv_off_goes_to_every_tap();
  test_a_replace_lists_the_new_one_and_waits();
  test_a_removal_cut_off_by_a_tap_restart_is_finished();
  test_a_removal_saves_then_erases();
  test_a_removal_of_several();
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
