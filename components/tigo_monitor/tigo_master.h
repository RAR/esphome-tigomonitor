#pragma once

// Frames a bus master sends to the Tigo TAP, and the replies it reads back.
//
// The CCA is the bus master on a stock install; tigo_protocol.h decodes what
// it and the TAP say to each other. This is the other direction, for taking
// the CCA's place: building requests and parsing the TAP's answers. What the
// CCA sends was taken from a capture of the reference rig's bus (see
// docs/superpowers/specs/2026-10-26-cca-replacement-design.md):
//   - 0x0148 telemetry polls, answered by 0x0149 power frames, and
//   - a tunnel, 0x0B0F with replies on 0x0B10, whose payload is
//     00 00 00 <sub> <seq> <args>; the reply is 00 0d|0e 00 <sub+1> <seq> <data>.
//     Sub 0x2E reads the TAP's run lease, sub 0x2D renews it.
//
// Header-only and free of ESPHome, like tigo_protocol.h, so both builds can
// use it and tests/host/master_protocol_test.cpp covers it.

#include "tigo_protocol.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

using Bytes = std::vector<uint8_t>;

// ---- framing ----------------------------------------------------------------

// The bytes the gateway escapes inside a frame, by escape index: 7E <index>.
static const uint8_t ESCAPED[] = {0x7E, 0x24, 0x23, 0x25, 0xA4, 0xA3, 0xA5};

inline void append_escaped(Bytes &out, uint8_t byte) {
  for (uint8_t i = 0; i < sizeof(ESCAPED); i++) {
    if (byte == ESCAPED[i]) {
      out.push_back(0x7E);
      out.push_back(i);
      return;
    }
  }
  out.push_back(byte);
}

// The whole frame as it goes on the wire: the 00 FF FF preamble the CCA sends
// before every request, 7E 07, the escaped body (DEST, CMD, payload, CRC), 7E 08.
inline Bytes encode_frame(uint16_t dest, uint16_t cmd, const Bytes &payload) {
  Bytes body = {static_cast<uint8_t>(dest >> 8), static_cast<uint8_t>(dest), static_cast<uint8_t>(cmd >> 8),
                static_cast<uint8_t>(cmd)};
  body.insert(body.end(), payload.begin(), payload.end());
  // crc16 returns the CRC byte-swapped, so writing its high byte first puts
  // the CRC's low byte first on the wire, as the gateway expects.
  uint16_t crc = protocol::crc16(body.data(), body.size());
  body.push_back(static_cast<uint8_t>(crc >> 8));
  body.push_back(static_cast<uint8_t>(crc));
  Bytes out = {0x00, 0xFF, 0xFF, 0x7E, 0x07};
  for (uint8_t b : body) append_escaped(out, b);
  out.push_back(0x7E);
  out.push_back(0x08);
  return out;
}

struct Frame {
  uint16_t dest = 0;
  uint16_t cmd = 0;
  Bytes payload;
  uint32_t wait_ms = 0;  // how long to wait for the reply; 0 = the usual
};

// escaped_body: the bytes between 7E 07 and 7E 08, as FrameSplitter yields
// them. False for a frame too short to hold DEST, CMD and CRC, or a bad CRC.
inline bool decode_frame(const std::string &escaped_body, Frame &out) {
  std::string body = protocol::unescape(escaped_body);
  if (body.size() < 6 || !protocol::checksum_ok(body)) return false;
  auto at = [&](size_t i) { return static_cast<uint8_t>(body[i]); };
  out.dest = static_cast<uint16_t>(at(0) << 8 | at(1));
  out.cmd = static_cast<uint16_t>(at(2) << 8 | at(3));
  out.payload.assign(body.begin() + 4, body.end() - 2);
  return true;
}

// ---- the tunnel ------------------------------------------------------------

static const uint16_t CMD_POLL = 0x0148;
static const uint16_t CMD_POLL_REPLY = 0x0149;
static const uint16_t CMD_TUNNEL = 0x0B0F;
static const uint16_t CMD_TUNNEL_REPLY = 0x0B10;
static const uint8_t SUB_LEASE_RENEW = 0x2D;
static const uint8_t SUB_LEASE_CHECK = 0x2E;
static const uint8_t SUB_LEASE_REPLY = 0x2F;
// Sub 0x41 and command 0x0B00: the CCA's housekeeping (Housekeeping, below).
static const uint8_t SUB_HOUSEKEEPING = 0x41;
static const uint16_t CMD_SESSION = 0x0B00;

inline Bytes tunnel_request(uint8_t sub, uint8_t seq, const Bytes &args) {
  Bytes out = {0x00, 0x00, 0x00, sub, seq};
  out.insert(out.end(), args.begin(), args.end());
  return out;
}

struct TunnelReply {
  uint8_t sub = 0;
  uint8_t seq = 0;
  Bytes data;
};

inline bool parse_tunnel_reply(const Bytes &payload, TunnelReply &out) {
  if (payload.size() < 5) return false;
  out.sub = payload[3];
  out.seq = payload[4];
  out.data.assign(payload.begin() + 5, payload.end());
  return true;
}

// ---- the run lease ---------------------------------------------------------

// The 0x2F reply's data: state (01 running, 00 lapsed), seconds left (BE),
// then three counts. On the rig they read 44/40/44 while running and 0/40/44
// at lapse; the CCA's renewal carried 44 with the first at 0, so the node
// count is the third.
struct Lease {
  bool running = false;
  uint16_t remaining_s = 0;
  uint16_t node_count = 0;
};

inline bool parse_lease(const TunnelReply &r, Lease &out) {
  if (r.sub != SUB_LEASE_REPLY || r.data.size() < 9) return false;
  out.running = r.data[0] == 0x01;
  out.remaining_s = static_cast<uint16_t>(r.data[1] << 8 | r.data[2]);
  out.node_count = static_cast<uint16_t>(r.data[7] << 8 | r.data[8]);
  return true;
}

// The CCA's renewal, byte for byte: BA BE, 02, 900 s, the node count, 01 00.
// What BA BE, 02 and 01 00 mean isn't known; they are copied, not composed.
inline Bytes lease_renewal_args(uint16_t node_count) {
  return {0xBA, 0xBE, 0x02, 0x03, 0x84, static_cast<uint8_t>(node_count >> 8), static_cast<uint8_t>(node_count),
          0x01, 0x00};
}

