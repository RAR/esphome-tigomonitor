#pragma once

// Adding an optimizer to the TAP's table, as the CCA did on the bench
// (2026-11-06, docs/superpowers/specs/2026-11-05-optimizer-commissioning-design.md):
// read the table (sub 0x26), write one record (sub 0x29) with bit 15 set,
// read it back, then hold the discovery window open until the TAP clears the
// bit, which it does when the optimizer joins. Unlike the CCA it doesn't
// erase the table first (the CCA's was empty; ours holds every optimizer on
// the roof), and sends none of the CCA's 0x22s with data or its writes after
// the join. See docs/superpowers/specs/2026-11-09-optimizer-table-edits-design.md.
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

// One add, start to finish: due() for the next request, on_reply() with what
// came back. It sends one request at a time, each marked table_edit, and
// sends the write at most once: a write with no ack is never repeated, and
// the read back decides whether it took.
class TableAdd : public RequestSource {
 public:
  enum class Step { read_before, write, read_after, joining, done, failed };
  static const uint32_t PAGE_WAIT_MS = 500;   // as Housekeeping's walks
  static const uint32_t WRITE_WAIT_MS = 2000;  // the TAP acks once the record is in flash
  static const int MAX_PAGES = 110;            // 1024 slots at about 12 a page
  static const uint32_t JOIN_CHECK_MS = 60000;
  static const uint32_t JOIN_TIMEOUT_MS = 30 * 60000;  // the bench's first join took 10 min

  // want_id 0: the lowest free id from 2, as the CCA chose on the bench.
  TableAdd(const Bytes &mac, uint16_t want_id, bool dry_run) : mac_(mac), want_id_(want_id), dry_run_(dry_run) {}

