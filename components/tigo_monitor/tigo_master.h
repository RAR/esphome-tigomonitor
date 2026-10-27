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
  uint32_t wait_ms = 0;  // how long to wait for the reply; 0 = the usual
};

// ---- housekeeping ----------------------------------------------------------

// Two things the CCA sends that the lease and polls don't cover, from a 24 h
// capture of the reference rig's bus:
//   - tunnel sub 0x41, the args a target Tuid then 00 00 00 <stream> 00 and 17
//     more bytes. Three streams (01, 02, 04), each sent once a minute, each
//     moving to its next target every 10 minutes. The 17 bytes never changed
//     in 24 h, across CCA reboots. What it does isn't known, so the bytes are
//     copied, not composed: a master sends only frames it heard the CCA send.
//   - 0x0B00 with 01, every hour; after the CCA reboots, 00 then 01 once.
// The content is the site's own, learned while listening (Learned41); none
// of it is built in.
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

// Sends the learned 0x41s on the CCA's timing, and 0x0B00 01 at the start
// and every hour after. session_reset: send 00 before the first 01, as the
// CCA does after it reboots and re-addresses the TAP.
class Housekeeping {
 public:
  static const uint32_t STREAM_EVERY_MS = 60000;
  static const uint32_t SLOT_MS = 600000;
  static const uint32_t SESSION_EVERY_MS = 3600000;
  // Streams 02, 01, 04 go out in that order, a few seconds apart, as the CCA's do.
  static constexpr uint8_t STREAMS[3] = {0x02, 0x01, 0x04};
  static constexpr uint32_t OFFSET_MS[3] = {1000, 6000, 11000};

  Housekeeping(const std::vector<Learned41::Entry> &learned, uint32_t now_ms, bool session_reset)
      : start_ms_(now_ms), reset_pending_(session_reset), next_session_ms_(now_ms) {
    for (int i = 0; i < 3; i++) {
      for (const auto &e : learned)
        if (e.stream == STREAMS[i]) targets_[i].push_back(e.args);
      // The CCA's stream 01 starts at 0x0001 after every reboot.
      if (STREAMS[i] == 0x01)
        std::stable_partition(targets_[i].begin(), targets_[i].end(),
                              [](const Bytes &a) { return a[0] == 0x00 && a[1] == 0x01; });
      next_ms_[i] = now_ms + OFFSET_MS[i];
    }
  }

  // The next request due, or one with cmd 0. A 0x41 takes the next seq.
  Request due(uint32_t now_ms, uint8_t &seq) {
    if (reset_pending_) {
      reset_pending_ = false;
      sent_session_++;
      return {CMD_SESSION, {0x00}};
    }
    if (static_cast<int32_t>(now_ms - next_session_ms_) >= 0) {
      next_session_ms_ = now_ms + SESSION_EVERY_MS;
      sent_session_++;
      return {CMD_SESSION, {0x01}};
    }
    for (int i = 0; i < 3; i++) {
      if (targets_[i].empty() || static_cast<int32_t>(now_ms - next_ms_[i]) < 0) continue;
      next_ms_[i] = now_ms + STREAM_EVERY_MS;
      const Bytes &args = targets_[i][((now_ms - start_ms_) / SLOT_MS) % targets_[i].size()];
      seq = next_seq(seq);
      sent_41_++;
      return {CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, seq, args)};
    }
    return {};
  }
  uint32_t sent_41() const { return sent_41_; }
  uint32_t sent_session() const { return sent_session_; }

 private:
  uint32_t start_ms_;
  bool reset_pending_;
  uint32_t next_session_ms_;
  std::vector<Bytes> targets_[3];
  uint32_t next_ms_[3] = {0, 0, 0};
  uint32_t sent_41_ = 0;
  uint32_t sent_session_ = 0;
};

// What a master sends next: lease work when it's due, then housekeeping (if
// there is any), a poll otherwise. seq is the last tunnel sequence number
// used, advanced when one is.
inline Request next_request(LeaseKeeper &lease, const PollCursor &cursor, uint8_t &seq, uint32_t now_ms,
                            Housekeeping *housekeeping = nullptr) {
  LeaseAction a = lease.due(now_ms);
  if (a.kind == LeaseAction::none) {
    if (housekeeping) {
      Request r = housekeeping->due(now_ms, seq);
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
};

class Discovery {
 public:
  static const int SCAN_TRIES = 5;
  static const uint32_t BACKOFF_WAIT_MS = 800;  // GET_MAC with backoff: the CCA allows ~0.8 s a try

  Discovery(uint16_t factory_id, uint16_t target_id, bool scan_only = false)
      : state_(scan_only ? scan : claim), factory_(factory_id), target_(target_id) {}

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
          state_ = st_done;
          return next();
        }
        scans_sent_++;
        r.dest = factory_; r.cmd = CMD_GET_MAC; r.wait_ms = BACKOFF_WAIT_MS;
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
      default: break;
    }
  }

  bool done() const { return state_ == st_done; }
  bool failed() const { return state_ == st_failed; }
  const Bytes &mac() const { return mac_; }
  // MACs of other TAPs that answered the scan, in the order heard.
  const std::vector<Bytes> &extra_taps() const { return extra_; }

 private:
  enum State { claim, get_mac, set_id, verify, scan, st_done, st_failed } state_;
  uint16_t factory_, target_;
  Bytes mac_;
  int scans_sent_ = 0;
  std::vector<Bytes> extra_;
};

// ---- what a master may send ------------------------------------------------

// yes: what the CCA sends in steady state, in the shapes it sends them.
// bench_only: reads and anything else, for a TAP on the bench.
// never: writes. The commands that change what a TAP stores or does (reset,
// store, ids, roster, whitelist, netrun) and anything carrying the setup key
// 37 24 92 66. Nothing in this code sends them, whatever the settings.
enum class Allow { yes, bench_only, startup_only, never };

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

inline bool may_send(uint16_t cmd, const Bytes &payload, bool bench, bool startup = false) {
  switch (classify_request(cmd, payload)) {
    case Allow::yes: return true;
    case Allow::bench_only: return bench || startup;
    case Allow::startup_only: return startup;
    case Allow::never: return false;
  }
  return false;
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
