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
};

// What a master sends next: lease work when it's due, a poll otherwise.
// seq is the last tunnel sequence number used, advanced when one is.
inline Request next_request(LeaseKeeper &lease, const PollCursor &cursor, uint8_t &seq, uint32_t now_ms) {
  LeaseAction a = lease.due(now_ms);
  if (a.kind == LeaseAction::none) return {CMD_POLL, poll_payload(cursor.next(), cursor.retry())};
  seq = next_seq(seq);
  if (a.kind == LeaseAction::renew) return {CMD_TUNNEL, tunnel_request(SUB_LEASE_RENEW, seq, lease_renewal_args(a.node_count))};
  return {CMD_TUNNEL, tunnel_request(SUB_LEASE_CHECK, seq, {})};
}

// ---- what a master may send ------------------------------------------------

// yes: what the CCA sends in steady state, in the shapes it sends them.
// bench_only: reads and anything else, for a TAP on the bench.
// never: writes. The commands that change what a TAP stores or does (reset,
// store, ids, roster, whitelist, netrun) and anything carrying the setup key
// 37 24 92 66. Nothing in this code sends them, whatever the settings.
enum class Allow { yes, bench_only, never };

inline Allow classify_request(uint16_t cmd, const Bytes &payload) {
  static const uint16_t WRITES[] = {0x0010, 0x0012, 0x0014, 0x003C, 0x011E, 0x0182, 0x0058};
  for (uint16_t w : WRITES)
    if (cmd == w) return Allow::never;
  static const uint8_t KEY[] = {0x37, 0x24, 0x92, 0x66};
  for (size_t i = 0; i + 4 <= payload.size(); i++)
    if (std::equal(KEY, KEY + 4, payload.begin() + i)) return Allow::never;

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
    case 0x26:  // a page of the roster, by index
      return args.size() == 2 ? Allow::yes : Allow::bench_only;
    case 0x06: {  // a text query to one node: the four the CCA sends
      if (args.size() < 2) return Allow::bench_only;
      std::string text(args.begin() + 2, args.end());
      for (const char *q : {"^00Version\r", "^00Smrt\r", "^00Mppt\r", "^00Info\r"})
        if (text == q) return Allow::yes;
      return Allow::bench_only;
    }
    default:
      return Allow::bench_only;
  }
}

inline bool may_send(uint16_t cmd, const Bytes &payload, bool bench) {
  Allow a = classify_request(cmd, payload);
  return a == Allow::yes || (a == Allow::bench_only && bench);
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
