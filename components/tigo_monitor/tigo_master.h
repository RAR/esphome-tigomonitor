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
//     Sub 0x2E reads the TAP's discovery window, sub 0x2D renews it; 0x41, 0x22 and
//     0x26 are housekeeping (Housekeeping), 0x06 the text queries to
//     optimizers (Queries), 0x0D part of the bring-up.
//   - 0x0B00, the hourly session frame; VERSION and 0x0E02 at bring-up; and
//     the re-addressing at the factory address (Discovery).
//
// Header-only and free of ESPHome, like tigo_protocol.h, so both builds can
// use it and tests/host/master_protocol_test.cpp covers it.

#include "tigo_protocol.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
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
// An H-series TAP's answer to a text query (sub 06) it couldn't deliver, in
// place of the 0x0B10: 00 02, with no seq. Seen on the bench (H1.0006,
// 2026-11-06) for a node powered off, and for one just after a RESET.
static const uint16_t CMD_NOT_TAKEN = 0x0146;
static const uint8_t SUB_DISCOVERY_START = 0x2D;
static const uint8_t SUB_DISCOVERY_STATUS = 0x2E;
// EraseFlash: wipes the TAP's optimizer table. Sent only by a removal the
// owner started (table_edit.h), BA BE, as the CCA sends it. The CCA's own UI
// names it (Mesh History, 2026-11-06); we first read it as "Stop Discovery",
// and before that 2D and 2B as a "run lease" that kept the optimizers
// producing, which the roof showed it doesn't.
static const uint8_t SUB_ERASE_FLASH = 0x2B;
static const uint8_t SUB_DISCOVERY_STATUS_REPLY = 0x2F;
// Sub 0x41 and command 0x0B00: the CCA's housekeeping (Housekeeping, below).
static const uint8_t SUB_HOUSEKEEPING = 0x41;
// The PV switch, though we first copied it as a "session" frame: one byte,
// 00 off, anything else on (the CCA sends 01). From the 4.0.3 CCA's
// PV-off updater (command 0x0B00, one payload byte; "OFF" when it is 0). The roof CCA sent 01 every hour and 00 then 01 after each
// of its reboots; the TAP answers 0x0B01 with the same byte (the roof
// capture: 0x0B00 [01] -> 0x0B01 [01]). Zero is off: never write a bare byte.
static const uint16_t CMD_SESSION = 0x0B00;
static const uint16_t CMD_SESSION_REPLY = 0x0B01;
static const uint8_t PV_OFF = 0x00;
static const uint8_t PV_ON = 0x01;

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

// ---- the discovery window --------------------------------------------------

// The 0x2F reply's data: state (01 open, 00 closed), seconds left (BE),
// then three counts: the count the last renewal carried (echoed), how many
// have joined, how many are in the TAP's table. On the rig they read
// 44/40/44 while running and 0/40/44 at lapse. Joined is the TAP's flag,
// kept through power cuts: a dead optimizer still counts.
struct DiscoveryWindow {
  bool running = false;
  uint16_t remaining_s = 0;
  uint16_t node_count = 0;  // in the table: what a renewal carries
  uint16_t requested = 0;   // after node_count, so {running, left, count} still initialises
  uint16_t joined = 0;
};

inline bool parse_discovery_window(const TunnelReply &r, DiscoveryWindow &out) {
  if (r.sub != SUB_DISCOVERY_STATUS_REPLY || r.data.size() < 9) return false;
  out.running = r.data[0] == 0x01;
  out.remaining_s = static_cast<uint16_t>(r.data[1] << 8 | r.data[2]);
  out.requested = static_cast<uint16_t>(r.data[3] << 8 | r.data[4]);
  out.joined = static_cast<uint16_t>(r.data[5] << 8 | r.data[6]);
  out.node_count = static_cast<uint16_t>(r.data[7] << 8 | r.data[8]);
  return true;
}

// The CCA's renewal, byte for byte: BA BE, 02, 900 s, the node count, 01 00.
// What BA BE, 02 and 01 00 mean isn't known; they are copied, not composed.
inline Bytes discovery_renewal_args(uint16_t node_count) {
  return {0xBA, 0xBE, 0x02, 0x03, 0x84, static_cast<uint8_t>(node_count >> 8), static_cast<uint8_t>(node_count),
          0x01, 0x00};
}

// What to do about the discovery window, as the CCA does it: read it every 20 s (or
// just after it should run out, if that's sooner) and renew only once a
// reading says 0 s are left. A master that restarts checks first, and one
// that stops calling due() lets the discovery window lapse.
//
// Guards for frames and loops the CCA never showed:
//   - no renewal carries a node count of 0, so a TAP with an empty table
//     never has its window opened by us (Phase 2's first table write is what
//     would make the count non-zero);
//   - "not running" with time left is not a reason to renew; only 0 s is;
//   - after RENEW_LIMIT renewals with no reading showing time on the discovery window,
//     stuck() says so until a reading shows time, and it renews only once
//     each RENEW_BACKOFF_MS.
// A check with no reply is retried a second later, up to CHECK_RETRIES
// times, so one lost reply doesn't leave the window closed for a whole period.
// set_renewing(false) is the owner's "let it close", for testing: it keeps
// reading the window and sends no renewal (and never 2B) until turned back on.
struct WindowAction {
  enum Kind { none, check, renew } kind = none;
  uint16_t node_count = 0;
};

class WindowKeeper {
 public:
  static const uint32_t CHECK_EVERY_MS = 20000;
  static const uint32_t RENEW_RETRY_MS = 1000;
  static const uint32_t EXPIRY_MARGIN_MS = 500;
  static const uint32_t CHECK_RETRY_MS = 1000;
  static const uint32_t CHECK_RETRIES = 3;
  static const uint32_t RENEW_LIMIT = 3;
  static const uint32_t RENEW_BACKOFF_MS = 30000;

