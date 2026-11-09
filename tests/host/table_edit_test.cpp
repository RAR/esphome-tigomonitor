// Host test for table_edit.h: an optimizer's label read as its MAC, the
// send gate's one shape of table write, and adding one optimizer to the
// TAP's table as the CCA did on the bench (2026-11-06), against a fake TAP.
//
// MACs are made up, with the 04C05B0000 prefix, but for taptap's published
// example (docs/protocol.md, "Barcodes").

#include "table_edit.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::master;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static Bytes hex(const char *s) {
  Bytes out;
  for (; s[0] && s[1]; s += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(s, 2), nullptr, 16)));
  return out;
}

static void test_labels() {
  // taptap's example, both ways.
  Bytes mac;
  CHECK(mac_of_label("4-9A57A2L", mac) && mac == hex("04C05B40009A57A2"));
  CHECK(label_of(hex("04C05B40009A57A2")) == "4-9A57A2L");
  // Made-up ones, lower case and with spaces as typed.
  CHECK(mac_of_label(" 0-1234r ", mac) && mac == hex("04C05B0000001234"));
  CHECK(label_of(hex("04C05B00000ABCDE")) == "0-ABCDEX");
  CHECK(label_of(hex("04C05B0000000000")) == "0-L");
  CHECK(mac_of_label("0-L", mac) && mac == hex("04C05B0000000000"));
  // A wrong check letter, a typo'd digit, the wrong shape: refused.
  CHECK(!mac_of_label("0-1234S", mac));
  CHECK(!mac_of_label("0-1235R", mac));
  CHECK(!mac_of_label("01234R", mac));
  CHECK(!mac_of_label("0-12345678901R", mac));
  CHECK(!mac_of_label("0-12G4R", mac));
  CHECK(!mac_of_label("", mac));
  // Not Tigo's prefix: no label.
  CHECK(label_of(hex("0011220000001234")).empty());
  CHECK(label_of(hex("04C05B00")).empty());
}

static Bytes record(const char *mac, uint16_t tuid) {
  Bytes r = hex(mac);
  r.push_back(static_cast<uint8_t>(tuid >> 8));
  r.push_back(static_cast<uint8_t>(tuid));
  return r;
}

static Bytes set_table(std::vector<Bytes> recs) {
  Bytes a = {static_cast<uint8_t>(recs.size())};
  for (const Bytes &r : recs) a.insert(a.end(), r.begin(), r.end());
  return a;
}

static void test_the_gate() {
  // The CCA's own shape: one record, bit 15 set.
  Bytes one = set_table({record("04C05B0000001234", 0x8002)});
  CHECK(is_set_table_args(one));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x29, 1, one)) == Allow::table_write);
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(0x29, 1, one), true, true, true, true));
  CHECK(may_send(CMD_TUNNEL, tunnel_request(0x29, 1, one), false, false, false, false, true));
  // One record a write, as the CCA's add: two are refused until a removal needs them.
  CHECK(!is_set_table_args(set_table({record("04C05B0000001234", 0x8002), record("04C05B0000001235", 0x8003)})));
  CHECK(!is_set_table_args(set_table({})));
  CHECK(!is_set_table_args(set_table({record("04C05B0000001234", 0x0002)})));  // bit 15 clear: joined at once
  CHECK(!is_set_table_args(set_table({record("04C05B0000001234", 0x8001)})));  // the TAP's own id
  CHECK(!is_set_table_args(set_table({record("04C05B0000001234", 0x8400)})));  // 1024
  CHECK(!is_set_table_args(set_table({record("04C05B0000001234", 0xC002)})));
  Bytes short_one = one;
  short_one.pop_back();
  CHECK(!is_set_table_args(short_one));
  Bytes counted_two = one;
  counted_two[0] = 2;
  CHECK(!is_set_table_args(counted_two));
  // Any other 0x29 is never sent, whatever the settings.
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x29, 1, short_one)) == Allow::never);
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(0x29, 1, short_one), true, true, true, true, true));
  // The erase stays never.
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, {0xBA, 0xBE}), true, true, true, true, true));
}

