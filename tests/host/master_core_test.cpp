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

int main() {
  test_a_lost_tap_at_the_factory_address_is_brought_back();
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