  // What to send now. Returning an action counts it as sent.
  WindowAction due(uint32_t now_ms) {
    WindowAction a;
    bool out = ever_read_ && last_.remaining_s == 0 && last_.node_count > 0;
    // Past RENEW_LIMIT, one try each RENEW_BACKOFF_MS: a TAP that refused
    // while browning out takes a renewal once it is back.
    bool may = renews_unanswered_ < RENEW_LIMIT ? !renewed_ || now_ms - last_renew_ms_ >= RENEW_RETRY_MS
                                                 : now_ms - last_renew_ms_ >= RENEW_BACKOFF_MS;
    if (out && may && renewing_) {
      a.kind = WindowAction::renew;
      a.node_count = last_.node_count;
      renewed_ = true;
      last_renew_ms_ = now_ms;
      renews_unanswered_++;
      renewals_++;
      last_sent_ = WindowAction::renew;
    } else if (!checked_ || static_cast<int32_t>(now_ms - next_check_ms_) >= 0) {
      a.kind = WindowAction::check;
      checked_ = true;
      next_check_ms_ = now_ms + CHECK_EVERY_MS;
      last_sent_ = WindowAction::check;
    }
    return a;
  }
  // A reading counts as this period's check.
  void on_window(const DiscoveryWindow &l, uint32_t now_ms) {
    last_ = l;
    ever_read_ = true;
    checked_ = true;
    check_retries_ = 0;
    if (l.remaining_s > 0) renews_unanswered_ = 0;
    uint32_t until_out = l.remaining_s > 0 ? l.remaining_s * 1000u + EXPIRY_MARGIN_MS : CHECK_EVERY_MS;
    next_check_ms_ = now_ms + (until_out < CHECK_EVERY_MS ? until_out : CHECK_EVERY_MS);
  }
  void on_no_reply(uint32_t now_ms) {
    if (last_sent_ == WindowAction::check && check_retries_ < CHECK_RETRIES) {
      check_retries_++;
      next_check_ms_ = now_ms + CHECK_RETRY_MS;
    }
  }

  void set_renewing(bool on) { renewing_ = on; }
  bool renewing() const { return renewing_; }
  const DiscoveryWindow &last() const { return last_; }
  uint32_t renewals() const { return renewals_; }
  bool ever_read() const { return ever_read_; }
  bool stuck() const { return renews_unanswered_ >= RENEW_LIMIT; }

 private:
  DiscoveryWindow last_;
  bool ever_read_ = false;
  bool checked_ = false;
  bool renewed_ = false;
  bool renewing_ = true;
  WindowAction::Kind last_sent_ = WindowAction::none;
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
  bool table_edit = false;  // from a TableAdd (table_edit.h), the only source of a table write
};

// ---- housekeeping ----------------------------------------------------------

// Two things the CCA sends that the discovery window and polls don't cover, from a 24 h
// capture of the reference rig's bus:
//   - tunnel sub 0x41, the args a target Tuid then 00 00 00 <stream> 00 and 17
//     more bytes. Three streams (01, 02, 04), each sent once a minute, each
//     moving to its next target every 10 minutes; between them they reached
//     every optimizer. The 17 bytes never changed in 24 h, across CCA
//     reboots. What they mean isn't known, so they are copied, not composed.
//   - 0x0B00 with 01, every hour; after the CCA reboots, 00 then 01 once.
//   - tunnel sub 0x22, empty, to the broadcast address then the TAP, a second
//     apart, a second after each 0x0B00 01 (the PV switch, CMD_SESSION).
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

// The PV state the master holds, and every 0x0B00 that isn't housekeeping's.
// A change goes out in the next request slot, as the CCA's PV-OFF overrides
// its queue. The TAP's reply must carry the byte that was sent; one that
// doesn't (or no reply) is sent again RETRY_MS later, up to RETRIES times,
// then unconfirmed() says so until a reply matches. While off is held it
// re-asserts 00 every HOURLY_MS if hourly (no housekeeping sends the hourly
// 0x0B00), and Housekeeping takes its byte from value() (set_pv), so no 01
// goes out while off is held. An unconfirmed hourly 01 from housekeeping
// with PV on is left as before: no retries.
//
// silent: a master that took over without hearing any 0x0B00 doesn't know
// what the TAP was last told, so it says nothing about PV, neither at once
// nor hourly, and Housekeeping drops its 0x0B00s too, but for a bring-up's
// after a re-address. The owner's set_off ends it.
class PvSwitch {
 public:
  static const uint32_t RETRY_MS = 5000;
  static const uint32_t RETRIES = 5;
  static const uint32_t HOURLY_MS = 3600000;

  PvSwitch(bool off, bool hourly, uint32_t now_ms, bool silent = false)
      : off_(off), hourly_(hourly), silent_(silent), pending_(off && !silent), due_ms_(now_ms),
        next_hourly_ms_(now_ms + HOURLY_MS) {}

  void set_off(bool off, uint32_t now_ms) {
    off_ = off;
    silent_ = false;
    pending_ = true;
    due_ms_ = now_ms;
    next_hourly_ms_ = now_ms + HOURLY_MS;
    awaiting_ = false;
    retries_ = 0;
    confirmed_ = unconfirmed_ = false;
  }
  bool off() const { return off_; }
  bool silent() const { return silent_; }
  uint8_t value() const { return off_ ? PV_OFF : PV_ON; }

  // The 0x0B00 due now, or one with cmd 0. One at a time: the next waits
  // for on_reply.
  Request due(uint32_t now_ms) {
    if (awaiting_ || silent_) return {};
    if (!pending_ && hourly_ && off_ && static_cast<int32_t>(now_ms - next_hourly_ms_) >= 0) {
      pending_ = true;
      due_ms_ = now_ms;
      retries_ = 0;
    }
    if (!pending_ || static_cast<int32_t>(now_ms - due_ms_) < 0) return {};
    if (static_cast<int32_t>(now_ms - next_hourly_ms_) >= 0) next_hourly_ms_ = now_ms + HOURLY_MS;
    pending_ = false;
    awaiting_ = true;
    last_sent_ = value();
    return {CMD_SESSION, {value()}};
  }
  // Every 0x0B00 sent, whoever built it.
  void on_reply(const Request &req, bool ok, const Frame &reply, uint32_t now_ms) {
    if (req.cmd != CMD_SESSION || req.payload.size() != 1) return;
    uint8_t sent = req.payload[0];
    bool ours = awaiting_ && sent == last_sent_;
    if (ours) awaiting_ = false;
    bool answered = ok && reply.cmd == CMD_SESSION_REPLY && reply.payload.size() == 1;
    if (answered) last_reply_ = reply.payload[0];
    if (sent != value()) return;  // the bring-up's 00 with PV on, or a superseded send
    if (answered && reply.payload[0] == sent) {
      confirmed_ = true;
      unconfirmed_ = false;
      retries_ = 0;
      return;
    }
    if (!ours && !off_) return;
    confirmed_ = false;
    if (retries_ < RETRIES) {
      retries_++;
      pending_ = true;
      due_ms_ = now_ms + RETRY_MS;
    } else {
      unconfirmed_ = true;
    }
  }

