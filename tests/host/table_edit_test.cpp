// Host test for table_edit.h: an optimizer's label read as its MAC, the
// send gate's shapes of table write and erase, and adding and removing an
// optimizer in the TAP's table as the CCA did on the bench (2026-11-06,
// 2026-11-09), against a fake TAP.
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
  // Up to ten a write, joined ones (bit 15 clear) among them, as the CCA
  // wrote a table back.
  std::vector<Bytes> ten;
  for (int i = 0; i < 10; i++) ten.push_back(record("04C05B0000001234", static_cast<uint16_t>((i % 2 ? 0x8002 : 0x0002) + i)));
  CHECK(is_set_table_args(set_table(ten)));
  ten.push_back(record("04C05B0000001235", 0x000C));
  CHECK(!is_set_table_args(set_table(ten)));
  CHECK(!is_set_table_args(set_table({})));
  CHECK(is_set_table_args(set_table({record("04C05B0000001234", 0x0002)})));
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
  // The erase: BA BE only, and only as a table write.
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, {0xBA, 0xBE})) == Allow::table_write);
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, {0xBA, 0xBE}), true, true, true, true, false));
  CHECK(may_send(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, {0xBA, 0xBE}), false, false, false, false, true));
  for (Bytes other : {Bytes{}, Bytes{0xBA}, Bytes{0xBA, 0xBF}, Bytes{0xBA, 0xBE, 0x00}})
    CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, other)) == Allow::never);
}