// What to do about the lease, as the CCA does it: read it every 20 s (or
// just after it should run out, if that's sooner) and renew only once a
// reading says 0 s are left. A master that restarts checks first, and one
// that stops calling due() lets the lease lapse.
//
// Guards for frames and loops the CCA never showed:
//   - no renewal carries a node count of 0;
//   - "not running" with time left is not a reason to renew; only 0 s is;
//   - after RENEW_LIMIT renewals with no reading showing time on the lease,
//     it stops renewing and stuck() says so, until a reading shows time.
// A check with no reply is retried a second later, up to CHECK_RETRIES
// times, so one lost reply doesn't leave a lapsed lease for a whole period.
struct LeaseAction {
  enum Kind { none, check, renew } kind = none;
  uint16_t node_count = 0;
};

class LeaseKeeper {
 public:
  static const uint32_t CHECK_EVERY_MS = 20000;
  static const uint32_t RENEW_RETRY_MS = 1000;
  static const uint32_t EXPIRY_MARGIN_MS = 500;
  static const uint32_t CHECK_RETRY_MS = 1000;
  static const uint32_t CHECK_RETRIES = 3;
  static const uint32_t RENEW_LIMIT = 3;

  // What to send now. Returning an action counts it as sent.
  LeaseAction due(uint32_t now_ms) {
    LeaseAction a;
    bool out = ever_read_ && last_.remaining_s == 0 && last_.node_count > 0;
    if (out && renews_unanswered_ < RENEW_LIMIT &&
        (!renewed_ || now_ms - last_renew_ms_ >= RENEW_RETRY_MS)) {
      a.kind = LeaseAction::renew;
      a.node_count = last_.node_count;
      renewed_ = true;
      last_renew_ms_ = now_ms;
      renews_unanswered_++;
      renewals_++;
      last_sent_ = LeaseAction::renew;
    } else if (!checked_ || static_cast<int32_t>(now_ms - next_check_ms_) >= 0) {
      a.kind = LeaseAction::check;
      checked_ = true;
      next_check_ms_ = now_ms + CHECK_EVERY_MS;
      last_sent_ = LeaseAction::check;
    }
    return a;
  }
  // A reading counts as this period's check.
  void on_lease(const Lease &l, uint32_t now_ms) {
    last_ = l;
    ever_read_ = true;
    checked_ = true;
    check_retries_ = 0;
    if (l.remaining_s > 0) renews_unanswered_ = 0;
    uint32_t until_out = l.remaining_s > 0 ? l.remaining_s * 1000u + EXPIRY_MARGIN_MS : CHECK_EVERY_MS;
    next_check_ms_ = now_ms + (until_out < CHECK_EVERY_MS ? until_out : CHECK_EVERY_MS);
  }
  void on_no_reply(uint32_t now_ms) {
    if (last_sent_ == LeaseAction::check && check_retries_ < CHECK_RETRIES) {
      check_retries_++;
      next_check_ms_ = now_ms + CHECK_RETRY_MS;
    }
  }

  const Lease &last() const { return last_; }
  uint32_t renewals() const { return renewals_; }
  bool ever_read() const { return ever_read_; }
  bool stuck() const { return renews_unanswered_ >= RENEW_LIMIT; }

 private:
  Lease last_;
  bool ever_read_ = false;
  bool checked_ = false;
  bool renewed_ = false;
  LeaseAction::Kind last_sent_ = LeaseAction::none;
  uint32_t next_check_ms_ = 0;
  uint32_t last_renew_ms_ = 0;
  uint32_t renewals_ = 0;
  uint32_t renews_unanswered_ = 0;
  uint32_t check_retries_ = 0;
};

// ---- telemetry polls -------------------------------------------------------

// retry: the previous poll got no reply; the CCA sends 00 in the second byte
// then, 01 otherwise (26 of 26 such polls in the capture).
inline Bytes poll_payload(uint16_t cursor, bool retry = false) {
  return {0x00, static_cast<uint8_t>(retry ? 0x00 : 0x01), static_cast<uint8_t>(cursor >> 8),
          static_cast<uint8_t>(cursor), 0x04};
}

// Complete power packets in a 0x0149 payload. The header is a status word,
// then fields present when their status bit is clear (bit 0: 1 byte, 1: 1,
// 2: 2, 3: 2, 4: 1) and two always present (bit 5: 1, bit 6: 2), as
// protocol::header_length_chars reads it. Each packet is 7 bytes plus the
// length in its byte 6.
inline uint16_t power_packet_count(const Bytes &payload) {
  if (payload.size() < 2) return 0;
  unsigned status = payload[0] << 8 | payload[1];
  size_t i = 2 + 3;
  static const uint8_t sizes[] = {1, 1, 2, 2, 1};
  for (unsigned bit = 0; bit < 5; bit++)
    if (!(status & (1u << bit))) i += sizes[bit];
  uint16_t n = 0;
  while (i + 7 <= payload.size()) {
    i += 7 + payload[i + 6];
    if (i <= payload.size()) n++;
  }
  return n;
}

// The cursor a 0x0149 reply carries: its low byte always (the field for
// status bit 5), all of it when the longer header is present (bit 4 clear;
// that field is the high byte). In the rig capture both always equal the
// poll's cursor.
struct ReplyCursor {
  uint8_t low = 0;
  bool full = false;
  uint16_t cursor = 0;
};

inline bool reply_cursor(const Bytes &payload, ReplyCursor &out) {
  if (payload.size() < 2) return false;
  unsigned status = payload[0] << 8 | payload[1];
  static const uint8_t sizes[] = {1, 1, 2, 2, 1};
  size_t i = 2, high_at = 0;
  for (unsigned bit = 0; bit < 5; bit++) {
    if (status & (1u << bit)) continue;
    if (bit == 4) high_at = i;
    i += sizes[bit];
  }
  if (i >= payload.size()) return false;
  out.low = payload[i];
  out.full = high_at != 0;
  out.cursor = out.full ? static_cast<uint16_t>(payload[high_at] << 8 | out.low) : out.low;
  return true;
}