  bool confirmed() const { return confirmed_; }
  bool unconfirmed() const { return unconfirmed_; }
  int last_sent() const { return last_sent_; }
  int last_reply() const { return last_reply_; }

 private:
  bool off_;
  bool hourly_;
  bool silent_;
  bool pending_;
  uint32_t due_ms_;
  uint32_t next_hourly_ms_;
  bool awaiting_ = false;
  uint32_t retries_ = 0;
  bool confirmed_ = false, unconfirmed_ = false;
  int last_sent_ = -1, last_reply_ = -1;
};

// What a Housekeeping sends. The CCA's order after it re-addresses a TAP
// (13:47:47 in the capture): VERSION, 0x0E02, [0x0B00 00], 0x0D 00 00,
// 0x0B00 01, 0x0D 00 01, the 0x22 pair, then two walks of the node table.
struct HousekeepingPlan {
  bool send_41 = true;
  bool session = true;          // 0x0B00 01 at the start and every hour
  bool session_reset = false;   // 0x0B00 00 first, as after the CCA's own reboot
  bool bring_up_reads = false;  // VERSION, 0x0E02 and the 0x0D pair: after a re-address
  bool config_read = false;     // 0x0D 00 00 alone: the TAP's radio settings (owner's setting)
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
  static const uint32_t WALK_PAGE_WAIT_MS = 500;

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
    opening_reset_ = plan.session && plan.session_reset;
    if (opening_reset_) opening_.push_back({CMD_SESSION, 0, {PV_OFF}, true, 0});
    if (plan.bring_up_reads || plan.config_read) opening_.push_back({CMD_TUNNEL, 0x0D, {0x00, 0x00}, true, 0});
    if (plan.session) opening_.push_back({CMD_SESSION, 0, {PV_ON}, false, 0});
    if (plan.bring_up_reads) opening_.push_back({CMD_TUNNEL, 0x0D, {0x00, 0x01}, true, 0});
    if (plan.broadcast) {  // the second a second later, the walk starting between
      opening_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x00}, false, 0});
      later_.push_back({CMD_TUNNEL, 0x22, {0x00, 0x01}, false, now_ms + BROADCAST_GAP_MS});
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
  // Where the 0x0B00s take their byte: with PV off held, the hourly and
  // the opening's 01 become 00, and after a bring-up's 00 the 01 is dropped.
  void set_pv(const PvSwitch *pv) { pv_ = pv; }

  Request due(uint32_t now_ms, uint8_t &seq) {
    while (opening_next_ < opening_.size()) {
      Item it = opening_[opening_next_++];
      // An H-series TAP (the bench H1.0006) refuses 0x0E02 and answers the
      // 3.6 CCA's 0x000E; the version read just before tells which.
      if (it.cmd == 0x0E02 && version_.find("Version H") != std::string::npos) it.cmd = 0x000E;
      if (it.cmd == CMD_SESSION && pv_ && pv_->silent() && !plan_.bring_up_reads) continue;
      if (it.cmd == CMD_SESSION && pv_ && pv_->off() && it.args == Bytes{PV_ON}) {
        if (opening_reset_) continue;
        it.args = {PV_OFF};
      }
      return emit_(it, seq);
    }
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
      if (plan_.session && !(pv_ && pv_->silent())) return emit_({CMD_SESSION, 0, {pv_ ? pv_->value() : PV_ON}, false, 0}, seq);
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
    // No reply: ask for the same page once more before giving the walk up.
    if (!ok && !walk_retried_) {
      walk_retried_ = true;
      return;
    }
    TunnelReply t;
    if (!ok || !parse_tunnel_reply(reply.payload, t) || t.sub != 0x27 || t.data.size() < 4) return end_walk_();
    walk_retried_ = false;
    uint16_t start = static_cast<uint16_t>(t.data[0] << 8 | t.data[1]);
    uint16_t count = static_cast<uint16_t>(t.data[2] << 8 | t.data[3]);
    uint32_t next = static_cast<uint32_t>(start) + count;
    if (count == 0) {  // the end of the table
      walks_done_++;
      return end_walk_();
    }
    // A reply that doesn't move on.
    if (next <= walk_index_ || next > 0xFFFF || walk_pages_ >= WALK_MAX_PAGES) return end_walk_();
    walk_index_ = static_cast<uint16_t>(next);
  }

  uint32_t sent_41() const { return sent_41_; }
  uint32_t sent_session() const { return sent_session_; }
  uint32_t sent_broadcast() const { return sent_broadcast_; }
  uint32_t sent_walk_pages() const { return sent_walk_pages_; }
  uint32_t walks_done() const { return walks_done_; }
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
      // A page is up to ~150 bytes, ~40 ms on the wire before the TAP turns
      // round: the poll's 50 ms cut most of them off.
      if (it.sub == 0x26) r.wait_ms = WALK_PAGE_WAIT_MS;
    } else {
      r.payload = it.args;
      if (it.cmd == CMD_SESSION) sent_session_++;
    }
    return r;
  }
  void end_walk_() {
    walk_waiting_ = false;
    walk_retried_ = false;
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
  const PvSwitch *pv_ = nullptr;
  bool opening_reset_ = false;  // the opening has the bring-up's 00
  uint32_t start_ms_;
  uint32_t next_tick_ms_;
  uint32_t next_walk_ms_;
  std::vector<Item> opening_;
  size_t opening_next_ = 0;
  std::vector<Item> later_;
  int walks_left_ = 0;
  bool walk_waiting_ = false;
  bool walk_retried_ = false;
  uint32_t walk_sent_ms_ = 0;
  uint16_t walk_index_ = 0;
  int walk_pages_ = 0;
  uint32_t sent_broadcast_ = 0;
  uint32_t sent_walk_pages_ = 0;
  uint32_t walks_done_ = 0;
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
// Unlike the CCA, which rewrote the same nodes all day, the Smrt write goes
// once a node a session: an H9 answers N to every read and reports S only
// in its reply to the write itself (its firmware, 3.5.0 and 4.0.3), so a
// read can never show the write took.
// Each packet in a 0x0149 payload: its type (0x31 a power report, 0x07 an
// optimizer's text answer, ...), the node it came from, and its data.
template <class Fn> inline void for_each_poll_packet(const Bytes &payload, Fn fn) {
  if (payload.size() < 2) return;
  unsigned status = payload[0] << 8 | payload[1];
  size_t i = 2 + 3;
  static const uint8_t sizes[] = {1, 1, 2, 2, 1};
  for (unsigned bit = 0; bit < 5; bit++)
    if (!(status & (1u << bit))) i += sizes[bit];
  while (i + 7 <= payload.size()) {
    size_t end = i + 7 + payload[i + 6];
    if (end > payload.size()) break;
    fn(payload[i], static_cast<uint16_t>(payload[i + 1] << 8 | payload[i + 2]), payload.data() + i + 7, end - i - 7);
    i = end;
  }
}

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
    for_each_poll_packet(payload, [&](uint8_t type, uint16_t tuid, const uint8_t *data, size_t n) {
      if (tuid <= 0x0001 || (tuid & 0x8000)) return;
      heard_(tuid, now_ms);
      if (type == 0x07) answer_(tuid, std::string(data, data + n), now_ms);
    });
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
  // Each optimizer's last !Info and !Mppt answers, as text: how the CCA left
  // it set up (the !Mppt flag is S once its config items are all set).
  struct Setup {
    std::string info, mppt;
  };
  const std::map<uint16_t, Setup> &setups() const { return setups_; }

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
    if (k == info || k == mppt) {
      std::string t = text;
      while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
      (k == info ? setups_[tuid].info : setups_[tuid].mppt) = t;
    }
    if (k == info && f.size() > 8 && f[8] == "00") {
      if (writes_) add_(tuid, w255, now_ms);
      add_(tuid, info, now_ms + INFO_AGAIN_MS);
    }
    if (k == smrt && f.size() > 10 && f[10] == "N" && writes_ && smart_written_.insert(tuid).second) {
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
  std::map<uint16_t, Setup> setups_;
  std::map<uint16_t, uint32_t> heard_ms_;
  std::set<uint16_t> smart_written_;  // nodes sent the Smrt write this session
  uint32_t sent_reads_ = 0, sent_writes_ = 0, answers_ = 0;
};

// What a master sends next: discovery work when it's due, then housekeeping (if
// there is any), then queries (if there are any), a poll otherwise. seq is
// the last tunnel sequence number used, advanced when one is.
// A job the owner started (table_edit.h's TableAdd): asked after the
// window and before housekeeping, so it finishes in seconds, not minutes.
class RequestSource {
 public:
  virtual ~RequestSource() = default;
  virtual Request due(uint32_t now_ms, uint8_t &seq) = 0;
};

inline Request next_request(WindowKeeper &window, const PollCursor &cursor, uint8_t &seq, uint32_t now_ms,
                            Housekeeping *housekeeping = nullptr, Queries *queries = nullptr, PvSwitch *pv = nullptr,
                            RequestSource *job = nullptr) {
  // A PV switch goes in the next slot, ahead even of the discovery window: one
  // request's delay is nothing to a 900 s window.
  if (pv) {
    Request r = pv->due(now_ms);
    if (r.cmd) return r;
  }
  WindowAction a = window.due(now_ms);
  if (a.kind == WindowAction::none) {
    if (job) {
      Request r = job->due(now_ms, seq);
      if (r.cmd) return r;
    }
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
  if (a.kind == WindowAction::renew)
    return {CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_START, seq, discovery_renewal_args(a.node_count))};
  return {CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_STATUS, seq, {})};
}

