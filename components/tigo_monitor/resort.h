#pragma once

// The re-sort: every TAP gets a new network, so every optimizer looks for a
// TAP again and joins the one it hears that lists it
// (docs/superpowers/specs/2026-11-23-tap-resort-design.md).
//
// An optimizer stays on its TAP while that TAP's network (PAN and key) stays
// the same; taking it off the TAP's list doesn't move it (the analyst,
// 2026-11-22). The CCA's 11-15 rediscovery, in the owner's capture (keys
// and PANs read as labels only): PV off to both TAPs; a GatewayCfg write to
// each, its channel kept, a new PAN each and one new key for both; PV on, a
// second later; the stored reads; 0xE1 to each naming the other's new
// network; a 2B erase of each table and every optimizer listed (0x8000) on
// both; windows ending 01 01. All 40 rejoined in about 12 minutes, each at
// its own address. When the window closed it trimmed each TAP to the
// optimizers that had joined it; those that hadn't stayed listed on both.
//
// Pi only: the ESP32 sends no 0xE1, so it never re-sorts.

#include "table_edit.h"
#include "tap_config.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

// One TAP as it runs before the re-sort: its last-read radio settings, and
// whether it saves only with the gateway Store (a G8).
struct ResortTap {
  uint16_t tap = 0;
  RadioBlock now;
  bool saves_with_store = false;
};

// The blocks each TAP gets, in taps' order: its own block kept whole but for
// the PAN and the key. Each PAN is pan_for() from two drawn bytes, past every
// TAP's current PAN and every one already picked; one key, drawn once, for
// all. Empty if some TAP finds no free PAN.
template<typename Draw> std::vector<RadioBlock> resort_networks(const std::vector<ResortTap> &taps, Draw draw) {
  std::set<uint16_t> in_use;
  for (const auto &t : taps) in_use.insert(t.now.pan);
  std::array<uint8_t, 16> key = key_from(draw);
  std::vector<RadioBlock> out;
  for (const auto &t : taps) {
    Bytes seed = {draw(), draw()};
    uint16_t pan = pan_for(seed, in_use);
    if (!pan) return {};
    in_use.insert(pan);
    RadioBlock b = t.now;
    b.pan = pan;
    b.key = key;
    out.push_back(b);
  }
  return out;
}

inline std::string resort_hex4_(uint16_t v) {
  char b[5];
  std::snprintf(b, sizeof(b), "%04X", v);
  return b;
}

// Every optimizer on any TAP's table, once each, listed (0x8000) at its own
// id, in id order. False, saying why, if two TAPs list one id under
// different MACs, if one MAC is listed at two ids, or if nothing is listed.
inline bool resort_list(const std::map<uint16_t, std::vector<TableRecord>> &tables, std::vector<TableRecord> &out,
                        std::string &why) {
  out.clear();
  std::map<uint16_t, std::pair<Bytes, uint16_t>> by_id;  // id -> (MAC, the TAP that listed it first)
  std::map<Bytes, std::pair<uint16_t, uint16_t>> by_mac;  // MAC -> (id, TAP)
  for (const auto &kv : tables) {
    for (const auto &r : kv.second) {
      auto i = by_id.find(r.id());
      if (i != by_id.end() && i->second.first != r.mac) {
        why = "TAP " + resort_hex4_(i->second.second) + " lists " + resort_hex4_(r.id()) + " as " +
              mac_hex(i->second.first) + " and TAP " + resort_hex4_(kv.first) + " as " + mac_hex(r.mac);
        return false;
      }
      auto m = by_mac.find(r.mac);
      if (m != by_mac.end() && m->second.first != r.id()) {
        why = mac_hex(r.mac) + " is listed at " + resort_hex4_(m->second.first) + " on TAP " +
              resort_hex4_(m->second.second) + " and at " + resort_hex4_(r.id()) + " on TAP " + resort_hex4_(kv.first);
        return false;
      }
      if (i == by_id.end()) by_id[r.id()] = {r.mac, kv.first};
      if (m == by_mac.end()) by_mac[r.mac] = {r.id(), kv.first};
    }
  }
  if (by_id.empty()) {
    why = "no TAP lists any optimizer";
    return false;
  }
  for (const auto &kv : by_id) out.push_back({kv.second.first, static_cast<uint16_t>(0x8000 | kv.first)});
  why.clear();
  return true;
}

