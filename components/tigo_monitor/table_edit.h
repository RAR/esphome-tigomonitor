#pragma once

// Adding an optimizer to the TAP's table and removing one, from the bench
// captures of a CCA doing both (2026-11-06 and 2026-11-09; see
// docs/superpowers/specs/2026-11-09-optimizer-table-edits-design.md).
//   - Add: read the table (sub 0x26), write one record (sub 0x29) with bit
//     15 set, and read it back. JoinWatch then holds the discovery window
//     open until the TAP clears the bit, which it does when the optimizer
//     joins, for any number of added optimizers at once. The CCA
//     erases the table and rewrites it with the new record; this appends,
//     as the owner's analysis says a 0x29 does, so an add never erases.
//   - Remove: there is no single-record delete. Read the table, erase it
//     (sub 0x2B BA BE), and write back what stays exactly as read, as the
//     CCA did. Nothing rejoins.
// Neither sends the CCA's 0x22s with data or its writes to optimizers.
//
// And an optimizer's printed label read as its MAC, from taptap's
// docs/protocol.md ("Barcodes"): "4-9A57A2L" is 04C05B 4 0 00 9A 57 A2. The
// vendor prefix 04C05B is left out, the character before the dash is the
// next nibble, the dash stands for zeroes, the hex digits are the rest of
// the address, and the last character is a CRC-4 of all eight bytes.
//
// Header-only and free of ESPHome, like tigo_master.h;
// tests/host/table_edit_test.cpp covers it.

#include "tigo_master.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

// ---- labels ----------------------------------------------------------------

static const char LABEL_CHECK_DIGITS[] = "GHJKLMNPRSTVWXYZ";
static const uint8_t TIGO_PREFIX[] = {0x04, 0xC0, 0x5B};

// CRC-4, polynomial 0x3, starting at 0x2, over the whole address.
inline uint8_t label_check(const Bytes &mac) {
  uint8_t crc = 0x2;
  for (uint8_t b : mac) {
    uint8_t r = static_cast<uint8_t>(b ^ (crc << 4));
    for (int i = 0; i < 8; i++) r = static_cast<uint8_t>(r & 0x80 ? (r << 1) ^ 0x30 : r << 1);
    crc = r >> 4;
  }
  return crc;
}

// The label printed on an optimizer with this MAC, or "" if it isn't Tigo's.
inline std::string label_of(const Bytes &mac) {
  if (mac.size() != 8 || !std::equal(TIGO_PREFIX, TIGO_PREFIX + 3, mac.begin())) return "";
  static const char HEX[] = "0123456789ABCDEF";
  std::string nibbles;
  for (size_t i = 3; i < 8; i++) {
    nibbles += HEX[mac[i] >> 4];
    nibbles += HEX[mac[i] & 0xF];
  }
  std::string rest = nibbles.substr(1);
  rest.erase(0, rest.find_first_not_of('0') == std::string::npos ? rest.size() : rest.find_first_not_of('0'));
  return std::string(1, nibbles[0]) + "-" + rest + LABEL_CHECK_DIGITS[label_check(mac)];
}

// The MAC a label stands for; false for a label of the wrong shape or one
// whose check letter doesn't match (a typo). Case and spaces around it are
// ignored.
inline bool mac_of_label(const std::string &typed, Bytes &mac) {
  std::string s;
  for (char c : typed)
    if (c != ' ' && c != '\t') s += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
  size_t dash = s.find('-');
  if (dash != 1 || s.size() < 3) return false;
  std::string nibbles = s.substr(0, 1) + s.substr(2, s.size() - 3);
  if (nibbles.size() > 10) return false;
  for (char c : nibbles)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  nibbles.insert(1, 10 - nibbles.size(), '0');
  Bytes out(TIGO_PREFIX, TIGO_PREFIX + 3);
  for (size_t i = 0; i < 10; i += 2) out.push_back(static_cast<uint8_t>(std::stoi(nibbles.substr(i, 2), nullptr, 16)));
  if (s.back() != LABEL_CHECK_DIGITS[label_check(out)]) return false;
  mac = out;
  return true;
}

// ---- the table -------------------------------------------------------------

// One record of the TAP's table as a read gives it: the optimizer's MAC and
// its TUID, with 0x8000 set while it is listed but hasn't joined.
struct TableRecord {
  Bytes mac;
  uint16_t tuid = 0;
  uint16_t id() const { return tuid & 0x7FFF; }
  bool joined() const { return !(tuid & 0x8000); }
};