// ---- gateway discovery -----------------------------------------------------

// A TAP that has lost power comes up at the factory RS485 address (0x1235 on
// the roof), not the id the CCA gave it. The CCA re-addresses it every boot:
// a SET_485_ID broadcast, GET_MAC to read its MAC, SET_ID_BY_MAC to assign the
// running id, then a read at that id to confirm. This drives the same four
// steps, then the scan and the hop below. It sends re-addressing writes, so
// BusMaster only runs it after a read has confirmed the TAP is actually at
// the factory address, and only in the explicit startup mode.
//
// Then, as the CCA does, it scans: five more GET_MACs at the factory address,
// each waiting out the TAP's reply backoff (the CCA's five take 3-4 s). A TAP
// that answers is a second one. Only one TAP is supported, so its MAC is
// noted for BusMaster to report, and nothing is ever sent to it by MAC.
// scan_only skips straight to the scan, for a TAP already at its id.
//
// Last, the CCA's hop, on every bring-up in the capture (16 of 16): the TAP
// to the id one up by SET_ID_BY_MAC, a read there, a RESET broadcast, ~5 s
// of silence, and a read at the configured id, where the RESET puts it back.
// Why the CCA does this isn't known. Not back after five reads: failed.
static const uint16_t CMD_SET_485_ID = 0x0014;
static const uint16_t CMD_GET_MAC = 0x0038;        // with backoff
static const uint16_t CMD_GET_MAC_NOW = 0x003A;    // no backoff; also the "where are you" read
static const uint16_t CMD_SET_ID_BY_MAC = 0x003C;
static const uint16_t CMD_RESET = 0x0010;
// The gateway's store, in the one form a bring-up sends: the key and nothing
// else, to the TAP at its new id. It commits the id, so the next RESET or
// power cycle brings the TAP back there (the 3.6 CCA, bench capture 2026-11-06).
static const uint16_t CMD_STORE = 0x0012;

struct DiscoveryRequest {
  bool send = false;
  bool done = false;
  bool failed = false;
  uint16_t dest = 0;
  uint16_t cmd = 0;
  Bytes payload;
  uint32_t wait_ms = 0;  // how long to wait for the reply; 0 = the usual
  uint32_t idle_ms = 0;  // send nothing at all for this long, then ask again
  // Every answer heard within wait_ms, not just the first: a GET_MAC at the
  // factory address that more than one TAP answers, each after its backoff.
  bool collect = false;
};

class Discovery {
 public:
  static const int SCAN_TRIES = 5;
  static const uint32_t BACKOFF_WAIT_MS = 800;  // GET_MAC with backoff: the CCA allows ~0.8 s a try
  // After its RESET the CCA leaves the bus quiet ~5 s, then reads once.
  static const uint32_t RESET_QUIET_MS = 5000;
  static const uint32_t BACK_WAIT_MS = 1000;
  static const int BACK_TRIES = 5;
  static const int STEP_TRIES = 5;  // each step before the RESET, unanswered this often: failed

  Discovery(uint16_t factory_id, uint16_t target_id, bool scan_only = false)
      : state_(scan_only ? scan : claim), scan_only_(scan_only), factory_(factory_id), target_(target_id) {}

