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
  uint16_t crc = protocol::crc16(body.data(), body.size());  // byte-swapped: high byte goes first
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

// ---- telemetry polls -------------------------------------------------------

inline Bytes poll_payload(uint16_t cursor) {
  return {0x00, 0x01, static_cast<uint8_t>(cursor >> 8), static_cast<uint8_t>(cursor), 0x04};
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