// A sub 0x27 reply: the first id at or after the one asked for, then
// count x (MAC, TUID), in id order (the id without its 0x8000 bit). A count
// of 0 is the end of the table. The roof's TAP, asked from 0000, answered
// from 0002 with 12, and the CCA asked next from 000E (sunrise, 2026-11-08).
inline bool parse_table_page(const TunnelReply &r, uint16_t &start, std::vector<TableRecord> &out) {
  if (r.sub != 0x27 || r.data.size() < 4) return false;
  start = static_cast<uint16_t>(r.data[0] << 8 | r.data[1]);
  size_t count = static_cast<size_t>(r.data[2] << 8 | r.data[3]);
  if (r.data.size() < 4 + 10 * count) return false;
  out.clear();
  for (size_t i = 0; i < count; i++) {
    const uint8_t *p = r.data.data() + 4 + 10 * i;
    out.push_back({Bytes(p, p + 8), static_cast<uint16_t>(p[8] << 8 | p[9])});
  }
  return true;
}

inline std::string mac_hex(const Bytes &mac) {
  std::string s;
  char b[3];
  for (uint8_t v : mac) {
    std::snprintf(b, sizeof(b), "%02X", v);
    s += b;
  }
  return s;
}

// What the 0x29 ack byte means (the owner's analysis of the TAP's firmware).
inline const char *set_table_ack_text(int ack) {
  switch (ack) {
    case 0: return "done";
    case 1: return "the address is outside 2..1023";
    case 2: return "the table is full";
    case 3: return "the TAP couldn't save it";
    case 4: return "that address is already in use";
    case 5: return "that optimizer is already listed under another address";
    default: return "an answer we don't know";
  }
}

// Reads the whole table, a page at a time: each page asked from one past
// the last id the one before held (the CCA's own next index, first + count,
// on a table with no gaps, and right on one with). A page with no reply is
// asked once more.
class TableWalk {
 public:
  enum Result { more, done, failed };
  static const uint32_t PAGE_WAIT_MS = 500;  // as Housekeeping's walks
  static const int MAX_PAGES = 110;          // 1024 slots at about 12 a page

  void start() {
    active_ = true;
    index_ = 0;
    pages_ = 0;
    retried_ = false;
    records_.clear();
  }
  bool active() const { return active_; }
  Request request(uint8_t &seq) {
    seq = next_seq(seq);
    pages_++;
    Request r{CMD_TUNNEL, tunnel_request(0x26, seq, {static_cast<uint8_t>(index_ >> 8), static_cast<uint8_t>(index_)}),
              PAGE_WAIT_MS};
    r.table_edit = true;
    return r;
  }
  Result on_reply(bool ok, const Frame &reply) {
    TunnelReply t;
    uint16_t start = 0;
    std::vector<TableRecord> page;
    if (!ok || reply.cmd != CMD_TUNNEL_REPLY || !parse_tunnel_reply(reply.payload, t) || !parse_table_page(t, start, page)) {
      if (!retried_) {
        retried_ = true;
        pages_--;
        return more;
      }
      active_ = false;
      return failed;
    }
    retried_ = false;
    if (page.empty()) {
      active_ = false;
      return done;
    }
    uint32_t next = static_cast<uint32_t>(page.back().id()) + 1;
    if (start < index_ || next <= index_ || pages_ >= MAX_PAGES) {
      active_ = false;
      return failed;
    }
    for (const auto &r : page)
      if (!find(records_, r.mac)) records_.push_back(r);
    index_ = static_cast<uint16_t>(next);
    return more;
  }
  const std::vector<TableRecord> &records() const { return records_; }

  static const TableRecord *find(const std::vector<TableRecord> &t, const Bytes &mac) {
    for (const auto &r : t)
      if (r.mac == mac) return &r;
    return nullptr;
  }

 private:
  bool active_ = false;
  bool retried_ = false;
  uint16_t index_ = 0;
  int pages_ = 0;
  std::vector<TableRecord> records_;
};

// Whether two tables list the same optimizers at the same ids, whatever
// their 0x8000 bits (one may have joined in between).
inline bool same_table(const std::vector<TableRecord> &a, const std::vector<TableRecord> &b) {
  if (a.size() != b.size()) return false;
  for (const auto &r : a) {
    const TableRecord *o = TableWalk::find(b, r.mac);
    if (!o || o->id() != r.id()) return false;
  }
  return true;
}