  DiscoveryRequest next() {
    DiscoveryRequest r;
    if (state_ == st_failed) { r.failed = true; return r; }
    if (state_ == st_done) { r.done = true; return r; }
    // Each step before the RESET is asked STEP_TRIES times. The hop's two
    // then go on to the RESET, since the TAP may already be one id up, where
    // nothing looks for it again; the others fail.
    if (state_ == claim || state_ == get_mac || state_ == set_id || state_ == verify || state_ == hop ||
        state_ == hop_verify || state_ == back_factory || state_ == re_set_id || state_ == re_verify ||
        state_ == store) {
      if (state_ != tries_state_) {
        tries_state_ = state_;
        tries_ = 0;
      }
      if (tries_ == STEP_TRIES) {
        unanswered_ = step_name_(state_);
        // An unanswered STORE leaves the TAP at its id, only not kept there.
        state_ = state_ == hop || state_ == hop_verify ? reset : state_ == store ? st_done : st_failed;
        return next();
      }
      tries_++;
    }
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
      case reset:  // once, ack or not: the TAP may reset before it answers
        r.dest = 0x0000; r.cmd = CMD_RESET;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        state_ = quiet;
        break;
      case quiet:
        state_ = back;
        r.send = false;
        r.idle_ms = RESET_QUIET_MS;
        break;
      case back:
        if (back_sent_ == BACK_TRIES) {  // not at the configured id: back at the factory address?
          state_ = back_factory;
          return next();
        }
        back_sent_++;
        r.dest = target_; r.cmd = CMD_GET_MAC_NOW; r.wait_ms = BACK_WAIT_MS;
        break;
      // A TAP with no stored id (the bench H1.0006, with no optimizer table)
      // is back at the factory address after the RESET. Re-address it once
      // more, with no hop and no second RESET.
      case back_factory:
        r.dest = factory_; r.cmd = CMD_GET_MAC_NOW;
        break;
      case re_set_id:
        r.dest = factory_; r.cmd = CMD_SET_ID_BY_MAC;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        r.payload.insert(r.payload.end(), mac_.begin(), mac_.end());
        r.payload.push_back(static_cast<uint8_t>(target_ >> 8));
        r.payload.push_back(static_cast<uint8_t>(target_));
        break;
      case re_verify:
        r.dest = target_; r.cmd = CMD_GET_MAC_NOW;
        break;
      // Only for a TAP that dropped back, so one that keeps its id (the roof's
      // G8.65) never gets a frame its CCA never sent it.
      case store:
        r.dest = target_; r.cmd = CMD_STORE;
        r.payload = {0x37, 0x24, 0x92, 0x66};
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
          state_ = reset;  // at 1202 or not, the RESET puts it back at its stored id
        break;
      case back:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10 && (f.payload[8] << 8 | f.payload[9]) == target_)
          state_ = st_done;
        break;
      case back_factory:  // ours, at the factory address; another TAP's MAC is not re-addressed
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10) {
          bool ours = Bytes(f.payload.begin(), f.payload.begin() + 8) == mac_ &&
                      (f.payload[8] << 8 | f.payload[9]) == factory_;
          if (ours) dropped_back_ = true;
          state_ = ours ? re_set_id : st_failed;
        }
        break;
      case re_set_id:
        if (f.cmd == CMD_SET_ID_BY_MAC + 1) state_ = re_verify;
        break;
      case re_verify:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10 && (f.payload[8] << 8 | f.payload[9]) == target_)
          state_ = store;
        break;
      case store:
        if (f.cmd == CMD_STORE + 1) {
          stored_ = true;
          state_ = st_done;
        }
        break;
      default: break;
    }
  }

  bool done() const { return state_ == st_done; }
  bool failed() const { return state_ == st_failed; }
  // The last step that went unanswered after all its tries, or "" if none.
  const char *unanswered() const { return unanswered_; }
  const Bytes &mac() const { return mac_; }
  // True once the TAP came back at the factory address after the RESET and was re-addressed again.
  bool dropped_back() const { return dropped_back_; }
  // True once such a TAP acked the STORE of its new id.
  bool stored() const { return stored_; }
  // MACs of other TAPs that answered the scan, in the order heard.
  const std::vector<Bytes> &extra_taps() const { return extra_; }

 private:
  enum State {
    claim, get_mac, set_id, verify, scan, hop, hop_verify, reset, quiet, back,
    back_factory, re_set_id, re_verify, store, st_done, st_failed
  } state_;
  bool scan_only_;
  uint16_t factory_, target_;
  Bytes mac_;
  int scans_sent_ = 0;
  State tries_state_ = st_done;
  int tries_ = 0;
  const char *unanswered_ = "";

  static const char *step_name_(State s) {
    switch (s) {
      case claim: return "the SET_485_ID broadcast";
      case get_mac: return "GET_MAC at the factory address";
      case set_id: return "SET_ID_BY_MAC";
      case verify: return "the read at the new id";
      case hop: return "the hop to the id one up";
      case hop_verify: return "the read at the id one up";
      case back: return "the read at the configured id after the RESET";
      case back_factory: return "the reads at the configured id and the factory address after the RESET";
      case re_set_id: return "SET_ID_BY_MAC after the RESET";
      case re_verify: return "the read at the new id after the RESET";
      case store: return "the STORE of the new id";
      default: return "";
    }
  }
  int back_sent_ = 0;
  bool dropped_back_ = false;
  bool stored_ = false;
  std::vector<Bytes> extra_;
};

// ---- the herd: every TAP on the bus at once ---------------------------------

// The CCA's bring-up with more than one TAP (bench capture 2026-11-06,
// cca-two-taps-20261106-2035, and docs/superpowers/specs/2026-11-10-multi-tap-design.md):
//   1. five SET_485_ID broadcasts, ~0.75 s apart: every TAP to the factory id;
//   2. GET_MAC there, every answer heard (each TAP after its own backoff);
//   3. SET_ID_BY_MAC for each, there: a known MAC its saved id, a new one the
//      next free id from the first, skipping 1234 and 1235;
//   4. five more GET_MACs there (the scan); one that answers is placed too;
//   5. GET_MAC_NOW at each new id, in the order placed, and for a TAP that
//      dropped back in an earlier round, a key-only STORE straight after;
//   6. the hop: each TAP to the next id past every placed one, read there;
//   7. a RESET broadcast, ~5 s of quiet, and a read at each home id.
// A TAP not back at its id after the RESET has no stored id: the whole herd
// runs again (ROUNDS), with the STORE for it. Every TAP on the bus is moved,
// the working ones too, so data collection stops for the herd's ~30 s.
class Herd {
 public:
  static constexpr int CLAIMS = 5;
  static constexpr uint32_t CLAIM_EVERY_MS = 750;
  static constexpr int MAC_TRIES = 5;  // GET_MACs before giving up on hearing any TAP
  static constexpr int SCAN_TRIES = 5;
  static constexpr uint32_t BACKOFF_WAIT_MS = 800;
  static constexpr uint32_t RESET_QUIET_MS = 5000;
  static constexpr uint32_t BACK_WAIT_MS = 1000;
  static constexpr int BACK_TRIES = 2;  // the CCA read each once; one more for a slow TAP
  static constexpr int STEP_TRIES = 5;
  static constexpr int ROUNDS = 2;
  static constexpr uint16_t BROADCAST_A = 0x1234;  // a TAP takes both as its own; never an id
  static constexpr uint16_t BROADCAST_B = 0x1235;