// A TAP with a table, answering pages of up to 12 from the id asked for, as
// the roof's does, and one write.
struct FakeTap {
  std::vector<std::pair<Bytes, uint16_t>> slots;  // MAC, TUID as read (0x8000: not joined)
  int ack = 0;
  bool answer_write = true;
  bool answer_pages = true;
  int writes = 0;
  int pages = 0;
  Bytes last_write;

  bool reply(const Request &req, Frame &out) {
    out = Frame();
    if (req.cmd != CMD_TUNNEL || req.payload.size() < 5) return false;
    uint8_t sub = req.payload[3], seq = req.payload[4];
    Bytes args(req.payload.begin() + 5, req.payload.end());
    out.dest = 0x9201;
    out.cmd = CMD_TUNNEL_REPLY;
    out.payload = {0x00, 0x0E, 0x00, static_cast<uint8_t>(sub + 1), seq};
    if (sub == 0x26) {
      pages++;
      if (!answer_pages) return false;
      uint16_t asked = static_cast<uint16_t>(args[0] << 8 | args[1]);
      auto sorted = slots;
      std::sort(sorted.begin(), sorted.end(),
                [](const auto &a, const auto &b) { return (a.second & 0x7FFF) < (b.second & 0x7FFF); });
      std::vector<std::pair<Bytes, uint16_t>> page;
      for (const auto &s : sorted)
        if ((s.second & 0x7FFF) >= asked && page.size() < 12) page.push_back(s);
      uint16_t first = page.empty() ? asked : page[0].second & 0x7FFF;
      out.payload.push_back(static_cast<uint8_t>(first >> 8));
      out.payload.push_back(static_cast<uint8_t>(first));
      out.payload.push_back(0);
      out.payload.push_back(static_cast<uint8_t>(page.size()));
      for (const auto &s : page) {
        out.payload.insert(out.payload.end(), s.first.begin(), s.first.end());
        out.payload.push_back(static_cast<uint8_t>(s.second >> 8));
        out.payload.push_back(static_cast<uint8_t>(s.second));
      }
      return true;
    }
    if (sub == 0x29) {
      writes++;
      last_write = args;
      if (ack == 0)
        for (size_t i = 0; i < args[0]; i++) {
          Bytes mac(args.begin() + 1 + 10 * i, args.begin() + 9 + 10 * i);
          slots.push_back({mac, static_cast<uint16_t>(args[9 + 10 * i] << 8 | args[10 + 10 * i])});
        }
      out.payload.push_back(static_cast<uint8_t>(ack));
      return answer_write;  // false: taken, but the reply lost
    }
    return false;
  }
  void join(const Bytes &mac) {
    for (auto &s : slots)
      if (s.first == mac) s.second &= 0x7FFF;
  }
};

// Runs the job against the TAP until it has nothing to send, advancing the
// clock a second a request. Returns how many requests went out.
static int run(TableAdd &job, FakeTap &tap, uint32_t &now, int limit = 400) {
  uint8_t seq = 0;
  int sent = 0;
  for (int i = 0; i < limit && !job.finished(); i++) {
    Request r = job.due(now, seq);
    if (!r.cmd) {
      now += 1000;
      continue;
    }
    CHECK(r.table_edit);
    CHECK(may_send(r.cmd, r.payload, false, false, false, false, true));
    sent++;
    Frame reply;
    bool ok = tap.reply(r, reply);
    job.on_reply(r, ok, reply, now);
    now += 1000;
  }
  return sent;
}

static FakeTap table_of(int n) {
  FakeTap t;
  for (int i = 0; i < n; i++) {
    char m[17];
    std::snprintf(m, sizeof(m), "04C05B00000010%02X", static_cast<unsigned>(i & 0xFF));
    t.slots.push_back({hex(m), static_cast<uint16_t>(2 + i)});
  }
  return t;
}