// An owner's change to the TAP's table, start to finish: due() for the next
// request, on_reply() with what came back. One request at a time, each
// marked table_edit. A write or erase with no ack is never repeated: the
// read after it decides whether it took.
class TableJob : public RequestSource {
 public:
  virtual void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t now_ms) = 0;
  virtual const char *kind() const = 0;       // "add", "remove"
  virtual const char *step_text() const = 0;  // for the Devices page
  virtual bool finished() const = 0;
  virtual bool ok() const = 0;
  // A removal's records to write back, before its erase: the caller saves
  // them (resume after a stop), then calls saved().
  virtual bool wants_save() const { return false; }
  virtual void saved(bool) {}
  // Whether a saved list must stay: a removal stopped after its erase.
  virtual bool keep_saved() const { return false; }
  virtual const std::vector<TableRecord> &survivors() const { return none_; }

  bool dry_run() const { return dry_run_; }
  const Bytes &mac() const { return mac_; }
  uint16_t id() const { return id_; }  // 0 until known
  int ack() const { return ack_; }     // the last write's or erase's; -1: none heard
  const std::string &message() const { return message_; }
  // A dry run's frames, as the sub then the args after the seq.
  const std::vector<Bytes> &dry_frames() const { return dry_frames_; }
  const std::vector<TableRecord> &table() const { return before_; }  // as read first

 protected:
  TableJob(const Bytes &mac, bool dry_run) : mac_(mac), dry_run_(dry_run) {}
  static std::string hex4_(uint16_t v) {
    char b[5];
    std::snprintf(b, sizeof(b), "%04X", v);
    return b;
  }
  Request send_(uint8_t sub, const Bytes &args, uint32_t wait_ms, uint8_t &seq) {
    seq = next_seq(seq);
    waiting_ = true;
    Request r{CMD_TUNNEL, tunnel_request(sub, seq, args), wait_ms};
    r.table_edit = true;
    return r;
  }
  // The reply to the job's own request: false if it isn't one.
  bool take_(const Request &req) {
    if (!req.table_edit || !waiting_ || req.payload.size() < 5) return false;
    waiting_ = false;
    return true;
  }
  // The ack byte of a write (0x2A) or erase (0x2C) reply, or -1.
  static int ack_of_(bool ok, const Frame &reply, uint8_t sub) {
    TunnelReply t;
    if (ok && reply.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(reply.payload, t) && t.sub == sub && !t.data.empty())
      return t.data[0];
    return -1;
  }

  Bytes mac_;
  bool dry_run_;
  uint16_t id_ = 0;
  int ack_ = -1;
  std::string message_;
  bool waiting_ = false;
  TableWalk walk_;
  std::vector<TableRecord> before_;
  std::vector<Bytes> dry_frames_;

 private:
  std::vector<TableRecord> none_;
};

// Adding one optimizer: read the table, write one record with bit 15 set at
// the lowest free id from 2 (or the one asked for), and read it back. Then
// it's done: waiting for the optimizer to join is JoinWatch's, so the next
// change can start at once.
class TableAdd : public TableJob {
 public:
  enum class Step { read_before, write, read_after, done, failed };
  static const uint32_t PAGE_WAIT_MS = TableWalk::PAGE_WAIT_MS;
  static const uint32_t WRITE_WAIT_MS = 2000;  // the TAP acks once the record is in flash

  // want_id 0: the lowest free id from 2, as the CCA chose on the bench.
  TableAdd(const Bytes &mac, uint16_t want_id, bool dry_run) : TableJob(mac, dry_run), want_id_(want_id) {}