  struct Known {
    Bytes mac;
    uint16_t id = 0;
  };
  struct Placed {
    Bytes mac;
    uint16_t id = 0;
    bool fresh = false;     // not in the saved map: given the next free id
    bool answered = false;  // back at its id after the last round's RESET
    bool stored = false;    // acked a STORE
    bool needs_store = false;
  };

  // adopt_new false: a MAC not in known is heard and left where it is, at
  // the factory address, unplaced (extras()); only known TAPs are moved on.
  Herd(uint16_t factory_id, uint16_t first_id, std::vector<Known> known, bool adopt_new = true)
      : factory_(factory_id), first_(first_id), known_(std::move(known)), adopt_new_(adopt_new) {}

  DiscoveryRequest next() {
    DiscoveryRequest r;
    if (state_ == st_failed) { r.failed = true; return r; }
    if (state_ == st_done) { r.done = true; return r; }
    switch (state_) {
      case claim:
        if (pause_) {
          pause_ = false;
          r.idle_ms = CLAIM_EVERY_MS;
          if (claims_ == CLAIMS) state_ = get_mac;
          return r;
        }
        claims_++;
        pause_ = true;
        r.send = true;
        r.dest = 0x0000; r.cmd = CMD_SET_485_ID;
        r.payload = {0x37, 0x24, 0x92, 0x66, static_cast<uint8_t>(factory_ >> 8), static_cast<uint8_t>(factory_)};
        return r;
      case get_mac:
        if (!placed_.empty()) {
          state_ = assign;
          return next();
        }
        if (mac_tries_ == MAC_TRIES) {
          unanswered_ = "GET_MAC at the factory address";
          state_ = st_failed;
          return next();
        }
        mac_tries_++;
        r.send = true; r.collect = true;
        r.dest = factory_; r.cmd = CMD_GET_MAC; r.wait_ms = BACKOFF_WAIT_MS;
        return r;
      case assign:
      case scan:
        // Each placed TAP not yet given its id, in the order heard.
        for (size_t i = 0; i < placed_.size(); i++)
          if (!given_[i]) {
            if (!try_(i)) {
              given_[i] = true;  // never acked: its verify will say
              return next();
            }
            current_ = i;
            r.send = true;
            r.dest = factory_; r.cmd = CMD_SET_ID_BY_MAC;
            r.payload = id_payload_(placed_[i].mac, placed_[i].id);
            return r;
          }
        if (state_ == assign) state_ = scan;
        if (scans_ == SCAN_TRIES) {
          state_ = verify;
          current_ = 0;
          return next();
        }
        scans_++;
        r.send = true; r.collect = true;
        r.dest = factory_; r.cmd = CMD_GET_MAC; r.wait_ms = BACKOFF_WAIT_MS;
        return r;
      case verify:
        if (current_ == placed_.size()) {
          state_ = hop;
          current_ = 0;
          return next();
        }
        if (!try_(current_)) {
          current_++;
          return next();
        }
        r.send = true;
        r.dest = placed_[current_].id; r.cmd = CMD_GET_MAC_NOW;
        return r;
      case store:
        if (!try_(current_)) {
          state_ = verify;
          current_++;
          return next();
        }
        r.send = true;
        r.dest = placed_[current_].id; r.cmd = CMD_STORE;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        return r;
      case hop:
        if (current_ == placed_.size()) {
          state_ = reset;
          return next();
        }
        if (!try_(current_)) {  // the RESET puts it back wherever it is
          current_++;
          return next();
        }
        r.send = true;
        r.dest = placed_[current_].id; r.cmd = CMD_SET_ID_BY_MAC;
        r.payload = id_payload_(placed_[current_].mac, hop_id_(current_));
        return r;
      case hop_verify:
        if (!try_(current_)) {
          state_ = hop;
          current_++;
          return next();
        }
        r.send = true;
        r.dest = hop_id_(current_); r.cmd = CMD_GET_MAC_NOW;
        return r;
      case reset:  // once, ack or not: the TAPs may reset before they answer
        state_ = quiet;
        r.send = true;
        r.dest = 0x0000; r.cmd = CMD_RESET;
        r.payload = {0x37, 0x24, 0x92, 0x66};
        return r;
      case quiet:
        state_ = back;
        current_ = 0;
        r.idle_ms = RESET_QUIET_MS;
        return r;
      case back:
        while (current_ < placed_.size() && (placed_[current_].answered || back_tries_ == BACK_TRIES)) {
          current_++;
          back_tries_ = 0;
        }
        if (current_ == placed_.size()) return round_over_();
        back_tries_++;
        r.send = true;
        r.dest = placed_[current_].id; r.cmd = CMD_GET_MAC_NOW; r.wait_ms = BACK_WAIT_MS;
        return r;
      default:
        return r;
    }
  }