// The bench add: read the table, write one provisional record at the lowest
// free id, read back, then wait for the join with the window held open.
static void test_add_writes_one_record_and_waits_for_the_join() {
  FakeTap tap = table_of(25);  // three pages
  tap.slots[3].second |= 0x8000;  // one listed and not joined yet
  uint32_t now = 1000;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  CHECK(job.step() == TableAdd::Step::read_before);
  CHECK(!job.wants_window());
  for (int i = 0; i < 60 && job.step() != TableAdd::Step::joining && !job.finished(); i++) run(job, tap, now, 1);
  CHECK(job.step() == TableAdd::Step::joining);
  CHECK(tap.writes == 1);
  CHECK(tap.last_write == set_table({record("04C05B0000001234", 0x8000 | 27)}));
  CHECK(job.id() == 27);
  CHECK(job.wants_window());
  CHECK(job.ack() == 0);
  // Not joined: the job reads the table again a minute on, and keeps waiting.
  int pages = tap.pages;
  for (int i = 0; i < 70; i++) run(job, tap, now, 1);
  CHECK(tap.pages > pages);
  CHECK(job.step() == TableAdd::Step::joining);
  // Joined: done at the next read.
  tap.join(hex("04C05B0000001234"));
  for (int i = 0; i < 70 && !job.finished(); i++) run(job, tap, now, 1);
  CHECK(job.finished() && job.ok());
  CHECK(job.step() == TableAdd::Step::done);
  CHECK(!job.wants_window());
  CHECK(tap.writes == 1);
}

// A table with gaps is read whole, each page from one past the last id.
static void test_a_table_with_gaps() {
  FakeTap tap;
  for (int i = 0; i < 30; i++) {
    char m[17];
    std::snprintf(m, sizeof(m), "04C05B00000020%02X", static_cast<unsigned>(i));
    tap.slots.push_back({hex(m), static_cast<uint16_t>(2 + 3 * i)});
  }
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, true);
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(job.table().size() == 30);
  CHECK(job.id() == 3);
  CHECK(tap.pages == 4);  // 12, 12, 6, then the end
}

// The lowest free id from 2, by the id without its 0x8000 bit; or the one asked for.
static void test_the_id() {
  FakeTap tap;
  tap.slots = {{hex("04C05B0000000101"), 2}, {hex("04C05B0000000102"), 0x8003}, {hex("04C05B0000000105"), 6}};
  uint32_t now = 0;
  const FakeTap fresh = tap;
  FakeTap tap2 = fresh;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  run(job, tap, now, 6);
  CHECK(job.id() == 4);
  TableAdd asked(hex("04C05B0000001234"), 0x20, false);
  run(asked, tap2, now, 6);
  CHECK(asked.id() == 0x20);
  // One taken (provisional counts as taken), or out of range: nothing is written.
  for (uint16_t want : {uint16_t(3), uint16_t(1), uint16_t(1024)}) {
    FakeTap t = fresh;
    TableAdd bad(hex("04C05B0000001234"), want, false);
    run(bad, t, now);
    CHECK(bad.finished() && !bad.ok());
    CHECK(t.writes == 0);
  }
}

// A MAC already listed needs no write: the window does the rest.
static void test_already_listed() {
  FakeTap tap = table_of(4);
  tap.slots[2].second |= 0x8000;
  uint32_t now = 0;
  TableAdd job(tap.slots[2].first, 0, false);
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(tap.writes == 0);
  CHECK(job.id() == 4);
  CHECK(job.message().find("already") != std::string::npos);
}