// The 0x0148 cursor. The TAP keeps power packets queued; each reply hands
// over some, and the next poll's cursor is the last one plus how many came.
// That rule matches 101,711 of 101,711 polls in the rig capture, and the
// retry byte 101,710 (linux/scripts/cursor-check.py).
class PollCursor {
 public:
  explicit PollCursor(uint16_t start) : cursor_(start) {}
  uint16_t next() const { return cursor_; }
  bool retry() const { return retry_; }
  // Whether a 0x0149 answers the poll just sent: its cursor's low byte is ours.
  bool matches(const Frame &reply) const {
    ReplyCursor rc;
    return reply.cmd == CMD_POLL_REPLY && reply_cursor(reply.payload, rc) && rc.low == (cursor_ & 0xFF);
  }
  // A reply whose full cursor differs from ours states the TAP's, which is
  // adopted: a new master starts at 0 and can't know it. Unconfirmed on a
  // real TAP (in the capture the CCA was always in step); resyncs() counts it.
  void on_reply(const Frame &reply) {
    if (reply.cmd != CMD_POLL_REPLY) return;
    ReplyCursor rc;
    if (reply_cursor(reply.payload, rc) && rc.full && rc.cursor != cursor_) {
      cursor_ = rc.cursor;
      resyncs_++;
    }
    cursor_ = static_cast<uint16_t>(cursor_ + power_packet_count(reply.payload));
    retry_ = false;
  }
  void on_timeout() { retry_ = true; }
  uint32_t resyncs() const { return resyncs_; }

 private:
  uint16_t cursor_;
  bool retry_ = false;
  uint32_t resyncs_ = 0;
};

// ---- the next request -------------------------------------------------------

// Tunnel sequence numbers, as the CCA counts them: FE is followed by 01.
inline uint8_t next_seq(uint8_t seq) {
  seq++;
  return seq == 0xFF || seq == 0x00 ? 0x01 : seq;
}

struct Request {
  uint16_t cmd = 0;
  Bytes payload;
  uint32_t wait_ms = 0;   // how long to wait for the reply; 0 = the usual
  bool bring_up = false;  // a frame the CCA sends only straight after re-addressing a TAP
};

// ---- housekeeping ----------------------------------------------------------

// Two things the CCA sends that the lease and polls don't cover, from a 24 h
// capture of the reference rig's bus:
//   - tunnel sub 0x41, the args a target Tuid then 00 00 00 <stream> 00 and 17
//     more bytes. Three streams (01, 02, 04), each sent once a minute, each
//     moving to its next target every 10 minutes; between them they reached
//     every optimizer. The 17 bytes never changed in 24 h, across CCA
//     reboots. What they mean isn't known, so they are copied, not composed.
//   - 0x0B00 with 01, every hour; after the CCA reboots, 00 then 01 once.
//   - tunnel sub 0x22, empty, to the broadcast address then the TAP, a second
//     apart, a second after each 0x0B00 01.
//   - tunnel sub 0x26, the TAP's node table a page at a time, each page from
//     where the last reply ended: twice over, every 15 minutes.
//   - after re-addressing a TAP: VERSION, 0x0E02 and a tunnel 0x0D pair
//     around the 0x0B00s (HousekeepingPlan::bring_up_reads).
// The 0x41 content is learned while listening (Learned41), or else built
// in. Only the 0x41 target is chosen here (Housekeeping).
inline bool is_housekeeping_args(const Bytes &args) {
  return args.size() == 24 && !args[2] && !args[3] && !args[4] && (args[5] == 0x01 || args[5] == 0x02 || args[5] == 0x04) &&
         !args[6];
}

// The 0x41s a CCA was heard sending: one entry per stream and target, in the
// order first heard; newer content for a target replaces the old.
class Learned41 {
 public:
  static const size_t PER_STREAM = 64;
  struct Entry {
    uint8_t stream = 0;
    uint16_t tuid = 0;
    Bytes args;  // after the seq: the Tuid, then the content
  };

  // A frame read off the bus. True if it taught something new.
  bool on_frame(const Frame &f) {
    if (f.dest & 0x8000 || f.cmd != CMD_TUNNEL || f.payload.size() < 5 || f.payload[0] || f.payload[1] ||
        f.payload[2] || f.payload[3] != SUB_HOUSEKEEPING)
      return false;
    return add(Bytes(f.payload.begin() + 5, f.payload.end()));
  }
  // Entries saved earlier; malformed ones are dropped.
  void load(const std::vector<Entry> &saved) {
    entries_.clear();
    for (const Entry &e : saved)
      if (is_housekeeping_args(e.args) && e.stream == e.args[5] && e.tuid == (e.args[0] << 8 | e.args[1])) add(e.args);
  }
  const std::vector<Entry> &entries() const { return entries_; }

 private:
  bool add(const Bytes &args) {
    if (!is_housekeeping_args(args)) return false;
    uint8_t stream = args[5];
    uint16_t tuid = static_cast<uint16_t>(args[0] << 8 | args[1]);
    size_t in_stream = 0;
    for (Entry &e : entries_) {
      if (e.stream != stream) continue;
      in_stream++;
      if (e.tuid != tuid) continue;
      if (e.args == args) return false;
      e.args = args;
      return true;
    }
    if (in_stream >= PER_STREAM) return false;
    entries_.push_back({stream, tuid, args});
    return true;
  }
  std::vector<Entry> entries_;
};

// What a Housekeeping sends. The CCA's order after it re-addresses a TAP
// (13:47:47 in the capture): VERSION, 0x0E02, [0x0B00 00], 0x0D 00 00,
// 0x0B00 01, 0x0D 00 01, the 0x22 pair, then two walks of the node table.
struct HousekeepingPlan {
  bool send_41 = true;
  bool session = true;          // 0x0B00 01 at the start and every hour
  bool session_reset = false;   // 0x0B00 00 first, as after the CCA's own reboot
  bool bring_up_reads = false;  // VERSION, 0x0E02 and the 0x0D pair: after a re-address
  bool broadcast = true;        // the empty 0x22 pair with each 0x0B00 01
  bool walks = true;            // the 0x26 node table walks
};