  void on_reply(const Frame &f) {
    switch (state_) {
      case get_mac:
      case scan:
        if (f.cmd == CMD_GET_MAC + 1 && f.payload.size() >= 10) place_(Bytes(f.payload.begin(), f.payload.begin() + 8));
        break;
      case assign:
        if (f.cmd == CMD_SET_ID_BY_MAC + 1 && current_ < given_.size()) given_[current_] = true;
        break;
      case verify:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10 &&
            (f.payload[8] << 8 | f.payload[9]) == placed_[current_].id) {
          if (placed_[current_].needs_store) {
            state_ = store;
          } else {
            current_++;
          }
        }
        break;
      case store:
        if (f.cmd == CMD_STORE + 1) {
          placed_[current_].stored = true;
          state_ = verify;
          current_++;
        }
        break;
      case hop:
        if (f.cmd == CMD_SET_ID_BY_MAC + 1) state_ = hop_verify;
        break;
      case hop_verify:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10) {
          state_ = hop;
          current_++;
        }
        break;
      case back:
        if (f.cmd == CMD_GET_MAC_NOW + 1 && f.payload.size() >= 10 && current_ < placed_.size() &&
            (f.payload[8] << 8 | f.payload[9]) == placed_[current_].id &&
            Bytes(f.payload.begin(), f.payload.begin() + 8) == placed_[current_].mac)
          placed_[current_].answered = true;
        break;
      default: break;
    }
  }

  bool done() const { return state_ == st_done; }
  bool failed() const { return state_ == st_failed; }
  int round() const { return round_; }
  const char *unanswered() const { return unanswered_; }
  // Every TAP heard, its id, and whether it was back there at the end.
  const std::vector<Placed> &placed() const { return placed_; }
  // MACs heard that aren't known, left at the factory address (adopt_new false).
  const std::vector<Bytes> &extras() const { return extras_; }

 private:
  enum State { claim, get_mac, assign, scan, verify, store, hop, hop_verify, reset, quiet, back, st_done, st_failed };

  static Bytes id_payload_(const Bytes &mac, uint16_t id) {
    Bytes p = {0x37, 0x24, 0x92, 0x66};
    p.insert(p.end(), mac.begin(), mac.end());
    p.push_back(static_cast<uint8_t>(id >> 8));
    p.push_back(static_cast<uint8_t>(id));
    return p;
  }
  static bool reserved_(uint16_t id) { return id == BROADCAST_A || id == BROADCAST_B || id == 0 || id == 0xFFFF; }
  bool taken_(uint16_t id) const {
    for (const auto &k : known_)
      if (k.id == id) return true;
    for (const auto &p : placed_)
      if (p.id == id) return true;
    return false;
  }
  // A MAC heard at the factory address: its saved id, or the next free one.
  void place_(const Bytes &mac) {
    for (const auto &p : placed_)
      if (p.mac == mac) return;
    Placed p;
    p.mac = mac;
    for (const auto &k : known_)
      if (k.mac == mac) p.id = k.id;
    if (!p.id && !adopt_new_) {
      if (std::find(extras_.begin(), extras_.end(), mac) == extras_.end()) extras_.push_back(mac);
      return;
    }
    for (const auto &b : fresh_before_)
      if (b.mac == mac && b.fresh) p.fresh = true;
    if (!p.id) {
      p.fresh = true;
      uint16_t id = first_;
      while (reserved_(id) || taken_(id)) id++;
      p.id = id;
    }
    for (const auto &s : stores_)
      if (s == mac) p.needs_store = true;
    placed_.push_back(p);
    given_.push_back(false);
  }
  // The hop's id for placed i: the next past every placed id, one each, in order.
  uint16_t hop_id_(size_t i) const {
    uint16_t top = 0;
    for (const auto &p : placed_) top = std::max(top, p.id);
    uint16_t id = top;
    for (size_t k = 0; k <= i; k++) {
      id++;
      while (reserved_(id) || taken_(id)) id++;
    }
    return id;
  }
  // Counts a try at this step for placed i; false once it has had STEP_TRIES.
  bool try_(size_t i) {
    size_t key = static_cast<size_t>(state_) * 4096 + i;
    if (key != tries_key_) {
      tries_key_ = key;
      tries_ = 0;
    }
    if (tries_ == STEP_TRIES) return false;
    tries_++;
    return true;
  }
  DiscoveryRequest round_over_() {
    bool any = false, all = true;
    for (const auto &p : placed_) {
      any = any || p.answered;
      all = all && p.answered;
    }
    if (all || round_ == ROUNDS) {
      if (!any) unanswered_ = "the reads at the TAPs' ids after the RESET";
      state_ = any ? st_done : st_failed;
      return next();
    }
    // Another round, as the CCA's, with a STORE for each that dropped back.
    // The ids stay: this round's become the saved ones.
    for (const auto &p : placed_) {
      bool have = false;
      for (auto &k : known_)
        if (k.mac == p.mac) {
          k.id = p.id;
          have = true;
        }
      if (!have) known_.push_back({p.mac, p.id});
      if (!p.answered) stores_.push_back(p.mac);
    }
    fresh_before_ = placed_;  // new to the saved map in round 1 stays new
    round_++;
    placed_.clear();
    given_.clear();
    claims_ = mac_tries_ = scans_ = 0;
    back_tries_ = 0;
    tries_key_ = static_cast<size_t>(-1);
    current_ = 0;
    pause_ = false;
    state_ = claim;
    return next();
  }

  uint16_t factory_, first_;
  std::vector<Known> known_;
  bool adopt_new_;
  std::vector<Bytes> extras_;
  std::vector<Bytes> stores_;
  std::vector<Placed> placed_;
  std::vector<Placed> fresh_before_;
  std::vector<bool> given_;
  State state_ = claim;
  bool pause_ = false;
  int claims_ = 0, mac_tries_ = 0, scans_ = 0, round_ = 1;
  int tries_ = 0, back_tries_ = 0;
  size_t current_ = 0, tries_key_ = static_cast<size_t>(-1);
  const char *unanswered_ = "";
};

// ---- what a master may send ------------------------------------------------

// yes: what the CCA sends in steady state, in the shapes it sends them.
// bench_only: reads and anything else, for a TAP on the bench.
// never: writes. The commands that change what a TAP stores or does (reset,
// store, ids, roster, whitelist, netrun, its channel, network config and
// firmware, the table erase, any other table write) and anything carrying the setup key
// 37 24 92 66. Nothing in this code sends them, whatever the settings,
// except the bring-up's own: RESET, SET_485_ID, SET_ID_BY_MAC and the
// key-only STORE (startup_only).
// optimizer_write: the two settings the CCA writes to optimizers whose
// replies ask for it (Queries), sent only with that setting on.
// pv_off: the PV switch's 00, sent only while the owner holds PV off (or
// in a bring-up, as the CCA's after its reboot).
// table_write: the CCA's table write (sub 0x29) and erase (sub 0x2B) in the
// shapes it sent, sent only by an add or removal the owner started
// (table_edit.h), with master_table_writes on.
enum class Allow { yes, bench_only, startup_only, optimizer_write, pv_off, table_write, never };

// Sub 0x29's args as the CCA sent them on the bench (2026-11-06, 11-09): a
// count, then count x (MAC, TUID), 1 to 10 (the TAP's limit). Each TUID is a
// real id, 2..1023, with bit 15 set for a new record (listed, not joined;
// the TAP clears it when the optimizer joins) or clear for one written back
// as it was, joined, as the CCA rewrites a table.
static const uint8_t SET_TABLE_MAX_RECORDS = 10;
inline bool is_set_table_args(const Bytes &args) {
  if (args.empty() || args[0] < 1 || args[0] > SET_TABLE_MAX_RECORDS || args.size() != 1 + 10u * args[0]) return false;
  for (size_t i = 0; i < args[0]; i++) {
    uint16_t id = static_cast<uint16_t>(args[9 + 10 * i] << 8 | args[10 + 10 * i]) & 0x7FFF;
    if (id < 2 || id > 1023) return false;
  }
  return true;
}


