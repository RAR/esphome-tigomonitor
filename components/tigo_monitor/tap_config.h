#pragma once

// A TAP's radio settings, and giving a spare TAP its own on the bench.
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
// ProvisionJob does that to one TAP, named by its MAC, and only on the
// bench: classify_request() puts the write in Allow::provision, which
// may_send() allows only for a provisioning request with master_bench on.
// No PV bracket, no 0x000E / 0x0E02, no table erase: the analysis found the
// TAP needs none of them, and the bench TAP is alone on its bus. A reset
// addressed to that TAP alone is sent only when asked for.
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
  // Not sent: ProvisionJob sets map bytes 5-7 to the TAP's own generation
  // plus one (wrapping to 1, never 0), read just before it writes.
  bool next_generation = false;
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

// The map for a profile by its first count (12, 14, 20 or 24: 40, 75, 120
// and 300 optimizers). Bytes 5-7 are a map generation the TAP doesn't check
// (the CCA's creep upward; zero is accepted). The last byte is the TAP's
// join-window dwell term: 01 as the CCA wrote it on 2026-11-11 (status 0);
// 03, from an older capture, is applied but answered with status 1. Only 12,
// 14 and 20 have known parameters; false for the rest.
inline bool map_for_profile(uint8_t first, std::array<uint8_t, 10> &out, uint32_t generation = 0) {
  if (first != 12 && first != 14 && first != 20) return false;
  out = {first, 2, 2, 1, 0, static_cast<uint8_t>(generation >> 16), static_cast<uint8_t>(generation >> 8),
         static_cast<uint8_t>(generation), 28, 1};
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
          finish_("dry run: would give the TAP " + block_.describe());
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
        if (block_.next_generation) {
          uint32_t g = (static_cast<uint32_t>(now.map[5]) << 16 | now.map[6] << 8 | now.map[7]) + 1;
          if (g > 0xFFFFFF) g = 1;
          block_.map[5] = static_cast<uint8_t>(g >> 16);
          block_.map[6] = static_cast<uint8_t>(g >> 8);
          block_.map[7] = static_cast<uint8_t>(g);
        }
        step_ = Step::write;
        return;
      case Step::write:
        ack_ = status;
        if (!got) return fail_("no echo of the write; read the TAP before trying again");
        // 0, or 1 with an echo of exactly what was sent: the bench H1.0007
        // answers a whole 36-byte write with 1 and applies it (2026-11-11);
        // what 1 means isn't known yet. Anything else stops here.
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
        return finish_("given " + block_.describe() + status_note_() +
                       "; saved. Power-cycle the TAP and read it to see it kept");
      default: return;
    }
  }

  const char *kind() const override { return "provision"; }
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

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