// For each TAP, the MACs it lists that another TAP lists as joined: what a
// trim takes off it. A MAC joined on two TAPs is taken off neither.
inline std::map<uint16_t, std::vector<Bytes>> resort_trim(const std::map<uint16_t, std::vector<TableRecord>> &tables) {
  std::map<Bytes, std::vector<uint16_t>> joined_on;
  for (const auto &kv : tables)
    for (const auto &r : kv.second)
      if (r.joined()) joined_on[r.mac].push_back(kv.first);
  std::map<uint16_t, std::vector<Bytes>> out;
  for (const auto &kv : tables) {
    out[kv.first];
    for (const auto &r : kv.second) {
      auto j = joined_on.find(r.mac);
      if (j == joined_on.end() || j->second.size() != 1 || j->second[0] == kv.first) continue;
      out[kv.first].push_back(r.mac);
    }
  }
  return out;
}

// The waited-for MACs joined on no TAP, in waited's order, each with the id
// from the first table that lists it (0 if none does).
inline std::vector<TableRecord> resort_missing(const std::map<uint16_t, std::vector<TableRecord>> &tables,
                                               const std::vector<Bytes> &waited) {
  std::vector<TableRecord> out;
  for (const auto &m : waited) {
    bool joined = false;
    const TableRecord *listed = nullptr;
    for (const auto &kv : tables) {
      const TableRecord *r = TableWalk::find(kv.second, m);
      if (!r) continue;
      if (r->joined()) joined = true;
      if (!listed) listed = r;
    }
    if (!joined) out.push_back(listed ? *listed : TableRecord{m, 0});
  }
  return out;
}


// The re-sort's frames, in the CCA's order, on every TAP: each step runs on
// every TAP, in taps' order, before the next step starts. due() gives a
// request only to the TAP whose turn it is, so the master polls the others in
// between. Then:
//   read_tables  each TAP's table (TableWalk); the list is resort_list's
//   save         the caller writes the old networks to disk (not a put-back)
//   pv_off       0B00 00 to each TAP; its reply isn't needed
//   write        0D 01 00 + the TAP's new block; the echo must match, status 0 or 1
//   pv_on        0B00 01 to each TAP, whatever failed after the first PV off
//   save_read    0D 00 01: an H1 saves on it, and its stored copy must match;
//                a G8's is still the old one until the Store
//   check        0D 00 00 must read the new block back
//   store        a G8's Store (0012 37 24 92 66), as the provision job saves one
//   store_read   then its stored copy must match
//   known        0xE1 naming every other TAP's new network, status 0
//   erase        2B BA BE, ack 0
//   list         every optimizer, listed, 10 to a 0x29, ack 0
//   window       2D, 900 s, ending 01 01 (known networks on)
// A failure after the first PV off sends PV on to every TAP, then stops. No
// message names a key or a PAN.
class ResortJob {
 public:
  enum class Step { read_tables, save, pv_off, write, pv_on, save_read, check, store, store_read, known, erase, list,
                    window, done, failed };
  static const uint32_t PV_WAIT_MS = 1000;
  static const uint32_t READ_WAIT_MS = 2000;
  static const uint32_t WRITE_WAIT_MS = 3000;
  static const uint32_t TABLE_WAIT_MS = 3000;
  static const uint32_t WINDOW_WAIT_MS = 1000;

  ResortJob(std::vector<ResortTap> taps, std::vector<RadioBlock> target, bool dry_run, bool put_back)
      : taps_(std::move(taps)), target_(std::move(target)), dry_run_(dry_run), put_back_(put_back) {
    if (taps_.size() < 2) {
      fail_now_("a re-sort needs at least two TAPs");
    } else if (taps_.size() - 1 > KNOWN_NETWORKS_MAX) {
      fail_now_("more than " + std::to_string(KNOWN_NETWORKS_MAX + 1) + " TAPs: 0xE1 can't name them all");
    } else if (target_.size() != taps_.size()) {
      fail_now_("no new network for every TAP");
    } else {
      walk_.start();
    }
  }

