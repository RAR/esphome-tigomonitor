#pragma once

// A TAP's radio settings, and giving a new TAP its own (Add a TAP's step).
//
// The block is what a CCA writes when it sets up a TAP (the 2026-11-06
// two-TAP capture, and the owner's analysis of the CCA's and the TAP's
// firmware, 2026-11-10 and 11-11): tunnel sub 0x0D with 01 00 and 36 bytes,
// answered by sub 0x0E with the block echoed and a status byte.
//
//   0-1   channel, big-endian; 11-26 (the TAP refuses anything else)
//   2-3   PAN, big-endian
//   4-13  map: four profile counts, 0, three bytes the TAP ignores (written
//         zero), and the profile's two parameters
//   14    SLT, the join threshold (0x5A)
//   15-30 the network key
//   31-34 optimization, big-endian (0x00000200)
//   35    SLm (0x3C)
//
// That's the 36-byte form, an H1.0007 TAP's on the bench (2026-11-11): its
// read, its write and the write's echo. The G8.59 in the 11-06 capture used
// a 46-byte form for its read (00 00), the CCA's write and the echo: 10 more
// bytes at 31-40, between the key and optimization (which then sits at 41,
// SLm at 45). Nobody has said yet what those 10 are. Its stored read (00 01)
// still answered with 36. So the job writes the form the TAP's own read
// came back in, and in the 46-byte form keeps the TAP's 10 bytes as they
// are rather than making them up.
//
// The write changes the TAP's RAM and retunes its radio at once; it saves
// nothing. The read of the stored copy (01 selector, 0D 00 01) is what marks
// every field to be saved, field by field, in the background. So a write is
// always followed by that read. Its reply comes from RAM, so only a power
// cycle shows what was saved.
//
// ProvisionJob does that to one TAP, named by its MAC (it asks the TAP at
// that address for its MAC first and stops unless it matches):
// classify_request() puts the write in Allow::provision, which may_send()
// allows only for a provisioning request, with "Allow changes to the TAPs"
// (master_table_writes) or master_bench on. No PV bracket, no 0x000E /
// 0x0E02, no table erase: the analysis found the TAP needs none of them, and
// the bench showed it. A reset addressed to that TAP alone is sent only when
// asked for; Add a TAP's step never asks.
//
// The 0x0E reply's status byte is the TAP's join-window state (1 while a
// window is open; bench, 2026-11-11), not a verdict on the write: the job
// takes 0, or 1 with an echo of exactly what was sent.
//
// The key never leaves this file in the clear: dry runs, messages and logs
// show it as hidden.
//
// Header-only and free of ESPHome, like table_edit.h;
// tests/host/tap_config_test.cpp covers it.

#include "table_edit.h"
#include "tigo_master.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace esphome {
namespace tigo_monitor {
namespace master {

static const uint8_t SUB_GATEWAY_CFG = 0x0D;
static const uint8_t SUB_GATEWAY_CFG_REPLY = 0x0E;
static const size_t RADIO_BLOCK_SIZE = 36;
static const size_t RADIO_BLOCK_WIDE = 46;
static const uint8_t CHANNEL_MIN = 11, CHANNEL_MAX = 26;

struct RadioBlock {
  uint16_t channel = 0;
  uint16_t pan = 0;
  std::array<uint8_t, 10> map{};
  uint8_t slt = 0x5A;
  std::array<uint8_t, 16> key{};
  uint32_t optimization = 0x00000200;
  uint8_t slm = 0x3C;
  // The 46-byte form, and its 10 unexplained bytes (31-40).
  bool wide = false;
  std::array<uint8_t, 10> extra{};
  // Not sent: how many optimizers the TAP's table holds, for sizing its map
  // (the CCA's burden).
  uint16_t burden = 0;
  // Not sent: keep the profile (map bytes 0-4 and 8-9) the TAP already has,
  // if it's a known one, instead of this block's.
  bool keep_tap_profile = false;
  // Not sent: keep the TAP's channel, PAN and key from its own read, so only
  // the map changes (changing a TAP's profile under its joined optimizers).
  bool keep_network = false;
  // Not sent: start at the read, without asking the TAP for its MAC first
  // (bench: does a lone 003A to a running TAP set the 0x0E status to 1?).
  bool skip_identify = false;