// Sends the 0x41s on the CCA's timing, and 0x0B00 01 at the start and every
// hour after, with the rest of its plan (HousekeepingPlan).
//
// Each stream's content is the CCA's: learned (Learned41) if heard, else
// default_content(), the block current CCA firmware sends. Targets are not
// copied: the CCA's own order favours some optimizers for reasons unknown,
// so each stream instead walks
// the optimizers heard lately (set_targets) in a random order, a new one
// every 10 minutes, each visited once a pass. Stream 01 goes to 0x0001 every
// other slot, starting with the first, as the CCA's does after every reboot.
class Housekeeping {
 public:
  static const uint32_t STREAM_EVERY_MS = 60000;
  static const uint32_t SLOT_MS = 600000;
  static const uint32_t SESSION_EVERY_MS = 3600000;
  // Streams 02, 01, 04 go out in that order, a few seconds apart, as the CCA's do.
  static constexpr uint8_t STREAMS[3] = {0x02, 0x01, 0x04};
  static constexpr uint32_t OFFSET_MS[3] = {1000, 6000, 11000};
  static const uint16_t TAP_TUID = 0x0001;
  static const uint32_t BROADCAST_GAP_MS = 1000;  // 0x0B00 01, then each 0x22 a second later
  static const uint32_t WALK_EVERY_MS = 15 * 60000;
  static const int WALKS = 2;
  static const int WALK_MAX_PAGES = 32;
  static const uint32_t WALK_REPLY_MS = 5000;  // a page unanswered this long ends its walk

  // 0x41s and 0x0B00 only, as before the rest was known.
  Housekeeping(const std::vector<Learned41::Entry> &learned, uint32_t now_ms, bool session_reset, uint32_t seed = 1)
      : Housekeeping(learned, now_ms, legacy_plan_(session_reset), seed) {}

  Housekeeping(const std::vector<Learned41::Entry> &learned, uint32_t now_ms, const HousekeepingPlan &plan,
               uint32_t seed)
      : plan_(plan), start_ms_(now_ms), next_tick_ms_(now_ms + SESSION_EVERY_MS),
        next_walk_ms_(now_ms + WALK_EVERY_MS), rng_(seed ? seed : 1) {
    for (int i = 0; i < 3; i++) {
      for (const auto &e : learned)
        if (e.stream == STREAMS[i]) {
          content_[i].assign(e.args.begin() + 2, e.args.end());
          break;
        }
      if (content_[i].empty()) content_[i] = default_content(STREAMS[i]);
      next_ms_[i] = now_ms + OFFSET_MS[i];
    }
    if (plan.bring_up_reads) {
      opening_.push_back({0x000A, 0, {}, false, 0});
      opening_.push_back({0x0E02, 0, {}, true, 0});
    }
    if (plan.session && plan.session_reset) opening_.push_back({CMD_SESSION, 0, {0x00}, true, 0});
    if (plan.bring_up_reads) opening_.push_back({CMD_TUNNEL, 0x0D, {0x00, 0x00}, true, 0});
    if (plan.session) opening_.push_back({CMD_SESSION, 0, {0x01}, false, 0});
    if (plan.bring_up_reads) opening_.push_back({CMD_TUNNEL, 0x0D, {0x00, 0x01}, true, 0});
    if (plan.broadcast) {
      opening_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x00}, false, 0});
      opening_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x01}, false, 0});
    }
    if (plan.walks) walks_left_ = WALKS;
  }

  // A stream's content (the args after the Tuid) as current CCA firmware
  // sends it: the same on every target, across CCA reboots, and on two
  // sites, ours and the one in taptap's issue #20 capture
  // (src/test_data/issue_20_initial.bin). taptap's protocol.md shows an older
  // firmware's, without the stream byte and with two values different, so a
  // learned block wins over this one.
  static Bytes default_content(uint8_t stream) {
    return {0x00, 0x00, 0x00, stream, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x04, 0x05,
            0x0f, 0xe8, 0x08, 0xa0, 0x0a, 0x18, 0x8f, 0xf6, 0x05, 0xc7};
  }

  // The optimizers to choose from: their Tuids. 0, 0x0001 and anything with
  // the high bit (a roster entry not yet joined) are left out.
  void set_targets(const std::vector<uint16_t> &tuids) {
    targets_.clear();
    for (uint16_t t : tuids)
      if (t > TAP_TUID && !(t & 0x8000) && std::find(targets_.begin(), targets_.end(), t) == targets_.end())
        targets_.push_back(t);
  }

  // The next request due, or one with cmd 0. A tunnel request takes the next seq.
  Request due(uint32_t now_ms, uint8_t &seq) {
    if (opening_next_ < opening_.size()) return emit_(opening_[opening_next_++], seq);
    for (size_t k = 0; k < later_.size(); k++)
      if (static_cast<int32_t>(now_ms - later_[k].at_ms) >= 0) {
        Item it = later_[k];
        later_.erase(later_.begin() + static_cast<long>(k));
        return emit_(it, seq);
      }
    if ((plan_.session || plan_.broadcast) && static_cast<int32_t>(now_ms - next_tick_ms_) >= 0) {
      next_tick_ms_ = now_ms + SESSION_EVERY_MS;
      if (plan_.broadcast) {
        later_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x00}, false, now_ms + BROADCAST_GAP_MS});
        later_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x01}, false, now_ms + 2 * BROADCAST_GAP_MS});
      }
      if (plan_.session) return emit_({CMD_SESSION, 0, {0x01}, false, 0}, seq);
    }
    if (plan_.walks) {
      if (walk_waiting_ && now_ms - walk_sent_ms_ >= WALK_REPLY_MS) end_walk_();  // never answered
      if (!walk_waiting_ && walks_left_ == 0 && static_cast<int32_t>(now_ms - next_walk_ms_) >= 0) {
        next_walk_ms_ = now_ms + WALK_EVERY_MS;
        walks_left_ = WALKS;
      }
      if (walks_left_ > 0 && !walk_waiting_) {
        walk_waiting_ = true;
        walk_sent_ms_ = now_ms;
        walk_pages_++;
        sent_walk_pages_++;
        return emit_({CMD_TUNNEL, 0x26, {static_cast<uint8_t>(walk_index_ >> 8), static_cast<uint8_t>(walk_index_)}, false, 0},
                     seq);
      }
    }
    for (int i = 0; i < 3 && plan_.send_41; i++) {
      if (content_[i].empty() || static_cast<int32_t>(now_ms - next_ms_[i]) < 0) continue;
      next_ms_[i] = now_ms + STREAM_EVERY_MS;
      uint32_t slot = (now_ms - start_ms_) / SLOT_MS;
      if (!picked_[i] || slot != slot_[i] || !target_[i]) {
        slot_[i] = slot;
        picked_[i] = true;
        target_[i] = STREAMS[i] == 0x01 && slot % 2 == 0 ? TAP_TUID : draw_(i);
      }
      if (!target_[i]) continue;  // no optimizers heard yet
      Bytes args = {static_cast<uint8_t>(target_[i] >> 8), static_cast<uint8_t>(target_[i])};
      args.insert(args.end(), content_[i].begin(), content_[i].end());
      seq = next_seq(seq);
      sent_41_++;
      return {CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, seq, args)};
    }
    return {};
  }
  // What came back for a request due() made; ok false if nothing did.
  void on_reply(const Request &req, bool ok, const Frame &reply) {
    if (req.cmd == 0x000A && ok) version_.assign(reply.payload.begin(), reply.payload.end());
    if (req.cmd != CMD_TUNNEL || req.payload.size() < 4 || req.payload[3] != 0x26 || !walk_waiting_) return;
    walk_waiting_ = false;
    TunnelReply t;
    if (!ok || !parse_tunnel_reply(reply.payload, t) || t.sub != 0x27 || t.data.size() < 4) return end_walk_();
    uint16_t start = static_cast<uint16_t>(t.data[0] << 8 | t.data[1]);
    uint16_t count = static_cast<uint16_t>(t.data[2] << 8 | t.data[3]);
    uint32_t next = static_cast<uint32_t>(start) + count;
    // The end of the table, or a reply that doesn't move on.
    if (count == 0 || next <= walk_index_ || next > 0xFFFF || walk_pages_ >= WALK_MAX_PAGES) return end_walk_();
    walk_index_ = static_cast<uint16_t>(next);
  }

  uint32_t sent_41() const { return sent_41_; }
  uint32_t sent_session() const { return sent_session_; }
  uint32_t sent_broadcast() const { return sent_broadcast_; }
  uint32_t sent_walk_pages() const { return sent_walk_pages_; }
  // VERSION's reply, if a bring-up asked.
  const std::string &version() const { return version_; }

 private:
  struct Item {
    uint16_t cmd;
    uint8_t sub;  // for a tunnel request
    Bytes args;   // the tunnel args, or the payload
    bool bring_up;
    uint32_t at_ms;
  };
  static HousekeepingPlan legacy_plan_(bool session_reset) {
    HousekeepingPlan p;
    p.session_reset = session_reset;
    p.broadcast = false;
    p.walks = false;
    return p;
  }
  Request emit_(const Item &it, uint8_t &seq) {
    Request r;
    r.cmd = it.cmd;
    r.bring_up = it.bring_up;
    if (it.cmd == CMD_TUNNEL) {
      seq = next_seq(seq);
      r.payload = tunnel_request(it.sub, seq, it.args);
      if (it.sub == 0x22) sent_broadcast_++;
    } else {
      r.payload = it.args;
      if (it.cmd == CMD_SESSION) sent_session_++;
    }
    return r;
  }
  void end_walk_() {
    walk_waiting_ = false;
    walk_index_ = 0;
    walk_pages_ = 0;
    if (walks_left_ > 0) walks_left_--;
  }

  // The next target from this stream's shuffled pass, 0 if there are none.
  uint16_t draw_(int i) {
    for (int tries = 0; tries < 2; tries++) {
      while (!bag_[i].empty()) {
        uint16_t t = bag_[i].back();
        bag_[i].pop_back();
        if (std::find(targets_.begin(), targets_.end(), t) != targets_.end()) return t;
      }
      bag_[i] = targets_;
      for (size_t k = bag_[i].size(); k > 1; k--) std::swap(bag_[i][k - 1], bag_[i][next_random_() % k]);
    }
    return 0;
  }
  uint32_t next_random_() {  // xorshift32: the same sequence on every build
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }

  HousekeepingPlan plan_;
  uint32_t start_ms_;
  uint32_t next_tick_ms_;
  uint32_t next_walk_ms_;
  std::vector<Item> opening_;
  size_t opening_next_ = 0;
  std::vector<Item> later_;
  int walks_left_ = 0;
  bool walk_waiting_ = false;
  uint32_t walk_sent_ms_ = 0;
  uint16_t walk_index_ = 0;
  int walk_pages_ = 0;
  uint32_t sent_broadcast_ = 0;
  uint32_t sent_walk_pages_ = 0;
  std::string version_;
  uint32_t rng_;
  std::vector<uint16_t> targets_;
  Bytes content_[3];
  std::vector<uint16_t> bag_[3];
  uint16_t target_[3] = {0, 0, 0};
  uint32_t slot_[3] = {0, 0, 0};
  bool picked_[3] = {false, false, false};
  uint32_t next_ms_[3] = {0, 0, 0};
  uint32_t sent_41_ = 0;
  uint32_t sent_session_ = 0;
};