  Request due(uint16_t tap, uint32_t, uint8_t &seq) {
    if (finished() || waiting_ || step_ == Step::save || i_ >= taps_.size() || taps_[i_].tap != tap) return {};
    const RadioBlock &b = target_[i_];
    switch (step_) {
      case Step::read_tables: return mark_(walk_.request(seq));
      case Step::pv_off:
      case Step::pv_on: {
        Request r{CMD_SESSION, {static_cast<uint8_t>(step_ == Step::pv_off ? 0x00 : 0x01)}, PV_WAIT_MS};
        r.resort = true;
        if (step_ == Step::pv_off) bracket_open_ = true;
        return mark_(r);
      }
      case Step::write: return provision_(tunnel_(SUB_GATEWAY_CFG, radio_write_args(b), WRITE_WAIT_MS, seq));
      case Step::save_read:
      case Step::store_read: return provision_(tunnel_(SUB_GATEWAY_CFG, {0x00, 0x01}, READ_WAIT_MS, seq));
      case Step::check: return provision_(tunnel_(SUB_GATEWAY_CFG, {0x00, 0x00}, READ_WAIT_MS, seq));
      case Step::store: {
        Request r{CMD_STORE, {0x37, 0x24, 0x92, 0x66}, WRITE_WAIT_MS};
        return provision_(r);
      }
      case Step::known: return tunnel_(SUB_KNOWN_NETWORKS, known_networks_args(others_(i_)), READ_WAIT_MS, seq);
      case Step::erase: return tunnel_(SUB_ERASE_FLASH, {0xBA, 0xBE}, TABLE_WAIT_MS, seq);
      case Step::list: return tunnel_(0x29, chunk_(), TABLE_WAIT_MS, seq);
      case Step::window: {
        seq = next_seq(seq);
        waiting_ = true;
        return {CMD_TUNNEL,
                tunnel_request(SUB_DISCOVERY_START, seq,
                               discovery_renewal_args(static_cast<uint16_t>(records_.size()), true)),
                WINDOW_WAIT_MS};
      }
      default: return {};
    }
  }

