#pragma once

// The Tigo CCA's Bluetooth command protocol, as the Tigo app speaks it
// (verified against the app, version 5.4.6).
//
//   request  = [reqNum] "cmd=X[ params]" '\0', written with response to one
//              characteristic of the Tigo service; reqNum is 1..255.
//   reply    = notifications on another characteristic: byte 0 echoes
//              reqNum, bytes 1-2 are overhead, the body is from byte 3 on,
//              and the reply is complete at a body byte '\0'.
//   session  = protected commands carry sid = SHA-512 hex of (uts + 159260),
//              uts being the nonce in the CCA's DEVICE_INFO reply.
//
// Header-only and free of ESPHome and BlueZ, so the ESP32 (tigo_server/
// tigo_cca_ble.cpp) and the Raspberry Pi build (linux/src/cca_ble.cpp) share
// it and the host tests cover it (tests/host/cca_ble_protocol_test.cpp). The
// hash itself stays with each caller: mbedTLS on the ESP32, OpenSSL on the Pi.

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace tigo_monitor {
namespace cca_ble {

static const char *const SERVICE_UUID = "d3dadcba-e4fa-4016-ba33-8bcc671999a7";
static const uint32_t SESSION_SEED = 159260;  // the app's constant

// What gets hashed for the session key.
inline std::string session_key_input(uint64_t uts) { return std::to_string(uts + SESSION_SEED); }

// The reply a protected command gets with a missing or stale sid.
template<class Str> bool is_unauthorized(const Str &reply) {
  return reply.find("Unauthorized request") != Str::npos;
}

// Parameters are space-separated on the wire, so values are encoded as the
// app's CK() does: encodeURIComponent, then !'()* too. Net effect: RFC 3986
// unreserved characters kept, everything else %XX in uppercase hex.
inline std::string uri_encode(const std::string &value) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 0x0F]);
    }
  }
  return out;
}

// "cmd=X", then params (already encoded) and the sid, in the app's order.
inline std::string command_text(const std::string &cmd, const std::string &params, const std::string &sid) {
  std::string text = "cmd=" + cmd;
  if (!params.empty()) text += " " + params;
  if (!sid.empty()) text += " sid=" + sid;
  return text;
}

// The request number after n; 0 is never used.
inline uint8_t next_request_number(uint8_t n) { return ++n == 0 ? 1 : n; }

// The bytes to write for one request. Bytes is any container of uint8_t.
template<class Bytes> Bytes request_frame(uint8_t request_number, const std::string &text) {
  Bytes out;
  out.reserve(text.size() + 2);
  out.push_back(request_number);
  for (char c : text) out.push_back(static_cast<uint8_t>(c));
  out.push_back(0);
  return out;
}

// Add one notification's body to reply. Returns false for a notification too
// short to carry one. Afterwards reply_complete() says whether that was the
// last piece; the terminating '\0' is left in place for the caller to drop.
template<class Buffer> bool append_reply_piece(Buffer &reply, const uint8_t *value, size_t len) {
  if (len < 4) return false;
  reply.insert(reply.end(), value + 3, value + len);
  return true;
}

template<class Buffer> bool reply_complete(const Buffer &reply) {
  return !reply.empty() && reply.back() == '\0';
}

}  // namespace cca_ble
}  // namespace tigo_monitor
}  // namespace esphome