  Bytes encode() const {
    Bytes b;
    b.push_back(channel >> 8);
    b.push_back(channel & 0xFF);
    b.push_back(pan >> 8);
    b.push_back(pan & 0xFF);
    b.insert(b.end(), map.begin(), map.end());
    b.push_back(slt);
    b.insert(b.end(), key.begin(), key.end());
    if (wide) b.insert(b.end(), extra.begin(), extra.end());
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(optimization >> s));
    b.push_back(slm);
    return b;
  }
  // Either form, by its length; nothing else.
  static bool decode(const Bytes &b, RadioBlock &out) {
    if (b.size() != RADIO_BLOCK_SIZE && b.size() != RADIO_BLOCK_WIDE) return false;
    out.wide = b.size() == RADIO_BLOCK_WIDE;
    size_t o = out.wide ? 10 : 0;
    out.channel = static_cast<uint16_t>(b[0] << 8 | b[1]);
    out.pan = static_cast<uint16_t>(b[2] << 8 | b[3]);
    std::copy(b.begin() + 4, b.begin() + 14, out.map.begin());
    out.slt = b[14];
    std::copy(b.begin() + 15, b.begin() + 31, out.key.begin());
    out.extra = {};
    if (out.wide) std::copy(b.begin() + 31, b.begin() + 41, out.extra.begin());
    out.optimization = static_cast<uint32_t>(b[31 + o]) << 24 | b[32 + o] << 16 | b[33 + o] << 8 | b[34 + o];
    out.slm = b[35 + o];
    return true;
  }
  bool operator==(const RadioBlock &o) const { return encode() == o.encode(); }
  bool operator!=(const RadioBlock &o) const { return !(*this == o); }
  // The same channel, PAN, map, thresholds, key and optimization, whichever
  // form either is in: a 46-byte TAP's stored read answers with 36.
  bool same_settings(const RadioBlock &o) const {
    RadioBlock a = *this, b = o;
    a.wide = b.wide = false;
    return a == b;
  }

  // For a person: everything but the key.
  std::string describe() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "channel %u, PAN %04X, map %s, SLT %02X, key hidden, optimization %08X, SLm %02X",
                  channel, pan, hex_(Bytes(map.begin(), map.end())).c_str(), slt, static_cast<unsigned>(optimization),
                  slm);
    return buf;
  }

 private:
  static std::string hex_(const Bytes &b) {
    std::string s;
    char x[3];
    for (uint8_t v : b) {
      std::snprintf(x, sizeof(x), "%02x", v);
      s += x;
    }
    return s;
  }
};

// How many optimizers a profile is for, by its first count; 0 unknown.
inline int profile_capacity(uint8_t first) {
  return first == 12 ? 40 : first == 14 ? 75 : first == 20 ? 120 : first == 24 ? 300 : 0;
}

// The smallest map whose capacity covers burden optimizers, by its first
// count: the CCA's auto-size rule (re-chosen at each reset there).
inline uint8_t profile_for(int burden) {
  return burden <= 40 ? 12 : burden <= 75 ? 14 : burden <= 120 ? 20 : 24;
}

// The CCA's period for a map, in slots: what its Change Mesh Map page's
// "Calculate period speed" shows (owner's analysis of the CCA's firmware,
// 2026-11-11; it matched all eleven values read off a CCA). The levels are
// fan-outs, so P[i] is the positions at depth i; W counts each report and
// its relays; 28 and the star hunks (52 each) come out of a 792 budget.
// 0xFFFF when the CCA would call it invalid.
inline int map_period_slots(const std::array<uint8_t, 10> &m) {
  long p = 1, s = 0, w = 0, last = 0;
  int depth = 0;
  for (int i = 0; i < 8 && m[i]; i++) {
    p *= m[i];
    if (p > 0xFFFF) return 0xFFFF;
    s += p;
    w += (i + 1) * p;
    last = p;
    depth++;
  }
  if (!depth || s > 0xFFFF || w > 0xFFFF) return 0xFFFF;
  long d = 792L - m[8] - 52L * m[9] + last - s;
  if (d <= 0) return 0xFFFF;
  long raw = (static_cast<long>(w + 31) * 800 + d - 1) / d;  // ceil
  return static_cast<int>((raw * 65 + 99) / 100);            // ceil(raw * 0.65)
}

// The cycle the CCA rounds a period up to, in hyperslots (200 a second): the
// first step above it; 0 past the last.
inline int period_hyperslots(int slots) {
  for (int step : {200, 400, 600, 800, 1000, 1200, 2000, 2400, 3000, 4000, 6000})
    if (step > slots) return step;
  return 0;
}