  void on_reply(uint16_t tap, const Request &req, bool ok, const Frame &reply, uint32_t) {
    if (!waiting_ || finished() || i_ >= taps_.size() || taps_[i_].tap != tap) return;
    if (!req.resort && !req.table_edit && !(req.cmd == CMD_TUNNEL && step_ == Step::window)) return;
    waiting_ = false;
    const ResortTap &t = taps_[i_];
    const RadioBlock &want = target_[i_];
    TunnelReply tr;
    bool tunnel = ok && reply.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(reply.payload, tr);
    RadioBlock got;
    bool block = tunnel && tr.sub == SUB_GATEWAY_CFG_REPLY && !tr.data.empty() &&
                 RadioBlock::decode(Bytes(tr.data.begin(), tr.data.end() - 1), got);
    int status = block ? tr.data.back() : -1;
    switch (step_) {
      case Step::read_tables:
        switch (walk_.on_reply(ok, reply)) {
          case TableWalk::more: return;
          case TableWalk::failed: return fail_(t.tap, "couldn't read TAP " + resort_hex4_(t.tap) + "'s table");
          case TableWalk::done:
            tables_[t.tap] = walk_.records();
            if (i_ + 1 < taps_.size()) {
              i_++;
              walk_.start();
              return;
            }
            return listed_();
        }
        return;
      case Step::pv_off:
      case Step::pv_on: return advance_();  // the 0B01 isn't needed
      case Step::write:
        if (!block) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " didn't echo its network write");
        if (status != 0 && status != 1)
          return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " answered its network write with status " +
                                  std::to_string(status));
        if (got != want) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + "'s echo differs from what was written");
        return advance_();
      case Step::save_read:
        if (!block) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " didn't answer the save (0D 00 01)");
        // A G8's stored copy changes only with the Store, after the check.
        if (!t.saves_with_store && !got.same_settings(want))
          return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + "'s stored copy differs after the save: " +
                                  want.differences(got));
        return advance_();
      case Step::check:
        if (!block) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " didn't answer the check read");
        if (got != want) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + "'s check read differs from what was written");
        return advance_();
      case Step::store:
        if (!ok || reply.cmd != CMD_STORE + 1) return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " didn't answer the Store");
        return advance_();
      case Step::store_read:
        if (!block || !got.same_settings(want))
          return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + "'s stored copy differs after the Store");
        return advance_();
      case Step::known: {
        int st = known_networks_status(reply);
        if (st != 0)
          return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + " was told the others' networks: " +
                                  known_networks_status_text(st));
        return advance_();
      }
      case Step::erase:
      case Step::list: {
        uint8_t sub = step_ == Step::erase ? 0x2C : 0x2A;
        int ack = tunnel && tr.sub == sub && !tr.data.empty() ? tr.data[0] : -1;
        if (ack != 0)
          return fail_(t.tap, "TAP " + resort_hex4_(t.tap) + (step_ == Step::erase ? " didn't erase its table" : " didn't take the list") +
                                  (ack < 0 ? " (no answer)" : " (" + std::string(step_ == Step::list ? set_table_ack_text(ack) : "refused") + ")"));
        if (step_ == Step::list) {
          list_off_ += chunk_size_();
          if (list_off_ < records_.size()) return;
          list_off_ = 0;
        }
        return advance_();
      }
      case Step::window: return advance_();  // the keeper renews it anyway while the re-sort waits
      default: return;
    }
  }

  // The gate refused one of its frames.
  void abort(const std::string &why) {
    waiting_ = false;
    fail_(i_ < taps_.size() ? taps_[i_].tap : 0, why);
  }
  bool wants_save() const { return step_ == Step::save; }
  void saved(bool ok) {
    if (step_ != Step::save) return;
    if (!ok) return fail_now_("couldn't save the old networks; nothing was changed");
    start_(Step::pv_off);
  }
  bool finished() const { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const { return step_ == Step::done; }
  bool dry_run() const { return dry_run_; }
  bool put_back() const { return put_back_; }
  // Between the first PV off and the last PV on: a restart then owes every
  // TAP a PV on.
  bool pv_off_sent() const { return bracket_open_; }
  Step step() const { return step_; }
  const char *step_text() const { return step_name(step_); }
  static const char *step_name(Step s) {
    switch (s) {
      case Step::read_tables: return "reading the tables";
      case Step::save: return "saving the old networks";
      case Step::pv_off: return "PV off";
      case Step::write: return "writing the networks";
      case Step::pv_on: return "PV on";
      case Step::save_read: return "saving";
      case Step::check: return "checking";
      case Step::store: return "storing";
      case Step::store_read: return "reading the stored copies";
      case Step::known: return "telling each TAP the others' networks";
      case Step::erase: return "erasing the tables";
      case Step::list: return "listing every optimizer on every TAP";
      case Step::window: return "opening the windows";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }
  const std::string &message() const { return message_; }
  const std::vector<TableRecord> &records() const { return records_; }
  const std::map<uint16_t, std::vector<TableRecord>> &tables() const { return tables_; }  // as read first
  const std::vector<std::string> &dry_text() const { return dry_text_; }
  uint16_t failed_tap() const { return failed_tap_; }
  const std::vector<ResortTap> &taps() const { return taps_; }

 private:
  Request mark_(Request r) {
    waiting_ = true;
    return r;
  }
  Request tunnel_(uint8_t sub, const Bytes &args, uint32_t wait_ms, uint8_t &seq) {
    seq = next_seq(seq);
    waiting_ = true;
    Request r{CMD_TUNNEL, tunnel_request(sub, seq, args), wait_ms};
    r.table_edit = true;
    return r;
  }
  Request provision_(Request r) {
    waiting_ = true;
    r.table_edit = true;
    r.provision = true;
    return r;
  }
  std::vector<KnownNetwork> others_(size_t self) const {
    std::vector<KnownNetwork> o;
    for (size_t k = 0; k < taps_.size(); k++)
      if (k != self) {
        KnownNetwork n;
        n.channel = static_cast<uint8_t>(target_[k].channel);
        n.pan = target_[k].pan;
        n.key = target_[k].key;
        o.push_back(n);
      }
    return o;
  }
  size_t chunk_size_() const {
    size_t left = records_.size() - list_off_;
    return left < SET_TABLE_MAX_RECORDS ? left : SET_TABLE_MAX_RECORDS;
  }
  Bytes chunk_() const {
    size_t n = chunk_size_();
    Bytes a = {static_cast<uint8_t>(n)};
    for (size_t k = list_off_; k < list_off_ + n; k++) {
      a.insert(a.end(), records_[k].mac.begin(), records_[k].mac.end());
      a.push_back(static_cast<uint8_t>(records_[k].tuid >> 8));
      a.push_back(static_cast<uint8_t>(records_[k].tuid));
    }
    return a;
  }
  bool skipped_(size_t k) const {
    return (step_ == Step::store || step_ == Step::store_read) && !taps_[k].saves_with_store;
  }
  void start_(Step s) {
    step_ = s;
    i_ = 0;
    while (i_ < taps_.size() && skipped_(i_)) i_++;
    if (i_ >= taps_.size()) next_();
  }
  void advance_() {
    i_++;
    while (i_ < taps_.size() && skipped_(i_)) i_++;
    if (i_ >= taps_.size()) next_();
  }
  void next_() {
    switch (step_) {
      case Step::pv_off: return start_(Step::write);
      case Step::write: return start_(Step::pv_on);
      case Step::pv_on:
        bracket_open_ = false;
        if (!pending_fail_.empty()) {
          step_ = Step::failed;
          message_ = pending_fail_;
          return;
        }
        return start_(Step::save_read);
      case Step::save_read: return start_(Step::check);
      case Step::check: return start_(Step::store);
      case Step::store: return start_(Step::store_read);
      case Step::store_read: return start_(Step::known);
      case Step::known: return start_(Step::erase);
      case Step::erase: return start_(Step::list);
      case Step::list: return start_(Step::window);
      case Step::window:
        step_ = Step::done;
        message_ = std::string(put_back_ ? "the old networks are back on " : "new networks on ") +
                   std::to_string(taps_.size()) + " TAPs and all " + std::to_string(records_.size()) +
                   " optimizers listed on each; waiting for them to join";
        return;
      default: return;
    }
  }
  // The tables are read: the list, then a dry run's account or the save.
  void listed_() {
    std::string why;
    if (!resort_list(tables_, records_, why)) return fail_now_(why);
    if (dry_run_) {
      describe_();
      step_ = Step::done;
      message_ = std::string("dry run: would give ") + std::to_string(taps_.size()) + " TAPs " +
                 (put_back_ ? "their old networks back" : "new networks") + " and list all " +
                 std::to_string(records_.size()) + " optimizers on each";
      return;
    }
    if (put_back_) return start_(Step::pv_off);
    step_ = Step::save;
  }
  void describe_() {
    std::string n = std::to_string(taps_.size());
    dry_text_.push_back("PV off (0B00 00) to " + n + " TAPs");
    for (size_t k = 0; k < taps_.size(); k++)
      dry_text_.push_back("0D 01 00 + block (channel " + std::to_string(target_[k].channel) +
                          (put_back_ ? ", its old network ID, the old key" : ", a new network ID, the new shared key") +
                          ") to TAP " + resort_hex4_(taps_[k].tap));
    dry_text_.push_back("PV on (0B00 01) to " + n + " TAPs");
    dry_text_.push_back("0D 00 01 (the save) and 0D 00 00 (the check) to each TAP");
    for (const auto &t : taps_)
      if (t.saves_with_store) dry_text_.push_back("0012 (the Store), then 0D 00 01, to TAP " + resort_hex4_(t.tap));
    dry_text_.push_back("0xE1 to each TAP, naming the " + std::to_string(taps_.size() - 1) + " others");
    dry_text_.push_back("2B erase, then " + std::to_string(records_.size()) + " listed (not joined) on each TAP");
    dry_text_.push_back("2D window ending 01 01 on each TAP");
  }
  void fail_now_(const std::string &why) {
    step_ = Step::failed;
    message_ = why;
  }
  void fail_(uint16_t tap, const std::string &why) {
    if (!failed_tap_) failed_tap_ = tap;
    if (bracket_open_ && step_ != Step::pv_on) {
      pending_fail_ = why;
      return start_(Step::pv_on);
    }
    if (step_ == Step::pv_on) {  // a PV on itself refused: carry on to the others
      if (pending_fail_.empty()) pending_fail_ = why;
      return advance_();
    }
    fail_now_(why);
  }

  std::vector<ResortTap> taps_;
  std::vector<RadioBlock> target_;
  bool dry_run_, put_back_;
  Step step_ = Step::read_tables;
  size_t i_ = 0;
  bool waiting_ = false;
  bool bracket_open_ = false;
  TableWalk walk_;
  std::map<uint16_t, std::vector<TableRecord>> tables_;
  std::vector<TableRecord> records_;
  size_t list_off_ = 0;
  std::string message_, pending_fail_;
  std::vector<std::string> dry_text_;
  uint16_t failed_tap_ = 0;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