// The CCA's text queries to optimizers (tunnel sub 0x06), and its two writes.
static const char *const QUERY_READS[] = {"^00Version\r", "^00Tests\r", "^00Info\r", "^00Smrt\r", "^00Mppt\r"};
static const char *const QUERY_W255 = "#00w255\r";
static const char *const QUERY_SMART_S = "^00Smrt_........S0000...0000\r";

// The CCA's text dialogue with optimizers, tunnel sub 0x06, from the 24 h
// capture. A query is the node's Tuid and an ASCII line; the TAP only
// forwards it (status 0d), and the optimizer's answer comes back later as a
// type 0x07 packet in a 0x0149 poll reply. Many are lost, so the CCA asks
// again, 3 minutes on, then 2 minutes later each time (up to 30), until
// one comes. A node first heard gets all five reads (as every node did after
// a CCA reboot); one heard again after half an hour asleep gets ^00Tests (as
// each did at sunrise). Two answers bring a write, with writes on: an Info
// whose ninth word is 00 brings #00w255 (50 of 50 in the capture; its
// field read FF on 104 other answers), and Info is read again 40 minutes
// on; a Smrt in mode N brings ^00Smrt_........S0000...0000 (30 of 30), and
// Smrt is read again to see it took. What the fields mean isn't known.
class Queries {
 public:
  static const uint32_t GAP_MS = 1500;  // the CCA's were 1-2 s apart
  static const uint32_t INFO_AGAIN_MS = 40 * 60000;
  static const uint32_t CHECK_AFTER_MS = 3 * 60000;  // a read after a write
  static const uint32_t ASLEEP_MS = 30 * 60000;
  enum Kind { version, tests, info, smrt, mppt, w255, smart_s };