// For a person: how often each optimizer would report on this map, as the
// CCA plans it. Not shown yet: writing the map alone didn't change a bench
// TAP's 2 s cycle (2026-11-12), so the period reaches it some other way.
inline std::string map_cycle_text(const std::array<uint8_t, 10> &m) {
  int hs = period_hyperslots(map_period_slots(m));
  if (!hs) return "";
  char b[32];
  if (hs % 200 == 0) std::snprintf(b, sizeof(b), "every %d s", hs / 200);
  else std::snprintf(b, sizeof(b), "every %.1f s", hs / 200.0);
  return b;
}

// The map for a profile by its first count (12, 14, 20 or 24: 40, 75, 120
// and 300 optimizers): the CCA's Change Mesh Map "12-2-2-1/28/1". Bytes 0-4
// are the counts and the 0 that ends them; 5-7 are left zero (the CCA's map
// parser never writes them; older CCA builds left stack bytes there, the
// "creeping" values in its writes); 8-9 are 28 and 1 for every size (the
// CCA's editor doesn't let you change them). From the owner's analysis of
// the CCA's firmware, 2026-11-11. False for an unknown first count.
inline bool map_for_profile(uint8_t first, std::array<uint8_t, 10> &out) {
  if (first != 12 && first != 14 && first != 20 && first != 24) return false;
  // 12, 14, 20: N-2-2-1; 24: 24-4-2-1. All /28/1.
  out = {first, static_cast<uint8_t>(first == 24 ? 4 : 2), 2, 1, 0, 0, 0, 0, 28, 1};
  return true;
}

// The shape the gate allows: a whole block on a channel the TAP takes.
inline bool is_radio_block_ok(const Bytes &block) {
  if ((block.size() != RADIO_BLOCK_SIZE && block.size() != RADIO_BLOCK_WIDE) || block[0] != 0) return false;
  return block[1] >= CHANNEL_MIN && block[1] <= CHANNEL_MAX;
}

// The PAN as a CCA without its own serial picks one: the TAP's MAC's low 16
// bits, stepped by 0x1000 past any PAN in use; never 0x0000 or 0xFFFF.
inline uint16_t pan_for(const Bytes &tap_mac, const std::set<uint16_t> &in_use) {
  uint16_t pan = tap_mac.size() >= 2 ? static_cast<uint16_t>(tap_mac[tap_mac.size() - 2] << 8 | tap_mac.back()) : 0x1000;
  for (int i = 0; i < 16; i++, pan = static_cast<uint16_t>(pan + 0x1000))
    if (pan != 0x0000 && pan != 0xFFFF && !in_use.count(pan)) return pan;
  return 0;
}

// The first channel from 11 up at least two away from every one in use.
inline uint16_t channel_for(const std::set<uint16_t> &in_use) {
  for (uint16_t c = CHANNEL_MIN; c <= CHANNEL_MAX; c++) {
    bool clear = true;
    for (uint16_t u : in_use)
      if (c + 1 >= u && c <= u + 1) clear = false;
    if (clear) return c;
  }
  return 0;
}

// A random key as the CCA draws one: no 00 or FF byte. draw() gives a byte.
template<typename Draw> std::array<uint8_t, 16> key_from(Draw draw) {
  std::array<uint8_t, 16> k{};
  for (auto &b : k) {
    do b = draw(); while (b == 0x00 || b == 0xFF);
  }
  return k;
}

// The write's args: 01 00 and the block.
inline Bytes radio_write_args(const RadioBlock &b) {
  Bytes a = {0x01, 0x00};
  Bytes blk = b.encode();
  a.insert(a.end(), blk.begin(), blk.end());
  return a;
}

// A frame as a dry run shows it, the key replaced.
inline std::string radio_write_text(const RadioBlock &b) {
  return "0D 01 00 + block (" + b.describe() + ")";
}

// Giving one TAP its radio settings: ask the TAP at that address for its
// MAC and stop unless it's the one named, read what it runs now, write ours,
// read the stored copy (the save), read it again, and, only if asked, reset
// that TAP alone. Then it's done; whether the TAP saved it is for a power
// cycle and a read to show.
class ProvisionJob : public TableJob {
 public:
  enum class Step { identify, read_before, write, save, check, reset, done, failed };
  static const uint32_t READ_WAIT_MS = 2000;
  static const uint32_t WRITE_WAIT_MS = 3000;
  static const uint32_t RESET_WAIT_MS = 2000;