// A TAP with a table, answering pages of up to 12 from the id asked for, as
// the roof's does, and one write.
struct FakeTap {
  std::vector<std::pair<Bytes, uint16_t>> slots;  // MAC, TUID as read (0x8000: not joined)
  int ack = 0;
  int ack_on_write = 1;  // the write that gets ack (1: every one)
  int erase_ack = 0;
  bool erase_takes = true;
  bool answer_erase = true;
  int erases = 0;
  std::vector<Bytes> all_writes;
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
    if (sub == SUB_ERASE_FLASH) {
      erases++;
      if (erase_ack == 0 && erase_takes) slots.clear();
      out.payload.push_back(static_cast<uint8_t>(erase_ack));
      return answer_erase;
    }
    if (sub == 0x29) {
      writes++;
      last_write = args;
      all_writes.push_back(args);
      int this_ack = writes >= ack_on_write ? ack : 0;
      out.payload.push_back(static_cast<uint8_t>(this_ack));
      if (this_ack) return answer_write;
      if (true)
        for (size_t i = 0; i < args[0]; i++) {
          Bytes mac(args.begin() + 1 + 10 * i, args.begin() + 9 + 10 * i);
          slots.push_back({mac, static_cast<uint16_t>(args[9 + 10 * i] << 8 | args[10 + 10 * i])});
        }
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
static int run(TableJob &job, FakeTap &tap, uint32_t &now, int limit = 400, bool save_ok = true) {
  uint8_t seq = 0;
  int sent = 0;
  for (int i = 0; i < limit && !job.finished(); i++) {
    if (job.wants_save()) job.saved(save_ok);
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
// free id, read back. Done once it is listed; the wait for the join is
// JoinWatch's.
static void test_add_writes_one_record() {
  FakeTap tap = table_of(25);  // three pages
  tap.slots[3].second |= 0x8000;  // one listed and not joined yet
  uint32_t now = 1000;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  CHECK(job.step() == TableAdd::Step::read_before);
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(tap.writes == 1);
  CHECK(tap.last_write == set_table({record("04C05B0000001234", 0x8000 | 27)}));
  CHECK(job.id() == 27);
  CHECK(job.ack() == 0);
  CHECK(job.awaiting_join());
  CHECK(job.message() == "listed at 001B; waiting for it to join");
}

// ---- JoinWatch ----

// Reads the table a minute apart while any added optimizer hasn't joined;
// each that has, or is gone, is an outcome; the window is wanted meanwhile.
static int watch_run(JoinWatch &w, FakeTap &tap, uint32_t &now, uint32_t until) {
  uint8_t seq = 0;
  int sent = 0;
  for (; now < until; now += 1000) {
    Request r = w.due(now, seq);
    if (!r.cmd) continue;
    CHECK(r.table_edit);
    CHECK(classify_request(r.cmd, r.payload) == Allow::yes);  // reads only
    sent++;
    Frame reply;
    bool ok = tap.reply(r, reply);
    w.on_reply(ok, reply, now);
  }
  return sent;
}

static void test_join_watch() {
  FakeTap tap = table_of(3);
  Bytes a = hex("04C05B0000001234"), b = hex("04C05B0000001235"), c = hex("04C05B0000001236");
  tap.slots.push_back({a, 0x8005});
  tap.slots.push_back({b, 0x8006});
  tap.slots.push_back({c, 0x8007});
  JoinWatch w;
  uint32_t now = 0;
  CHECK(!w.wants_window());
  w.add(a, 5, now);
  w.add(b, 6, now);
  w.add(c, 7, now);
  CHECK(w.wants_window() && w.waiting().size() == 3);
  CHECK(watch_run(w, tap, now, 30000) == 0);  // the first read a minute on
  CHECK(watch_run(w, tap, now, 70000) > 0);
  CHECK(w.waiting().size() == 3 && w.take_outcomes().empty());
  tap.join(a);
  tap.slots.erase(tap.slots.begin() + 5);  // c gone from the table
  watch_run(w, tap, now, 140000);
  auto out = w.take_outcomes();
  CHECK(out.size() == 2);
  CHECK(out.size() == 2 && out[0].mac == a && out[0].joined && out[0].text == "joined at 0005");
  CHECK(out.size() == 2 && out[1].mac == c && !out[1].joined && out[1].text == "gone from the table");
  CHECK(w.waiting().size() == 1 && w.waiting()[0].mac == b);
  // Given up on by the owner: nothing waited for, nothing read.
  CHECK(w.drop(b) && !w.drop(b));
  CHECK(!w.wants_window());
  int pages = tap.pages;
  watch_run(w, tap, now, 400000);
  CHECK(tap.pages == pages);
}

// Each is given up after its own 30 min, left listed.
static void test_join_watch_timeout() {
  FakeTap tap = table_of(2);
  Bytes a = hex("04C05B0000001234"), b = hex("04C05B0000001235");
  tap.slots.push_back({a, 0x8004});
  tap.slots.push_back({b, 0x8005});
  JoinWatch w;
  uint32_t now = 0;
  w.add(a, 4, now);
  watch_run(w, tap, now, 10 * 60000);
  w.add(b, 5, now);
  watch_run(w, tap, now, JoinWatch::JOIN_TIMEOUT_MS + 1000);
  auto out = w.take_outcomes();
  CHECK(out.size() == 1 && out[0].mac == a && !out[0].joined);
  CHECK(out.size() == 1 && out[0].text.find("not joined after 30 min") == 0);
  CHECK(w.waiting().size() == 1);
  watch_run(w, tap, now, 10 * 60000 + JoinWatch::JOIN_TIMEOUT_MS + 1000);
  CHECK(w.take_outcomes().size() == 1 && !w.wants_window());
  CHECK(tap.slots.size() == 4);  // nothing written or removed
}

// A change starting mid-read: the read is dropped and begun again later.
static void test_join_watch_pause() {
  FakeTap tap = table_of(30);
  Bytes a = hex("04C05B0000001234");
  tap.slots.push_back({a, 0x8040});
  JoinWatch w;
  uint32_t now = 0;
  uint8_t seq = 0;
  w.add(a, 0x40, now);
  now = JoinWatch::JOIN_CHECK_MS;
  Request r = w.due(now, seq);
  Frame reply;
  CHECK(r.cmd && tap.reply(r, reply));
  w.on_reply(true, reply, now);  // one page of three
  w.pause();
  CHECK(!w.asked());
  tap.join(a);
  watch_run(w, tap, now, now + 5000);
  auto out = w.take_outcomes();
  CHECK(out.size() == 1 && out[0].joined);
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
  run(job2, tap2, now);
  CHECK(job2.ok() && job2.awaiting_join());
}

// No ack: never sent again; the read back decides.
static void test_no_ack() {
  FakeTap tap = table_of(2);
  tap.answer_write = false;  // the TAP took it but the reply was lost
  uint32_t now = 0;
  TableAdd job(hex("04C05B0000001234"), 0, false);
  // Its write still lands: answer_write only drops the reply.
  run(job, tap, now);
  CHECK(tap.writes == 1);
  CHECK(job.ok() && job.awaiting_join());
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
  Bytes frame = {0x29};
  Bytes args = set_table({record("04C05B0000001234", 0x8005)});
  frame.insert(frame.end(), args.begin(), args.end());
  CHECK(job.dry_frames()[0] == frame);
  CHECK(job.message().find("dry run") != std::string::npos);
}


// ---- removal ----

static std::vector<TableRecord> records_of(const FakeTap &tap) {
  std::vector<TableRecord> out;
  for (const auto &s : tap.slots) out.push_back({s.first, s.second});
  std::sort(out.begin(), out.end(), [](const TableRecord &a, const TableRecord &b) { return a.id() < b.id(); });
  return out;
}

// The bench removal: erase, then the others back exactly as read, joined
// ones with bit 15 clear, in id order; nothing rejoins.
static void test_remove_erases_and_writes_back_the_rest() {
  FakeTap tap = table_of(5);
  tap.slots[3].second |= 0x8000;  // one not joined yet keeps its bit
  std::swap(tap.slots[0], tap.slots[4]);  // slots out of id order
  std::vector<TableRecord> want = records_of(tap);
  Bytes gone = want[2].mac;
  want.erase(want.begin() + 2);
  uint32_t now = 0;
  TableRemove job(gone, false);
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(!job.keep_saved());
  CHECK(job.id() == 4);
  CHECK(tap.erases == 1);
  CHECK(tap.writes == 1);
  Bytes expect = {4};
  for (const auto &r : want) {
    expect.insert(expect.end(), r.mac.begin(), r.mac.end());
    expect.push_back(static_cast<uint8_t>(r.tuid >> 8));
    expect.push_back(static_cast<uint8_t>(r.tuid));
  }
  CHECK(tap.last_write == expect);
  std::vector<TableRecord> now_held = records_of(tap);
  CHECK(now_held.size() == 4);
  for (size_t i = 0; i < want.size() && i < now_held.size(); i++) CHECK(now_held[i].tuid == want[i].tuid);
  CHECK(job.message() == "removed 0004; the other 4 written back as they were");
}

// The write back waits the CCA's holdoff after the empty read, and no longer.
static void test_remove_waits_the_holdoff() {
  FakeTap tap = table_of(3);
  uint32_t now = 0;
  TableRemove job(tap.slots[0].first, false);
  uint8_t seq = 0;
  uint32_t empty_at = 0, written_at = 0;
  for (int i = 0; i < 400 && !job.finished(); i++, now += 50) {
    if (job.wants_save()) job.saved(true);
    Request r = job.due(now, seq);
    if (!r.cmd) continue;
    Frame reply;
    bool ok = tap.reply(r, reply);
    if (r.payload[3] == 0x29 && !written_at) written_at = now;
    job.on_reply(r, ok, reply, now);
    if (job.step() == TableRemove::Step::write && !empty_at) empty_at = now;
  }
  CHECK(job.ok());
  CHECK(written_at - empty_at >= TableRemove::REWRITE_HOLDOFF_MS);
  CHECK(written_at - empty_at <= TableRemove::REWRITE_HOLDOFF_MS + 50);
}

// More than ten stay: written ten at a time.
static void test_remove_writes_ten_at_a_time() {
  FakeTap tap = table_of(25);
  Bytes gone = tap.slots[0].first;
  uint32_t now = 0;
  TableRemove job(gone, false);
  run(job, tap, now);
  CHECK(job.ok());
  CHECK(tap.writes == 3);
  CHECK(tap.all_writes.size() == 3 && tap.all_writes[0][0] == 10 && tap.all_writes[1][0] == 10 && tap.all_writes[2][0] == 4);
  CHECK(tap.slots.size() == 24);
}

// Not listed: nothing erased. The only one: erased, nothing written.
static void test_remove_not_listed_or_the_only_one() {
  FakeTap tap = table_of(3);
  uint32_t now = 0;
  TableRemove absent(hex("04C05B0000001234"), false);
  run(absent, tap, now);
  CHECK(absent.ok() && tap.erases == 0 && tap.writes == 0);
  CHECK(absent.message() == "not in the table: nothing to remove");
  FakeTap one = table_of(1);
  TableRemove only(one.slots[0].first, false);
  run(only, one, now);
  CHECK(only.ok() && one.erases == 1 && one.writes == 0 && one.slots.empty());
}

// The list must be saved before the erase; if it can't be, nothing is erased.
static void test_remove_saves_before_erasing() {
  FakeTap tap = table_of(3);
  uint32_t now = 0;
  TableRemove job(tap.slots[1].first, false);
  uint8_t seq = 0;
  bool asked = false;
  for (int i = 0; i < 20 && !job.finished(); i++) {
    if (job.wants_save()) {
      asked = true;
      CHECK(tap.erases == 0);
      CHECK(job.survivors().size() == 2);
      job.saved(false);
    }
    Request r = job.due(now, seq);
    if (r.cmd) {
      Frame reply;
      bool ok = tap.reply(r, reply);
      job.on_reply(r, ok, reply, now);
    }
    now += 1000;
  }
  CHECK(asked);
  CHECK(job.finished() && !job.ok() && !job.keep_saved());
  CHECK(tap.erases == 0 && tap.slots.size() == 3);
}

// The TAP refuses the erase, or doesn't answer and it didn't take: stopped,
// the table as it was, nothing to resume. No answer but it took: carries on.
static void test_remove_erase_outcomes() {
  {
    FakeTap tap = table_of(3);
    tap.erase_ack = 1;
    uint32_t now = 0;
    TableRemove job(tap.slots[1].first, false);
    run(job, tap, now);
    CHECK(job.finished() && !job.ok() && !job.keep_saved() && job.ack() == 1);
    CHECK(tap.writes == 0 && tap.slots.size() == 3);
  }
  {
    FakeTap tap = table_of(3);
    tap.answer_erase = false;
    tap.erase_takes = false;
    uint32_t now = 0;
    TableRemove job(tap.slots[1].first, false);
    run(job, tap, now);
    CHECK(job.finished() && !job.ok() && !job.keep_saved());
    CHECK(job.message().find("didn't take") != std::string::npos);
    CHECK(tap.erases == 1 && tap.writes == 0);
  }
  {
    FakeTap tap = table_of(3);
    tap.answer_erase = false;
    uint32_t now = 0;
    TableRemove job(tap.slots[1].first, false);
    run(job, tap, now);
    CHECK(job.ok() && tap.erases == 1 && tap.slots.size() == 2);
  }
}

// A refused write back stops the job with the list kept for a resume; the
// resume, given that list, erases and writes it all.
static void test_remove_resumes_after_a_stop() {
  FakeTap tap = table_of(25);
  tap.ack = 3;
  tap.ack_on_write = 2;  // the second batch fails to commit
  Bytes gone = tap.slots[0].first;
  uint32_t now = 0;
  TableRemove job(gone, false);
  run(job, tap, now);
  CHECK(job.finished() && !job.ok() && job.keep_saved());
  CHECK(tap.slots.size() == 10);
  std::vector<TableRecord> saved = job.survivors();
  CHECK(saved.size() == 24);
  tap.ack = 0;
  TableRemove resume(gone, false, &saved);
  CHECK(resume.resuming());
  run(resume, tap, now);
  CHECK(resume.ok() && !resume.keep_saved());
  CHECK(tap.erases == 2);
  CHECK(same_table(records_of(tap), saved));
  // A resume that finds the table already right sends nothing.
  int erases = tap.erases, writes = tap.writes;
  TableRemove again(gone, false, &saved);
  run(again, tap, now);
  CHECK(again.ok() && tap.erases == erases && tap.writes == writes);
}

// A read back short of what stays: stopped, the list kept.
static void test_remove_bad_read_back() {
  FakeTap tap = table_of(4);
  tap.answer_write = false;
  tap.ack = 3;  // not taken, and the reply lost: the read back decides
  uint32_t now = 0;
  TableRemove job(tap.slots[0].first, false);
  run(job, tap, now);
  CHECK(job.finished() && !job.ok() && job.keep_saved());
  CHECK(tap.writes == 1);
}

// A dry run reads, then lists the erase and the write backs, sending neither.
static void test_remove_dry_run() {
  FakeTap tap = table_of(12);
  uint32_t now = 0;
  TableRemove job(tap.slots[0].first, true);
  run(job, tap, now);
  CHECK(job.ok() && tap.erases == 0 && tap.writes == 0);
  CHECK(job.dry_frames().size() == 3);
  CHECK(job.dry_frames()[0] == (Bytes{0x2B, 0xBA, 0xBE}));
  CHECK(job.dry_frames()[1][0] == 0x29 && job.dry_frames()[1][1] == 10);
  CHECK(job.dry_frames()[2][0] == 0x29 && job.dry_frames()[2][1] == 1);
}

// ---- replace ----

// The old record left out, the new one written provisional at its id, in
// the same rewrite; every other record back as read.
static void test_replace_writes_the_new_one_at_the_old_id() {
  FakeTap tap = table_of(12);
  Bytes old = tap.slots[5].first;  // id 7
  Bytes fresh = hex("04C05B0000102E66");
  uint32_t now = 0;
  TableReplace job(old, fresh, false);
  CHECK(std::string(job.kind()) == "replace");
  run(job, tap, now);
  CHECK(job.finished() && job.ok());
  CHECK(job.id() == 7);
  CHECK(tap.erases == 1 && tap.writes == 2);
  std::vector<TableRecord> held = records_of(tap);
  CHECK(held.size() == 12);
  bool old_gone = true, fresh_at_7 = false;
  for (const auto &r : held) {
    if (r.mac == old) old_gone = false;
    if (r.mac == fresh) fresh_at_7 = r.tuid == 0x8007;
  }
  CHECK(old_gone && fresh_at_7);
  CHECK(job.awaiting_join());
  CHECK(job.message() == "replaced 0007; the other 11 written back as they were, the new one waiting to join");
}

// The new one already listed, the old one not: stopped before anything is sent.
static void test_replace_refusals() {
  FakeTap tap = table_of(4);
  uint32_t now = 0;
  TableReplace listed(tap.slots[0].first, tap.slots[1].first, false);
  run(listed, tap, now);
  CHECK(listed.finished() && !listed.ok() && tap.erases == 0);
  CHECK(listed.message().find("already in the table") != std::string::npos);
  TableReplace absent(hex("04C05B0000009999"), hex("04C05B0000102E66"), false);
  run(absent, tap, now);
  CHECK(absent.finished() && !absent.ok() && tap.erases == 0);
  CHECK(absent.message().find("not in the table") != std::string::npos);
  TableReplace same(tap.slots[0].first, tap.slots[0].first, false);
  run(same, tap, now);
  CHECK(!same.ok() && tap.erases == 0);
}

// A dry run lists the erase and the writes, the new record among them.
static void test_replace_dry_run() {
  FakeTap tap = table_of(3);
  uint32_t now = 0;
  TableReplace job(tap.slots[1].first, hex("04C05B0000102E66"), true);
  run(job, tap, now);
  CHECK(job.ok() && tap.erases == 0 && tap.writes == 0);
  CHECK(job.dry_frames().size() == 2);
  Bytes expect = {0x29, 3};
  for (const auto &r : std::vector<std::pair<Bytes, uint16_t>>{{tap.slots[0].first, 2}, {hex("04C05B0000102E66"), 0x8003}, {tap.slots[2].first, 4}}) {
    expect.insert(expect.end(), r.first.begin(), r.first.end());
    expect.push_back(static_cast<uint8_t>(r.second >> 8));
    expect.push_back(static_cast<uint8_t>(r.second));
  }
  CHECK(job.dry_frames().size() == 2 && job.dry_frames()[1] == expect);
}

int main() {
  test_replace_writes_the_new_one_at_the_old_id();
  test_replace_refusals();
  test_replace_dry_run();
  test_remove_erases_and_writes_back_the_rest();
  test_remove_writes_ten_at_a_time();
  test_remove_waits_the_holdoff();
  test_remove_not_listed_or_the_only_one();
  test_remove_saves_before_erasing();
  test_remove_erase_outcomes();
  test_remove_resumes_after_a_stop();
  test_remove_bad_read_back();
  test_remove_dry_run();
  test_labels();
  test_the_gate();
  test_add_writes_one_record();
  test_join_watch();
  test_join_watch_timeout();
  test_join_watch_pause();
  test_the_id();
  test_a_table_with_gaps();
  test_already_listed();
  test_a_refused_write();
  test_a_bad_read_back();
  test_no_ack();
  test_no_table();
  test_dry_run();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