  Request due(uint32_t now_ms, uint8_t &seq) override {
    if (finished() || waiting_) return {};
    if (step_ == Step::joining) {
      if (static_cast<int32_t>(now_ms - joining_since_ms_) >= static_cast<int32_t>(JOIN_TIMEOUT_MS)) {
        fail_("not joined after " + std::to_string(JOIN_TIMEOUT_MS / 60000) +
              " min: it stays listed, not joined, and joins whenever the window is next open");
        return {};
      }
      if (!walking_ && static_cast<int32_t>(now_ms - next_check_ms_) < 0) return {};
    }
    if (step_ == Step::write) {
      Bytes args = {1};
      args.insert(args.end(), mac_.begin(), mac_.end());
      args.push_back(static_cast<uint8_t>(0x80 | id_ >> 8));
      args.push_back(static_cast<uint8_t>(id_));
      if (dry_run_) {
        dry_frames_.push_back(args);
        finish_("dry run: would list " + mac_hex(mac_) + " at " + hex4_(id_) + " (not joined)");
        return {};
      }
      seq = next_seq(seq);
      waiting_ = true;
      return tag_({CMD_TUNNEL, tunnel_request(0x29, seq, args), WRITE_WAIT_MS});
    }
    if (!walking_) {
      walking_ = true;
      page_index_ = 0;
      pages_ = 0;
      retried_ = false;
      walk_.clear();
    }
    seq = next_seq(seq);
    waiting_ = true;
    pages_++;
    return tag_({CMD_TUNNEL,
                 tunnel_request(0x26, seq, {static_cast<uint8_t>(page_index_ >> 8), static_cast<uint8_t>(page_index_)}),
                 PAGE_WAIT_MS});
  }

  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t now_ms) {
    if (!req.table_edit || !waiting_ || req.payload.size() < 5) return;
    waiting_ = false;
    TunnelReply t;
    bool parsed = ok && reply.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(reply.payload, t);
    if (req.payload[3] == 0x29) {
      if (parsed && t.sub == 0x2A && !t.data.empty()) {
        ack_ = t.data[0];
        if (ack_ != 0) return fail_(std::string("the TAP refused the write: ") + set_table_ack_text(ack_));
      }
      step_ = Step::read_after;  // no ack: the read back says whether it took
      return;
    }
    uint16_t start = 0;
    std::vector<TableRecord> page;
    if (!parsed || !parse_table_page(t, start, page)) {
      if (!retried_) {  // the same page once more
        retried_ = true;
        pages_--;
        return;
      }
      if (step_ == Step::joining) {  // try again at the next check
        walking_ = false;
        next_check_ms_ = now_ms + JOIN_CHECK_MS;
        return;
      }
      return fail_("the TAP didn't answer a read of its table");
    }
    retried_ = false;
    if (page.empty()) return walked_(now_ms);
    // The next page from one past this page's last id: the CCA's own next
    // index (first + count) on a table with no gaps, and right on one with.
    uint32_t next = static_cast<uint32_t>(page.back().id()) + 1;
    if (start < page_index_ || next <= page_index_ || pages_ >= MAX_PAGES)
      return fail_("the TAP's table read didn't add up");
    for (const auto &r : page)
      if (!find_(walk_, r.mac)) walk_.push_back(r);
    page_index_ = static_cast<uint16_t>(next);
  }

  Step step() const { return step_; }
  bool finished() const { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const { return step_ == Step::done; }
  bool dry_run() const { return dry_run_; }
  const Bytes &mac() const { return mac_; }
  uint16_t id() const { return id_; }  // 0 until chosen
  int ack() const { return ack_; }     // -1: none heard
  const std::string &message() const { return message_; }
  // True while the optimizer is listed and the job waits for it to join.
  bool wants_window() const { return step_ == Step::joining; }
  // A dry run's write, as the args after the seq.
  const std::vector<Bytes> &dry_frames() const { return dry_frames_; }
  const std::vector<TableRecord> &table() const { return before_; }
  static const char *step_name(Step s) {
    switch (s) {
      case Step::read_before: return "reading the table";
      case Step::write: return "writing";
      case Step::read_after: return "reading it back";
      case Step::joining: return "waiting for it to join";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }

 private:
  static std::string hex4_(uint16_t v) {
    char b[5];
    std::snprintf(b, sizeof(b), "%04X", v);
    return b;
  }
  Request tag_(Request r) {
    r.table_edit = true;
    return r;
  }
  void fail_(const std::string &why) {
    step_ = Step::failed;
    message_ = why;
    walking_ = waiting_ = false;
  }
  void finish_(const std::string &what) {
    step_ = Step::done;
    message_ = what;
    walking_ = waiting_ = false;
  }
  const TableRecord *find_(const std::vector<TableRecord> &t, const Bytes &mac) const {
    for (const auto &r : t)
      if (r.mac == mac) return &r;
    return nullptr;
  }
  void walked_(uint32_t now_ms) {
    walking_ = false;
    if (step_ == Step::read_before) {
      before_ = walk_;
      if (const TableRecord *r = find_(before_, mac_)) {
        id_ = r->id();
        return finish_("already in the table at " + hex4_(id_) + (r->joined() ? ", joined" : ", not joined yet") +
                       ": nothing to write; it joins while the window is open");
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
    if (step_ == Step::read_after) {
      // As before, plus the one record. Others may have joined meanwhile, so
      // records compare by MAC and id, not the 0x8000 bit.
      const TableRecord *added = find_(walk_, mac_);
      bool same = added && added->id() == id_ && walk_.size() == before_.size() + 1;
      for (const auto &r : before_) {
        const TableRecord *now = same ? find_(walk_, r.mac) : nullptr;
        if (!now || now->id() != r.id()) same = false;
      }
      if (!same)
        return fail_(added        ? "the read back doesn't match the table before the write"
                     : ack_ == -1 ? "no answer to the write, and the read back doesn't list it"
                                  : "the read back doesn't list it");
      if (added->joined()) return finish_("listed at " + hex4_(id_) + " and joined");
      step_ = Step::joining;
      joining_since_ms_ = now_ms;
      next_check_ms_ = now_ms + JOIN_CHECK_MS;
      message_ = "listed at " + hex4_(id_) + "; waiting for it to join";
      return;
    }
    // Joining: has the TAP cleared the bit?
    const TableRecord *r = find_(walk_, mac_);
    if (!r) return fail_("it's gone from the table");
    if (r->joined()) return finish_("joined at " + hex4_(r->id()));
    next_check_ms_ = now_ms + JOIN_CHECK_MS;
  }

  Bytes mac_;
  uint16_t want_id_;
  bool dry_run_;
  Step step_ = Step::read_before;
  uint16_t id_ = 0;
  int ack_ = -1;
  std::string message_;
  bool waiting_ = false;
  bool walking_ = false;
  bool retried_ = false;
  uint16_t page_index_ = 0;
  int pages_ = 0;
  std::vector<TableRecord> walk_, before_;
  uint32_t joining_since_ms_ = 0, next_check_ms_ = 0;
  std::vector<Bytes> dry_frames_;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