// The TAP's refusal codes stop the job and are reported.
static void test_a_refused_write() {
  for (int ack : {1, 2, 3, 4, 5}) {
    FakeTap tap = table_of(3);
    tap.ack = ack;
    uint32_t now = 0;
    TableAdd job(hex("04C05B0000001234"), 0, false);
    run(job, tap, now);
    CHECK(job.finished() && !job.ok());
    CHECK(job.ack() == ack);
    CHECK(tap.writes == 1);
    CHECK(!job.message().empty());
  }
  FakeTap tap = table_of(3);
  tap.ack = 5;
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  run(job, tap, now);
  CHECK(job.message().find("another") != std::string::npos);
}

// A read back that doesn't show the table as it was plus the one record stops it.
static void test_a_bad_read_back() {
  FakeTap tap = table_of(5);
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  // Run until the write has gone out, then lose a record before the read back.
  for (int i = 0; i < 20 && tap.writes == 0; i++) run(job, tap, now, 1);
  CHECK(tap.writes == 1);
  tap.slots.erase(tap.slots.begin());
  run(job, tap, now);
  CHECK(job.finished() && !job.ok());
  CHECK(job.message().find("read back") != std::string::npos);
  // Another optimizer joining in between is not a difference.
  FakeTap tap2 = table_of(5);
  tap2.slots[1].second |= 0x8000;
  TableAdd job2(hex("04C05B0000001234"), 0, false);
  for (int i = 0; i < 20 && tap2.writes == 0; i++) run(job2, tap2, now, 1);
  tap2.join(tap2.slots[1].first);
  for (int i = 0; i < 20 && job2.step() != TableAdd::Step::joining && !job2.finished(); i++) run(job2, tap2, now, 1);
  CHECK(job2.step() == TableAdd::Step::joining);
}

// No ack: never sent again; the read back decides.
static void test_no_ack() {
  FakeTap tap = table_of(2);
  tap.answer_write = false;  // the TAP took it but the reply was lost
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  // Its write still lands: answer_write only drops the reply.
  for (int i = 0; i < 20 && job.step() != TableAdd::Step::joining && !job.finished(); i++) run(job, tap, now, 1);
  CHECK(tap.writes == 1);
  CHECK(job.step() == TableAdd::Step::joining);
  CHECK(job.ack() == -1);
}

// Pages that never come back: one retry, then the job stops having sent nothing.
static void test_no_table() {
  FakeTap tap = table_of(2);
  tap.answer_pages = false;
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  run(job, tap, now);
  CHECK(job.finished() && !job.ok());
  CHECK(tap.pages == 2);
  CHECK(tap.writes == 0);
}

// A dry run reads the table and lists the write without sending it.
static void test_dry_run() {
  FakeTap tap = table_of(3);
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, true);
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(tap.writes == 0);
  CHECK(job.dry_frames().size() == 1);
  CHECK(job.dry_frames()[0] == set_table({record("04C05B0000001234", 0x8005)}));
  CHECK(job.message().find("dry run") != std::string::npos);
}

// Not joined within the time: stopped, the record left provisional.
static void test_join_timeout() {
  FakeTap tap = table_of(2);
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  uint8_t seq = 0;
  for (int i = 0; i < 20000 && !job.finished(); i++) {
    Request r = job.due(now, seq);
    if (r.cmd) {
      Frame reply;
      bool ok = tap.reply(r, reply);
      job.on_reply(r, ok, reply, now);
    }
    now += 1000;
  }
  CHECK(job.finished() && !job.ok());
  CHECK(now >= TableAdd::JOIN_TIMEOUT_MS);
  CHECK(now < TableAdd::JOIN_TIMEOUT_MS + 5 * 60000);
  CHECK(!job.wants_window());
  CHECK(job.message().find("not joined") != std::string::npos);
}

int main() {
  test_labels();
  test_the_gate();
  test_add_writes_one_record_and_waits_for_the_join();
  test_the_id();
  test_a_table_with_gaps();
  test_already_listed();
  test_a_refused_write();
  test_a_bad_read_back();
  test_no_ack();
  test_no_table();
  test_dry_run();
  test_join_timeout();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