  Queries(uint32_t now_ms, bool writes) : writes_(writes), last_sent_ms_(now_ms - GAP_MS) {}

  static uint32_t retry_after_ms(uint32_t tries) {
    uint32_t m = 3 + 2 * (tries > 0 ? tries - 1 : 0);
    return (m > 30 ? 30 : m) * 60000;
  }

  // A 0x0149 payload: who reported, and any answers.
  void on_poll_reply(const Bytes &payload, uint32_t now_ms) {
    if (payload.size() < 2) return;
    unsigned status = payload[0] << 8 | payload[1];
    size_t i = 2 + 3;
    static const uint8_t sizes[] = {1, 1, 2, 2, 1};
    for (unsigned bit = 0; bit < 5; bit++)
      if (!(status & (1u << bit))) i += sizes[bit];
    while (i + 7 <= payload.size()) {
      size_t end = i + 7 + payload[i + 6];
      if (end > payload.size()) break;
      uint16_t tuid = static_cast<uint16_t>(payload[i + 1] << 8 | payload[i + 2]);
      if (tuid > 0x0001 && !(tuid & 0x8000)) {
        heard_(tuid, now_ms);
        if (payload[i] == 0x07) answer_(tuid, std::string(payload.begin() + i + 7, payload.begin() + end), now_ms);
      }
      i = end;
    }
  }

  // The next query due, or one with cmd 0.
  Request due(uint32_t now_ms, uint8_t &seq) {
    if (now_ms - last_sent_ms_ < GAP_MS) return {};
    // Asleep since: its queries go, and wait for it to wake.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Item &it) {
                                    auto h = heard_ms_.find(it.tuid);
                                    return h != heard_ms_.end() && now_ms - h->second >= ASLEEP_MS;
                                  }),
                   pending_.end());
    size_t best = pending_.size();
    for (size_t k = 0; k < pending_.size(); k++) {
      if (static_cast<int32_t>(now_ms - pending_[k].next_ms) < 0) continue;
      if (best == pending_.size() || static_cast<int32_t>(pending_[k].next_ms - pending_[best].next_ms) < 0) best = k;
    }
    if (best == pending_.size()) return {};
    Item &it = pending_[best];
    Bytes args = {static_cast<uint8_t>(it.tuid >> 8), static_cast<uint8_t>(it.tuid)};
    const char *text = it.kind == w255 ? QUERY_W255 : it.kind == smart_s ? QUERY_SMART_S : QUERY_READS[it.kind];
    args.insert(args.end(), text, text + std::char_traits<char>::length(text));
    if (it.kind == w255 || it.kind == smart_s) {
      sent_writes_++;
      pending_.erase(pending_.begin() + static_cast<long>(best));  // once, as the CCA's
    } else {
      sent_reads_++;
      it.tries++;
      it.next_ms = now_ms + retry_after_ms(it.tries);
    }
    last_sent_ms_ = now_ms;
    seq = next_seq(seq);
    return {CMD_TUNNEL, tunnel_request(0x06, seq, args)};
  }

  uint32_t sent_reads() const { return sent_reads_; }
  uint32_t sent_writes() const { return sent_writes_; }
  uint32_t answers() const { return answers_; }

 private:
  struct Item {
    uint16_t tuid;
    Kind kind;
    uint32_t next_ms;
    uint32_t tries;
  };

  void heard_(uint16_t tuid, uint32_t now_ms) {
    auto h = heard_ms_.find(tuid);
    if (h == heard_ms_.end()) {
      for (Kind k : {version, tests, info, smrt, mppt}) add_(tuid, k, now_ms);
    } else if (now_ms - h->second >= ASLEEP_MS) {
      drop_(tuid);
      add_(tuid, tests, now_ms);
    }
    heard_ms_[tuid] = now_ms;
  }

  void answer_(uint16_t tuid, const std::string &text, uint32_t now_ms) {
    std::vector<std::string> f;
    size_t a = 0;
    while (a < text.size()) {
      size_t b = text.find_first_of(" \r", a);
      if (b == std::string::npos) b = text.size();
      if (b > a) f.push_back(text.substr(a, b - a));
      a = b + 1;
    }
    if (f.empty()) return;
    Kind k;
    if (f[0] == "Mnode") k = version;
    else if (f[0] == "!Tests") k = tests;
    else if (f[0] == "!Info") k = info;
    else if (f[0] == "!Smrt") k = smrt;
    else if (f[0] == "!Mppt") k = mppt;
    else return;
    answers_++;
    remove_(tuid, k);
    if (k == info && f.size() > 8 && f[8] == "00") {
      if (writes_) add_(tuid, w255, now_ms);
      add_(tuid, info, now_ms + INFO_AGAIN_MS);
    }
    if (k == smrt && f.size() > 10 && f[10] == "N" && writes_) {
      add_(tuid, smart_s, now_ms);
      add_(tuid, smrt, now_ms + CHECK_AFTER_MS);
    }
  }

  void add_(uint16_t tuid, Kind k, uint32_t at_ms) {
    for (Item &it : pending_)
      if (it.tuid == tuid && it.kind == k) return;
    pending_.push_back({tuid, k, at_ms, 0});
  }
  void remove_(uint16_t tuid, Kind k) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Item &it) { return it.tuid == tuid && it.kind == k; }),
                   pending_.end());
  }
  void drop_(uint16_t tuid) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Item &it) { return it.tuid == tuid; }),
                   pending_.end());
  }

  bool writes_;
  uint32_t last_sent_ms_;
  std::vector<Item> pending_;
  std::map<uint16_t, uint32_t> heard_ms_;
  uint32_t sent_reads_ = 0, sent_writes_ = 0, answers_ = 0;
};

// What a master sends next: lease work when it's due, then housekeeping (if
// there is any), then queries (if there are any), a poll otherwise. seq is
// the last tunnel sequence number used, advanced when one is.
inline Request next_request(LeaseKeeper &lease, const PollCursor &cursor, uint8_t &seq, uint32_t now_ms,
                            Housekeeping *housekeeping = nullptr, Queries *queries = nullptr) {
  LeaseAction a = lease.due(now_ms);
  if (a.kind == LeaseAction::none) {
    if (housekeeping) {
      Request r = housekeeping->due(now_ms, seq);
      if (r.cmd) return r;
    }
    if (queries) {
      Request r = queries->due(now_ms, seq);
      if (r.cmd) return r;
    }
    return {CMD_POLL, poll_payload(cursor.next(), cursor.retry())};
  }
  seq = next_seq(seq);
  if (a.kind == LeaseAction::renew) return {CMD_TUNNEL, tunnel_request(SUB_LEASE_RENEW, seq, lease_renewal_args(a.node_count))};
  return {CMD_TUNNEL, tunnel_request(SUB_LEASE_CHECK, seq, {})};
}