  ProvisionJob(const Bytes &tap_mac, const RadioBlock &block, bool reset, bool dry_run)
      : TableJob(tap_mac, dry_run), block_(block), reset_(reset) {
    if (block.skip_identify) step_ = Step::read_before;
  }

  Request due(uint32_t, uint8_t &seq) override {
    if (finished() || waiting_) return {};
    // One turn to the master between the read and the write: its poll, as
    // the CCA sent (bench, 2026-11-11: does the write's status follow it?).
    if (turn_) {
      turn_ = false;
      return {};
    }
    switch (step_) {
      case Step::identify: {
        waiting_ = true;
        Request r{CMD_GET_MAC_NOW, {}, READ_WAIT_MS};
        r.table_edit = true;
        r.provision = true;
        return r;
      }
      case Step::read_before: return provision_(send_(SUB_GATEWAY_CFG, {0x00, 0x00}, READ_WAIT_MS, seq));
      case Step::write:
        if (dry_run_) {
          dry_text_.push_back(radio_write_text(block_));
          dry_text_.push_back("0D 00 01 (the save)");
          dry_text_.push_back("0D 00 00 (the check)");
          if (reset_) dry_text_.push_back("0010 37 24 92 66 to this TAP alone");
          finish_("dry run: would give the TAP " + block_.describe() + capacity_note_());
          return {};
        }
        return provision_(send_(SUB_GATEWAY_CFG, radio_write_args(block_), WRITE_WAIT_MS, seq));
      case Step::save: return provision_(send_(SUB_GATEWAY_CFG, {0x00, 0x01}, READ_WAIT_MS, seq));
      case Step::check: return provision_(send_(SUB_GATEWAY_CFG, {0x00, 0x00}, READ_WAIT_MS, seq));
      case Step::reset: {
        waiting_ = true;
        Request r{CMD_RESET, {0x37, 0x24, 0x92, 0x66}, RESET_WAIT_MS};
        r.table_edit = true;
        r.provision = true;
        return r;
      }
      default: return {};
    }
  }

  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t) override {
    if (req.cmd == CMD_GET_MAC_NOW) {
      if (!req.table_edit || !waiting_ || step_ != Step::identify) return;
      waiting_ = false;
      if (!ok || reply.cmd != CMD_GET_MAC_NOW + 1 || reply.payload.size() < 10)
        return fail_("the TAP didn't say which TAP it is (GET_MAC)");
      Bytes heard(reply.payload.begin(), reply.payload.begin() + 8);
      if (heard != mac_) return fail_("the TAP at that address is " + mac_hex(heard) + ", not " + mac_hex(mac_));
      step_ = Step::read_before;
      return;
    }
    if (req.cmd == CMD_RESET) {
      if (!req.table_edit || !waiting_) return;
      waiting_ = false;
      // A TAP restarting may not answer: the next poll shows it back.
      return finish_("given " + block_.describe() + status_note_() + "; saved and reset");
    }
    if (!take_(req)) return;
    TunnelReply t;
    // The block in either form, then a status byte.
    RadioBlock now;
    bool got = ok && reply.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(reply.payload, t) &&
               t.sub == SUB_GATEWAY_CFG_REPLY && !t.data.empty() &&
               RadioBlock::decode(Bytes(t.data.begin(), t.data.end() - 1), now);
    int status = got ? t.data.back() : -1;
    switch (step_) {
      case Step::read_before:
        if (!got) return fail_("the TAP didn't answer a read of its radio settings");
        before_block_ = now;
        had_before_ = true;
        // Write the form this TAP reads back in; the 10 unexplained bytes
        // of the 46-byte form stay what the TAP has.
        block_.wide = now.wide;
        block_.extra = now.extra;
        turn_ = true;
        if (block_.keep_network) {
          block_.channel = now.channel;
          block_.pan = now.pan;
          block_.key = now.key;
        }
        // Sized the CCA's way: a TAP keeps its own known map while that still
        // covers its table; otherwise the smallest that does.
        if (block_.keep_tap_profile) {
          int own = now.map[8] ? profile_capacity(now.map[0]) : 0;
          if (own && own >= block_.burden) {
            for (int i : {0, 1, 2, 3, 4, 8, 9}) block_.map[i] = now.map[i];
          } else {
            map_for_profile(profile_for(block_.burden), block_.map);
          }
        }
        block_.map[5] = block_.map[6] = block_.map[7] = 0;  // never written by the CCA
        step_ = Step::write;
        return;
      case Step::write:
        ack_ = status;
        if (!got) return fail_("no echo of the write; read the TAP before trying again");
        // 0, or 1 with an echo of exactly what was sent: 1 is the TAP's join
        // window being open (bench, 2026-11-11), and the write still applies.
        // Anything else stops here.
        if (status != 0 && status != 1)
          return fail_("the TAP answered the write with status " + std::to_string(status));
        if (now != block_) return fail_("the write's echo differs from what was sent");
        write_status_ = status;
        step_ = Step::save;
        return;
      case Step::save:
        if (!got) return fail_("the TAP didn't answer the save (0D 00 01); the write is in RAM only");
        if (!now.same_settings(block_)) return fail_("after the save the TAP runs something else");
        step_ = Step::check;
        return;
      case Step::check:
        if (!got) return fail_("the TAP didn't answer the check read");
        if (now != block_) return fail_("the check read differs from what was written");
        if (reset_) {
          step_ = Step::reset;
          return;
        }
        return finish_("given " + block_.describe() + capacity_note_() + status_note_() +
                       "; saved. Power-cycle the TAP and read it to see it kept");
      default: return;
    }
  }

  const char *kind() const override { return "provision"; }
  void abort(const std::string &why) override { fail_(why); }
  Step step() const { return step_; }
  bool finished() const override { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const override { return step_ == Step::done; }
  const char *step_text() const override { return step_name(step_); }
  static const char *step_name(Step s) {
    switch (s) {
      case Step::identify: return "asking which TAP it is";
      case Step::read_before: return "reading its radio settings";
      case Step::write: return "writing";
      case Step::save: return "saving";
      case Step::check: return "checking";
      case Step::reset: return "resetting it";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }
  const RadioBlock &block() const { return block_; }
  // What the TAP ran before, for the log: never shown with its key.
  bool had_before() const { return had_before_; }
  const RadioBlock &before() const { return before_block_; }
  // A dry run's frames as text, the key hidden.
  const std::vector<std::string> &dry_text() const { return dry_text_; }

 private:
  static Request provision_(Request r) {
    r.provision = true;
    return r;
  }
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

  std::string capacity_note_() const {
    // Not the cycle (map_cycle_text): on the bench a TAP moved to 14-2-2-1
    // still reported every 2 s, so the period reaches it some other way.
    int n = profile_capacity(block_.map[0]);
    return n ? ", for up to " + std::to_string(n) + " optimizers" : "";
  }
  std::string status_note_() const {
    return write_status_ ? " (the write answered status " + std::to_string(write_status_) + ", its echo exact)" : "";
  }

  RadioBlock block_;
  bool reset_;
  int write_status_ = 0;
  bool turn_ = false;  // a turn left to the master before the write
  Step step_ = Step::identify;
  RadioBlock before_block_;
  bool had_before_ = false;
  std::vector<std::string> dry_text_;
};

// ---- the report period: tunnel 0x22 -------------------------------------
//
// How often the TAP's optimizers report is not set by the map: on the
// bench, a TAP moved to 14-2-2-1 still reported every 2 s. It is a setting
// of its own (owner's analysis of the CCA's firmware, 2026-11-12), read
// with 0x22 00 <setting> and written with 0x22 01 <setting> <code>, each
// answered 0x23 <code>. There are two settings, 00 and 01; the CCA writes
// both the same, and only where the TAP's code differs from the period its
// map works out to. The code stands for a cycle in hyperslots (200 a
// second): 1 to 11 below. On the bench's H1 (2026-11-12) one write to
// setting 00 moved both readings, its reply held the code from before, and
// the optimizer's reports moved from 400 to 600 ticks apart. Our housekeeping has always sent both reads, as
// the CCA does; the roof TAP answers 3 on both, under the CCA (2026-10-27)
// and under us, and the bench's H1 answers 2. The CCA's channel scan sends
// the write form with code 12, which isn't a period and is never sent here.
static const uint8_t SUB_PERIOD = 0x22;
static const uint8_t SUB_PERIOD_REPLY = 0x23;

inline int period_code_hyperslots(int code) {
  static const int HS[] = {200, 400, 600, 800, 1000, 1200, 2000, 2400, 3000, 4000, 6000};
  return code >= 1 && code <= 11 ? HS[code - 1] : 0;
}
inline std::string period_code_text(int code) {
  int hs = period_code_hyperslots(code);
  return hs ? std::to_string(hs / 200) + " s" : "code " + std::to_string(code);
}

// Setting one TAP's report period: read both settings, write each that
// differs from the code asked for, and read both back. Marked provision,
// so it goes out under the same switch as the radio settings.
class PeriodJob : public TableJob {
 public:
  enum class Step { read, write, check, done, failed };
  static const uint32_t WAIT_MS = 2000;

  PeriodJob(const Bytes &tap_mac, uint8_t code, bool dry_run) : TableJob(tap_mac, dry_run), code_(code) {}

  Request due(uint32_t, uint8_t &seq) override {
    if (finished() || waiting_) return {};
    switch (step_) {
      case Step::read:
      case Step::check: return provision_(send_(SUB_PERIOD, {0x00, sel_}, WAIT_MS, seq));
      case Step::write: return provision_(send_(SUB_PERIOD, {0x01, sel_, code_}, WAIT_MS, seq));
      default: return {};
    }
  }

  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t) override {
    if (!take_(req)) return;
    TunnelReply t;
    int got = ok && reply.cmd == CMD_TUNNEL_REPLY && parse_tunnel_reply(reply.payload, t) && t.sub == SUB_PERIOD_REPLY &&
                      !t.data.empty()
                  ? t.data[0]
                  : -1;
    switch (step_) {
      case Step::read:
        if (got < 0) return fail_("the TAP didn't answer a read of its report period");
        before_[sel_] = got;
        if (sel_ == 0) {
          sel_ = 1;
          return;
        }
        if (before_[0] == code_ && before_[1] == code_)
          return finish_("the report period is already " + period_code_text(code_) + " on both: nothing written");
        if (dry_run_) {
          for (uint8_t s : {0, 1})
            if (before_[s] != code_) dry_frames_.push_back({SUB_PERIOD, 0x01, s, code_});
          return finish_("dry run: would set the report period to " + period_code_text(code_) + " on both (now " +
                         was_() + ")");
        }
        sel_ = before_[0] != code_ ? 0 : 1;
        step_ = Step::write;
        return;
      case Step::write:
        // The reply holds the code from before the write (bench, 2026-11-12):
        // it says the write arrived, and the reads after it say what took.
        if (got < 0) return fail_("no answer to the write; read the TAP before trying again");
        written_[sel_] = true;
        sel_ = 0;
        step_ = Step::check;
        return;
      case Step::check:
        if (got < 0) return fail_("the TAP didn't answer the check read");
        now_[sel_] = got;
        if (sel_ == 0) {
          sel_ = 1;
          return;
        }
        // One write moved both on the bench's H1; the other is written only
        // if it still differs.
        for (uint8_t s : {0, 1}) {
          if (now_[s] == code_) continue;
          if (written_[s])
            return fail_("after the write the TAP still reads " + period_code_text(now_[s]) + ", not " +
                         period_code_text(code_));
          sel_ = s;
          step_ = Step::write;
          return;
        }
        return finish_("report period " + period_code_text(code_) + " on both (was " + was_() + ")");
      default: return;
    }
  }

  const char *kind() const override { return "period"; }
  void abort(const std::string &why) override { fail_(why); }
  bool finished() const override { return step_ == Step::done || step_ == Step::failed; }
  bool ok() const override { return step_ == Step::done; }
  const char *step_text() const override {
    switch (step_) {
      case Step::read: return "reading its report period";
      case Step::write: return "writing";
      case Step::check: return "checking";
      case Step::done: return "done";
      case Step::failed: return "stopped";
    }
    return "";
  }
  uint8_t code() const { return code_; }
  int before(int sel) const { return sel == 0 || sel == 1 ? before_[sel] : -1; }

 private:
  static Request provision_(Request r) {
    r.provision = true;
    return r;
  }
  std::string was_() const { return period_code_text(before_[0]) + " and " + period_code_text(before_[1]); }
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

  uint8_t code_;
  uint8_t sel_ = 0;
  int before_[2] = {-1, -1};
  int now_[2] = {-1, -1};
  bool written_[2] = {false, false};
  Step step_ = Step::read;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