  Request due(uint32_t, uint8_t &seq) override {
    if (finished() || waiting_) return {};
    if (step_ == Step::write) {
      Bytes args = {1};
      args.insert(args.end(), mac_.begin(), mac_.end());
      args.push_back(static_cast<uint8_t>(0x80 | id_ >> 8));
      args.push_back(static_cast<uint8_t>(id_));
      if (dry_run_) {
        Bytes f = {0x29};
        f.insert(f.end(), args.begin(), args.end());
        dry_frames_.push_back(f);
        finish_("dry run: would list " + mac_hex(mac_) + " at " + hex4_(id_) + " (not joined)");
        return {};
      }
      return send_(0x29, args, WRITE_WAIT_MS, seq);
    }
    if (!walk_.active()) walk_.start();
    waiting_ = true;
    return walk_.request(seq);
  }

  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t) override {
    if (!take_(req)) return;
    if (req.payload[3] == 0x29) {
      ack_ = ack_of_(ok, reply, 0x2A);
      if (ack_ > 0) return fail_(std::string("the TAP refused the write: ") + set_table_ack_text(ack_));
      step_ = Step::read_after;  // no ack: the read back says whether it took
      return;
    }
    switch (walk_.on_reply(ok, reply)) {
      case TableWalk::more: return;
      case TableWalk::failed: return fail_("the TAP didn't answer a read of its table");
      case TableWalk::done: return walked_();
    }
  }

  const char *kind() const override { return "add"; }
  Step step() const { return step_; }
  bool finished() const override { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const override { return step_ == Step::done; }
  // Listed and not joined yet: for JoinWatch.
  bool awaiting_join() const { return awaiting_; }
  const char *step_text() const override { return step_name(step_); }
  static const char *step_name(Step s) {
    switch (s) {
      case Step::read_before: return "reading the table";
      case Step::write: return "writing";
      case Step::read_after: return "reading it back";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }

 private:
  void fail_(const std::string &why) {
    step_ = Step::failed;
    message_ = why;
    waiting_ = false;
  }
  void finish_(const std::string &what) {
    step_ = Step::done;
    message_ = what;
    waiting_ = false;
  }
  void walked_() {
    const std::vector<TableRecord> &walk = walk_.records();
    if (step_ == Step::read_before) {
      before_ = walk;
      if (const TableRecord *r = TableWalk::find(before_, mac_)) {
        id_ = r->id();
        awaiting_ = !r->joined() && !dry_run_;
        return finish_("already in the table at " + hex4_(id_) + (r->joined() ? ", joined" : ", not joined yet") +
                       ": nothing to write" + (r->joined() ? "" : "; waiting for it to join"));
      }
      std::vector<bool> used(1024, false);
      for (const auto &r : before_)
        if (r.id() < 1024) used[r.id()] = true;
      if (want_id_) {
        if (want_id_ < 2 || want_id_ > 1023) return fail_("an address must be 0002..03FF");
        if (used[want_id_]) return fail_(hex4_(want_id_) + " is already in the table");
        id_ = want_id_;
      } else {
        for (uint16_t i = 2; i < 1024 && !id_; i++)
          if (!used[i]) id_ = i;
        if (!id_) return fail_("the table has no free address");
      }
      step_ = Step::write;
      return;
    }
    // The read back: as before, plus the one record.
    const TableRecord *added = TableWalk::find(walk, mac_);
    std::vector<TableRecord> expected = before_;
    if (added) expected.push_back({mac_, id_});
    if (!added || !same_table(walk, expected))
      return fail_(added        ? "the read back doesn't match the table before the write"
                   : ack_ == -1 ? "no answer to the write, and the read back doesn't list it"
                                : "the read back doesn't list it");
    if (added->joined()) return finish_("listed at " + hex4_(id_) + " and joined");
    awaiting_ = true;
    finish_("listed at " + hex4_(id_) + "; waiting for it to join");
  }

  uint16_t want_id_;
  Step step_ = Step::read_before;
  bool awaiting_ = false;
};

// The optimizers listed and not joined yet, after their adds: the window
// is wanted open while any are, and the table is read every JOIN_CHECK_MS
// to see which have joined (the TAP clears their 0x8000 bit). Each is given
// up after JOIN_TIMEOUT_MS, left listed: it joins whenever the window is
// next open. A table change running pauses the reads (pause()).
class JoinWatch : public RequestSource {
 public:
  static const uint32_t JOIN_CHECK_MS = 60000;
  static const uint32_t JOIN_TIMEOUT_MS = 30 * 60000;  // the bench's joins took 3.5 and 10 min
  static const size_t RECENT = 10;
  struct Waiting {
    Bytes mac;
    uint16_t id = 0;
    uint32_t since_ms = 0;
  };
  struct Outcome {
    Bytes mac;
    uint16_t id = 0;
    bool joined = false;
    std::string text;
  };

  void add(const Bytes &mac, uint16_t id, uint32_t now_ms) {
    drop(mac);
    waiting_.push_back({mac, id, now_ms});
    if (waiting_.size() == 1) next_check_ms_ = now_ms + JOIN_CHECK_MS;
  }
  // No longer waited for (removed from the table, or the owner gave up). True if it was.
  bool drop(const Bytes &mac) {
    for (size_t i = 0; i < waiting_.size(); i++)
      if (waiting_[i].mac == mac) {
        waiting_.erase(waiting_.begin() + static_cast<long>(i));
        return true;
      }
    return false;
  }
  // A table change is about to run: abandon a read half done.
  void pause() {
    walk_ = TableWalk();
    asked_ = false;
  }
  bool wants_window() const { return !waiting_.empty(); }
  const std::vector<Waiting> &waiting() const { return waiting_; }
  // Newest last; take_outcomes() hands over the ones not yet seen.
  std::vector<Outcome> take_outcomes() {
    std::vector<Outcome> out;
    out.swap(new_outcomes_);
    return out;
  }

  Request due(uint32_t now_ms, uint8_t &seq) override {
    if (asked_) return {};
    // Time limits, checked whether or not a read is due.
    for (size_t i = 0; i < waiting_.size();) {
      if (static_cast<int32_t>(now_ms - waiting_[i].since_ms) >= static_cast<int32_t>(JOIN_TIMEOUT_MS)) {
        outcome_(waiting_[i], false,
                 "not joined after " + std::to_string(JOIN_TIMEOUT_MS / 60000) +
                     " min: it stays listed, and joins whenever the window is next open");
        waiting_.erase(waiting_.begin() + static_cast<long>(i));
      } else {
        i++;
      }
    }
    if (waiting_.empty()) {
      walk_ = TableWalk();
      return {};
    }
    if (!walk_.active()) {
      if (static_cast<int32_t>(now_ms - next_check_ms_) < 0) return {};
      walk_.start();
    }
    asked_ = true;
    return walk_.request(seq);
  }

  // A reply to one of its own reads (Request::table_edit, from due()).
  void on_reply(bool ok, const Frame &reply, uint32_t now_ms) {
    if (!asked_) return;
    asked_ = false;
    switch (walk_.on_reply(ok, reply)) {
      case TableWalk::more: return;
      case TableWalk::failed:
        next_check_ms_ = now_ms + JOIN_CHECK_MS;
        return;
      case TableWalk::done: break;
    }
    next_check_ms_ = now_ms + JOIN_CHECK_MS;
    const std::vector<TableRecord> &table = walk_.records();
    for (size_t i = 0; i < waiting_.size();) {
      const TableRecord *r = TableWalk::find(table, waiting_[i].mac);
      if (r && !r->joined()) {
        i++;
        continue;
      }
      char b[5];
      std::snprintf(b, sizeof(b), "%04X", r ? r->id() : waiting_[i].id);
      outcome_(waiting_[i], r != nullptr, r ? std::string("joined at ") + b : std::string("gone from the table"));
      waiting_.erase(waiting_.begin() + static_cast<long>(i));
    }
  }
  bool asked() const { return asked_; }

 private:
  void outcome_(const Waiting &w, bool joined, const std::string &text) {
    new_outcomes_.push_back({w.mac, w.id, joined, text});
  }
  std::vector<Waiting> waiting_;
  std::vector<Outcome> new_outcomes_;
  TableWalk walk_;
  bool asked_ = false;
  uint32_t next_check_ms_ = 0;
};

// Removing one optimizer, as the CCA did on the bench (2026-11-09): there is
// no single-record delete, so read the table, have the records that stay
// saved (the caller's job: wants_save, saved), erase the table (sub 0x2B
// BA BE), check it reads empty, write the records that stay back exactly as
// read (bit 15 and all, up to 10 a write), and read back. Nothing has to
// rejoin, and no window is opened: the CCA opened none.
//
// resume: the records a removal stopped before writing back (saved), for
// the next start to finish: read, and unless the table already holds just
// those, erase and write them.
class TableRemove : public TableJob {
 public:
  enum class Step { read_before, save, erase, read_empty, write, read_after, done, failed };
  static const uint32_t ERASE_WAIT_MS = 3000;  // the TAP erases, then acks: 16-48 ms on the bench
  // From the empty read to the first write back, as the CCA's holdoff (0.5 s,
  // and 0.7 to 2.3 s seen, 2026-11-06/09). Every optimizer is unjoined until
  // the write back, so this is the whole of the gap.
  static const uint32_t REWRITE_HOLDOFF_MS = 500;
  static const uint32_t WRITE_WAIT_MS = TableAdd::WRITE_WAIT_MS;
  static constexpr size_t PER_WRITE = 10;

  TableRemove(const Bytes &mac, bool dry_run, const std::vector<TableRecord> *resume = nullptr)
      : TableJob(mac, dry_run), resuming_(resume != nullptr) {
    if (resume) survivors_ = *resume;
  }

  Request due(uint32_t now_ms, uint8_t &seq) override {
    if (finished() || waiting_ || step_ == Step::save) return {};
    if (step_ == Step::write && written_ == 0 && static_cast<int32_t>(now_ms - empty_at_ms_) < static_cast<int32_t>(REWRITE_HOLDOFF_MS))
      return {};
    if (step_ == Step::erase) {
      if (dry_run_) {
        dry_frames_.push_back({SUB_ERASE_FLASH, 0xBA, 0xBE});
        for (size_t i = 0; i < survivors_.size(); i += PER_WRITE) {
          Bytes f = {0x29};
          Bytes a = batch_(i);
          f.insert(f.end(), a.begin(), a.end());
          dry_frames_.push_back(f);
        }
        finish_(add_mac_.empty()
                    ? "dry run: would erase the table and write back the other " + std::to_string(survivors_.size())
                    : "dry run: would erase the table, write back the other " + std::to_string(survivors_.size() - 1) +
                          " and the new one at " + hex4_(id_) + " (not joined)");
        return {};
      }
      erase_sent_ = true;
      return send_(SUB_ERASE_FLASH, {0xBA, 0xBE}, ERASE_WAIT_MS, seq);
    }
    if (step_ == Step::write) return send_(0x29, batch_(written_), WRITE_WAIT_MS, seq);
    if (!walk_.active()) walk_.start();
    waiting_ = true;
    return walk_.request(seq);
  }

  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t now_ms) override {
    if (!take_(req)) return;
    uint8_t sub = req.payload[3];
    empty_at_ms_ = now_ms;  // the read that finds it empty is the last reply before the holdoff
    if (sub == SUB_ERASE_FLASH) {
      ack_ = ack_of_(ok, reply, 0x2C);
      if (ack_ > 0) return fail_("the TAP refused the erase (" + std::to_string(ack_) + "); its table is as it was", false);
      step_ = Step::read_empty;  // no ack: the read says whether it took
      return;
    }
    if (sub == 0x29) {
      ack_ = ack_of_(ok, reply, 0x2A);
      if (ack_ > 0)
        return fail_(std::string("the TAP refused a write back: ") + set_table_ack_text(ack_) +
                         "; the rest is saved and is written at the next start",
                     true);
      if (ack_ < 0) {  // never sent again; read what the TAP holds
        step_ = Step::read_after;
        return;
      }
      written_ += std::min(PER_WRITE, survivors_.size() - written_);
      if (written_ >= survivors_.size()) step_ = Step::read_after;
      return;
    }
    switch (walk_.on_reply(ok, reply)) {
      case TableWalk::more: return;
      case TableWalk::failed:
        return fail_("the TAP didn't answer a read of its table", step_ != Step::read_before);
      case TableWalk::done: return walked_();
    }
  }

  bool wants_save() const override { return step_ == Step::save; }
  void saved(bool ok) override {
    if (step_ != Step::save) return;
    if (!ok) return fail_("couldn't save the records to write back; nothing was erased", false);
    step_ = Step::erase;
  }
  bool keep_saved() const override { return keep_saved_; }
  const std::vector<TableRecord> &survivors() const override { return survivors_; }

  const char *kind() const override { return add_mac_.empty() ? "remove" : "replace"; }
  Step step() const { return step_; }
  bool finished() const override { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const override { return step_ == Step::done; }
  bool resuming() const { return resuming_; }
  bool erase_sent() const { return erase_sent_; }
  // A replace's new optimizer, listed and not joined yet: for JoinWatch.
  bool awaiting_join() const { return ok() && !add_mac_.empty() && !dry_run_; }
  const Bytes &added_mac() const { return add_mac_; }
  const char *step_text() const override { return step_name(step_); }
  static const char *step_name(Step s) {
    switch (s) {
      case Step::read_before: return "reading the table";
      case Step::save: return "saving what stays";
      case Step::erase: return "erasing the table";
      case Step::read_empty: return "checking it's empty";
      case Step::write: return "writing back what stays";
      case Step::read_after: return "reading it back";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }

 protected:
  // A replace: the new optimizer, written provisional at the removed one's id.
  Bytes add_mac_;

 private:
  Bytes batch_(size_t from) const {
    size_t n = std::min(PER_WRITE, survivors_.size() - from);
    Bytes a = {static_cast<uint8_t>(n)};
    for (size_t i = from; i < from + n; i++) {
      a.insert(a.end(), survivors_[i].mac.begin(), survivors_[i].mac.end());
      a.push_back(static_cast<uint8_t>(survivors_[i].tuid >> 8));
      a.push_back(static_cast<uint8_t>(survivors_[i].tuid));
    }
    return a;
  }
  // keep: the saved records must stay for a resume (the table may be short of them).
  void fail_(const std::string &why, bool keep) {
    step_ = Step::failed;
    message_ = why;
    waiting_ = false;
    keep_saved_ = keep;
  }
  void finish_(const std::string &what) {
    step_ = Step::done;
    message_ = what;
    waiting_ = false;
    keep_saved_ = false;
  }
  void walked_() {
    const std::vector<TableRecord> &walk = walk_.records();
    if (step_ == Step::read_before) {
      before_ = walk;
      if (resuming_) {
        if (same_table(walk, survivors_)) return finish_("the interrupted removal had finished: nothing to do");
        step_ = Step::erase;  // the saved list is already on disk
        keep_saved_ = true;
        return;
      }
      const TableRecord *r = TableWalk::find(before_, mac_);
      if (!r) {
        if (!add_mac_.empty()) return fail_("the optimizer coming out is not in the table", false);
        return finish_("not in the table: nothing to remove");
      }
      id_ = r->id();
      for (const auto &rec : before_)
        if (rec.mac != mac_) survivors_.push_back(rec);
      if (!add_mac_.empty()) {
        if (add_mac_ == mac_) return fail_("the new optimizer is the one coming out", false);
        if (const TableRecord *dup = TableWalk::find(before_, add_mac_))
          return fail_("the new optimizer is already in the table at " + hex4_(dup->id()), false);
        survivors_.push_back({add_mac_, static_cast<uint16_t>(0x8000 | id_)});
      }
      // In id order, as the CCA wrote them back.
      std::sort(survivors_.begin(), survivors_.end(),
                [](const TableRecord &a, const TableRecord &b) { return a.id() < b.id(); });
      if (dry_run_) {
        step_ = Step::erase;
        return;
      }
      step_ = Step::save;
      return;
    }
    if (step_ == Step::read_empty) {
      if (walk.empty()) {
        keep_saved_ = true;
        written_ = 0;
        step_ = survivors_.empty() ? Step::read_after : Step::write;
        if (survivors_.empty()) return finish_(done_text_());
        return;
      }
      if (same_table(walk, before_) && !resuming_)
        return fail_("the erase didn't take (no answer, and the table is as it was): nothing changed", false);
      return fail_("the table didn't read empty after the erase; the records to write back are saved", true);
    }
    // After the writes: just the records that stay.
    if (!same_table(walk, survivors_))
      return fail_("the read back doesn't match the records that stay; they are saved and are written at the next start",
                   true);
    finish_(done_text_());
  }
  std::string done_text_() const {
    std::string n = std::to_string(survivors_.size());
    if (resuming_) return "finished the interrupted removal: " + n + " written back";
    if (!add_mac_.empty())
      return "replaced " + hex4_(id_) + "; the other " + std::to_string(survivors_.size() - 1) +
             " written back as they were, the new one waiting to join";
    return "removed " + hex4_(id_) + "; the other " + n + " written back as they were";
  }

  bool resuming_;
  Step step_ = Step::read_before;
  std::vector<TableRecord> survivors_;
  size_t written_ = 0;
  uint32_t empty_at_ms_ = 0;
  bool erase_sent_ = false;
  bool keep_saved_ = false;
};

// Replacing one: a removal whose rewrite also lists the new optimizer,
// provisional (bit 15 set) at the removed one's id, as the CCA writes an
// add into a whole-table rewrite (2026-11-09). One gap of under a second
// instead of a removal and then an add, and the new optimizer reports under
// the address the history already uses. The new MAC must not be listed.
class TableReplace : public TableRemove {
 public:
  TableReplace(const Bytes &old_mac, const Bytes &new_mac, bool dry_run) : TableRemove(old_mac, dry_run) {
    add_mac_ = new_mac;
  }
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