// ---- gateway discovery -----------------------------------------------------

// A TAP that has lost power comes up at the factory RS485 address (0x1235 on
// the roof), not the id the CCA gave it. The CCA re-addresses it every boot:
// a SET_485_ID broadcast, GET_MAC to read its MAC, SET_ID_BY_MAC to assign the
// running id, then a read at that id to confirm. This drives the same four
// steps. It sends re-addressing writes, so BusMaster only runs it after a read
// has confirmed the TAP is actually at the factory address, and only in the
// explicit startup mode.
//
// Then, as the CCA does, it scans: five more GET_MACs at the factory address,
// each waiting out the TAP's reply backoff (the CCA's five take 3-4 s). A TAP
// that answers is a second one. Only one TAP is supported, so its MAC is
// noted for BusMaster to report, and nothing is ever sent to it by MAC.
// scan_only skips straight to the scan, for a TAP already at its id.
static const uint16_t CMD_SET_485_ID = 0x0014;
static const uint16_t CMD_GET_MAC = 0x0038;        // with backoff
static const uint16_t CMD_GET_MAC_NOW = 0x003A;    // no backoff; also the "where are you" read
static const uint16_t CMD_SET_ID_BY_MAC = 0x003C;
static const uint16_t CMD_RESET = 0x0010;

struct DiscoveryRequest {
  bool send = false;
  bool done = false;
  bool failed = false;
  uint16_t dest = 0;
  uint16_t cmd = 0;
  Bytes payload;
  uint32_t wait_ms = 0;  // how long to wait for the reply; 0 = the usual
  uint32_t idle_ms = 0;  // send nothing at all for this long, then ask again
};

class Discovery {
 public:
  static const int SCAN_TRIES = 5;
  static const uint32_t BACKOFF_WAIT_MS = 800;  // GET_MAC with backoff: the CCA allows ~0.8 s a try
  // After its RESET the CCA leaves the bus quiet ~5 s, then reads once.
  static const uint32_t RESET_QUIET_MS = 5000;
  static const uint32_t BACK_WAIT_MS = 1000;
  static const int BACK_TRIES = 5;

  Discovery(uint16_t factory_id, uint16_t target_id, bool scan_only = false)
      : state_(scan_only ? scan : claim), scan_only_(scan_only), factory_(factory_id), target_(target_id) {}

  DiscoveryRequest next() {
    DiscoveryRequest r;
    if (state_ == st_failed) { r.failed = true; return r; }
    if (state_ == st_done) { r.done = true; return r; }
    r.send = true;
    switch (state_) {
      case claim:  // SET_485_ID broadcast: payload = key + factory id (BE)
        r.dest = 0x0000; r.cmd = CMD_SET_485_ID;
        r.payload = {0x37, 0x24, 0x92, 0x66, static_cast<uint8_t>(factory_ >> 8), static_cast<uint8_t>(factory_)};
        break;
      case get_mac:
        r.dest = factory_; r.cmd = CMD_GET_MAC; r.wait_ms = BACKOFF_WAIT_MS;
        break;
      case set_id:  // key + MAC (8) + target id (BE)
        r.dest = factory_; r.cmd = CMD_SET_ID_BY_MAC;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        r.payload.insert(r.payload.end(), mac_.begin(), mac_.end());
        r.payload.push_back(static_cast<uint8_t>(target_ >> 8));
        r.payload.push_back(static_cast<uint8_t>(target_));
        break;
      case verify:
        r.dest = target_; r.cmd = CMD_GET_MAC_NOW;
        break;
      case scan:
        if (scans_sent_ == SCAN_TRIES) {
          state_ = scan_only_ ? st_done : hop;
          return next();
        }
        scans_sent_++;
        r.dest = factory_; r.cmd = CMD_GET_MAC; r.wait_ms = BACKOFF_WAIT_MS;
        break;
      // Then the CCA's hop: the TAP to the id one up, read there, a RESET
      // broadcast, quiet, and the TAP answers at the configured id again.
      case hop:
        r.dest = target_; r.cmd = CMD_SET_ID_BY_MAC;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        r.payload.insert(r.payload.end(), mac_.begin(), mac_.end());
        r.payload.push_back(static_cast<uint8_t>((target_ + 1) >> 8));
        r.payload.push_back(static_cast<uint8_t>(target_ + 1));
        break;
      case hop_verify:
        r.dest = static_cast<uint16_t>(target_ + 1); r.cmd = CMD_GET_MAC_NOW;
        break;
      case reset:
        r.dest = 0x0000; r.cmd = CMD_RESET;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        break;
      case quiet:
        state_ = back;
        r.send = false;
        r.idle_ms = RESET_QUIET_MS;
        break;
      case back:
        if (back_sent_ == BACK_TRIES) {
          state_ = st_failed;
          return next();
        }
        back_sent_++;
        r.dest = target_; r.cmd = CMD_GET_MAC_NOW; r.wait_ms = BACK_WAIT_MS;
        break;
      default: break;
    }
    return r;
  }