inline Allow classify_request(uint16_t cmd, const Bytes &payload) {
  // The re-addressing writes a cold boot needs. Allowed only in startup mode,
  // and BusMaster only after a read confirms the TAP is at the factory address.
  static const uint16_t STARTUP_WRITES[] = {CMD_RESET, CMD_SET_485_ID, CMD_SET_ID_BY_MAC};
  for (uint16_t w : STARTUP_WRITES)
    if (cmd == w) return Allow::startup_only;
  static const uint8_t STORE_KEY[] = {0x37, 0x24, 0x92, 0x66};
  if (cmd == CMD_STORE && payload.size() == 4 && std::equal(STORE_KEY, STORE_KEY + 4, payload.begin()))
    return Allow::startup_only;
  // The writes nothing here ever sends: store, clear-roster, whitelist, the
  // rapid-shutdown netrun lever, and the gateway's mode (SET_GATEWAY_MODE,
  // 0x0054, and its response number 0x0055, from the CCA's opcode table).
  static const uint16_t NEVER[] = {0x0012, 0x011E, 0x0182, 0x0058, 0x0054, 0x0055};
  for (uint16_t w : NEVER)
    if (cmd == w) return Allow::never;
  // What the CCA's local UI sends to reconfigure a TAP (2026-11-06): SET_CHANNEL
  // (00 00 then a STORE turns its radio off for good), the channel scan (it
  // takes the TAP off its channel and the bus goes quiet for minutes), and the
  // TAP firmware family 0x0520-0x0527 (install, list, erase).
  if (cmd == 0x000C || cmd == 0x0070 || cmd == 0x0072 || (cmd >= 0x0520 && cmd <= 0x0527)) return Allow::never;
  // Any other command carrying the setup key is refused.
  static const uint8_t KEY[] = {0x37, 0x24, 0x92, 0x66};
  for (size_t i = 0; i + 4 <= payload.size(); i++)
    if (std::equal(KEY, KEY + 4, payload.begin() + i)) return Allow::never;

  // Reads: VERSION any time; 0x0E02 (0x000E on an H-series TAP) and the
  // GET_MACs only in the CCA's bring-up.
  if (cmd == 0x000A) return payload.empty() ? Allow::yes : Allow::bench_only;
  if (cmd == CMD_GET_MAC || cmd == CMD_GET_MAC_NOW) return payload.empty() ? Allow::startup_only : Allow::bench_only;
  if (cmd == 0x0E02 || cmd == 0x000E) return payload.empty() ? Allow::startup_only : Allow::bench_only;
  // The CCA's hourly 01; its 00 only follows its own reboot, so bring-up only.
  if (cmd == CMD_SESSION)
    return payload == Bytes{PV_ON} ? Allow::yes : payload == Bytes{PV_OFF} ? Allow::pv_off : Allow::bench_only;
  if (cmd == CMD_POLL)
    return payload.size() == 5 && payload[0] == 0x00 && payload[1] <= 0x01 && payload[4] == 0x04 ? Allow::yes
                                                                                               : Allow::bench_only;
  if (cmd != CMD_TUNNEL || payload.size() < 5 || payload[0] || payload[1] || payload[2]) return Allow::bench_only;
  Bytes args(payload.begin() + 5, payload.end());
  switch (payload[3]) {
    case 0x29:  // SetTable, the optimizer table write
      return is_set_table_args(args) ? Allow::table_write : Allow::never;
    case SUB_ERASE_FLASH:  // the whole table, exactly as the CCA sent it
      return args == Bytes{0xBA, 0xBE} ? Allow::table_write : Allow::never;
    case 0x1D:  // ResetNode
    case 0x13:  // PeriodicCfg Write
    case 0xE1:  // SetKnownNetworks
    case 0xE5:  // EraseStackTrace
    case 0x1B:  // unnamed in the CCA's UI; its neighbours are all writes
    // The CCA's other messages to an optimizer besides SetPgw (0x41): Own star,
    // Ignore star, Kick (evicts a node its TAP doesn't list), firmware request
    // and delivery, and Blacklist (makes a node forget a network for good). In
    // none of our captures (owner's analysis, 2026-11-10).
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
      return Allow::never;
    case SUB_DISCOVERY_STATUS:
      return args.empty() ? Allow::yes : Allow::bench_only;
    case SUB_DISCOVERY_START:
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
      // Any other write: a '#' command, or a '^' text in the write form
      // (name, '_', values), Smrt_ with other settings among them. A CCA's
      // sendbeacon CGI passes any text through, so "a CCA could send it"
      // proves nothing for these.
      if (text.rfind("#", 0) == 0 || (text.rfind("^", 0) == 0 && text.find('_') != std::string::npos))
        return Allow::never;
      return Allow::bench_only;
    }
    case 0x0D:  // GatewayCfg. The bring-up's pair of reads, 00 00 then 00 01.
      // A first byte of 01 is the write of the TAP's whole network config
      // (channel, PAN, key): how a CCA re-PANs a TAP.
      if (!args.empty() && args[0] != 0x00) return Allow::never;
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

// The bring-up's GatewayCfg read, 0x0D 00 00 in a tunnel request: the one
// bring-up frame the owner can ask for outside a bring-up (the TAP's radio
// settings). The write (01 00 + block) is never sent.
inline bool is_config_read(uint16_t cmd, const Bytes &payload) {
  return cmd == CMD_TUNNEL && payload.size() == 7 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 &&
         payload[3] == 0x0D && payload[5] == 0x00 && payload[6] == 0x00;
}

inline bool may_send(uint16_t cmd, const Bytes &payload, bool bench, bool startup = false, bool writes = false,
                     bool pv_off = false, bool table_writes = false) {
  switch (classify_request(cmd, payload)) {
    case Allow::yes: return true;
    case Allow::bench_only: return bench;  // startup is not bench: a bring-up sends only its own
    case Allow::startup_only: return startup;
    case Allow::optimizer_write: return writes;
    case Allow::pv_off: return pv_off || startup;
    case Allow::table_write: return table_writes;  // never bench or startup alone
    case Allow::never: return false;
  }
  return false;
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