  void on_reply(const Frame &f) {
    switch (state_) {
      case claim:
        if (f.cmd == CMD_SET_485_ID + 1) state_ = get_mac;
        break;
      case get_mac:
        if (f.cmd == CMD_GET_MAC + 1 && f.payload.size() >= 10) {
          mac_.assign(f.payload.begin(), f.payload.begin() + 8);
          state_ = set_id;
        }
        break;
      case set_id:
        if (f.cmd == CMD_SET_ID_BY_MAC + 1) state_ = verify;
        break;
      case verify:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10) {
          uint16_t id = static_cast<uint16_t>(f.payload[8] << 8 | f.payload[9]);
          state_ = id == target_ ? scan : st_failed;
        }
        break;
      case scan:
        if (f.cmd == CMD_GET_MAC + 1 && f.payload.size() >= 10) {
          Bytes mac(f.payload.begin(), f.payload.begin() + 8);
          if (mac != mac_ && std::find(extra_.begin(), extra_.end(), mac) == extra_.end()) extra_.push_back(mac);
        }
        break;
      case hop:
        if (f.cmd == CMD_SET_ID_BY_MAC + 1) state_ = hop_verify;
        break;
      case hop_verify:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10)
          state_ = (f.payload[8] << 8 | f.payload[9]) == target_ + 1 ? reset : st_failed;
        break;
      case reset:
        if (f.cmd == CMD_RESET + 1) state_ = quiet;
        break;
      case back:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10 && (f.payload[8] << 8 | f.payload[9]) == target_)
          state_ = st_done;
        break;
      default: break;
    }
  }

  bool done() const { return state_ == st_done; }
  bool failed() const { return state_ == st_failed; }
  const Bytes &mac() const { return mac_; }
  // MACs of other TAPs that answered the scan, in the order heard.
  const std::vector<Bytes> &extra_taps() const { return extra_; }

 private:
  enum State { claim, get_mac, set_id, verify, scan, hop, hop_verify, reset, quiet, back, st_done, st_failed } state_;
  bool scan_only_;
  uint16_t factory_, target_;
  Bytes mac_;
  int scans_sent_ = 0;
  int back_sent_ = 0;
  std::vector<Bytes> extra_;
};

// ---- what a master may send ------------------------------------------------

// yes: what the CCA sends in steady state, in the shapes it sends them.
// bench_only: reads and anything else, for a TAP on the bench.
// never: writes. The commands that change what a TAP stores or does (reset,
// store, ids, roster, whitelist, netrun) and anything carrying the setup key
// 37 24 92 66. Nothing in this code sends them, whatever the settings.
// optimizer_write: the two settings the CCA writes to optimizers whose
// replies ask for it (Queries), sent only with that setting on.
enum class Allow { yes, bench_only, startup_only, optimizer_write, never };


inline Allow classify_request(uint16_t cmd, const Bytes &payload) {
  // The re-addressing writes a cold boot needs. Allowed only in startup mode,
  // and BusMaster only after a read confirms the TAP is at the factory address.
  static const uint16_t STARTUP_WRITES[] = {CMD_RESET, CMD_SET_485_ID, CMD_SET_ID_BY_MAC};
  for (uint16_t w : STARTUP_WRITES)
    if (cmd == w) return Allow::startup_only;
  // The writes nothing here ever sends: store, clear-roster, whitelist, and
  // the rapid-shutdown netrun lever.
  static const uint16_t NEVER[] = {0x0012, 0x011E, 0x0182, 0x0058};
  for (uint16_t w : NEVER)
    if (cmd == w) return Allow::never;
  // Any other command carrying the setup key is refused.
  static const uint8_t KEY[] = {0x37, 0x24, 0x92, 0x66};
  for (size_t i = 0; i + 4 <= payload.size(); i++)
    if (std::equal(KEY, KEY + 4, payload.begin() + i)) return Allow::never;

  // Reads: VERSION any time; 0x0E02 only in the CCA's bring-up.
  if (cmd == 0x000A) return payload.empty() ? Allow::yes : Allow::bench_only;
  if (cmd == 0x0E02) return payload.empty() ? Allow::startup_only : Allow::bench_only;
  // The CCA's hourly 01; its 00 only follows its own reboot, so bring-up only.
  if (cmd == CMD_SESSION)
    return payload == Bytes{0x01} ? Allow::yes : payload == Bytes{0x00} ? Allow::startup_only : Allow::bench_only;
  if (cmd == CMD_POLL)
    return payload.size() == 5 && payload[0] == 0x00 && payload[1] <= 0x01 && payload[4] == 0x04 ? Allow::yes
                                                                                               : Allow::bench_only;
  if (cmd != CMD_TUNNEL || payload.size() < 5 || payload[0] || payload[1] || payload[2]) return Allow::bench_only;
  Bytes args(payload.begin() + 5, payload.end());
  switch (payload[3]) {
    case SUB_LEASE_CHECK:
      return args.empty() ? Allow::yes : Allow::bench_only;
    case SUB_LEASE_RENEW:
      // Only the CCA's renewal, with a node count; it never sent 0.
      return args.size() == 9 && Bytes(args.begin(), args.begin() + 5) == Bytes{0xBA, 0xBE, 0x02, 0x03, 0x84} &&
                     (args[5] || args[6]) && args[7] == 0x01 && args[8] == 0x00
                 ? Allow::yes
                 : Allow::bench_only;
    case SUB_HOUSEKEEPING:  // only the CCA's shape; Housekeeping sends only learned ones
      return is_housekeeping_args(args) ? Allow::yes : Allow::bench_only;
    case 0x26:  // a page of the roster, by index
      return args.size() == 2 ? Allow::yes : Allow::bench_only;
    case 0x06: {  // a text query to one node: the five the CCA sends, and its two writes
      if (args.size() < 2) return Allow::bench_only;
      std::string text(args.begin() + 2, args.end());
      for (const char *q : QUERY_READS)
        if (text == q) return Allow::yes;
      if (text == QUERY_W255 || text == QUERY_SMART_S) return Allow::optimizer_write;
      // Any other write: a '#' command, or Smrt_ with other settings.
      if (text.rfind("#", 0) == 0 || text.rfind("^00Smrt_", 0) == 0) return Allow::never;
      return Allow::bench_only;
    }
    case 0x0D:  // the bring-up's pair, 00 00 then 00 01
      return args == Bytes{0x00, 0x00} || args == Bytes{0x00, 0x01} ? Allow::startup_only : Allow::bench_only;
    case 0x22:
      // Empty, to the broadcast address or the TAP. With data after the node
      // id it is what taptap saw an older CCA use for "PV off": never.
      if (args.size() > 2) return Allow::never;
      return args == Bytes{0x00, 0x00} || args == Bytes{0x00, 0x01} ? Allow::yes : Allow::bench_only;
    default:
      return Allow::bench_only;
  }
}

inline bool may_send(uint16_t cmd, const Bytes &payload, bool bench, bool startup = false, bool writes = false) {
  switch (classify_request(cmd, payload)) {
    case Allow::yes: return true;
    case Allow::bench_only: return bench || startup;
    case Allow::startup_only: return startup;
    case Allow::optimizer_write: return writes;
    case Allow::never: return false;
  }
  return false;
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
