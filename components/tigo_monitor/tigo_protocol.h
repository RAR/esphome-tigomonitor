#pragma once

// Decoding the Tigo gateway's RS485 frames.
//
// Everything here is pure: bytes or hex text in, values out. It is shared by
// the ESP32 component (tigo_monitor.cpp) and the Linux daemon (linux/), and is
// header-only and free of ESPHome/IDF dependencies so it builds on the host
// (tests/host/protocol_test.cpp). Functions are templated on the string type
// because the firmware keeps frames in PSRAM-backed strings (frame_string) and
// the daemon uses std::string; both have the std::string interface.
//
// Pipeline, as the firmware runs it:
//   bytes between 7E 07 and 7E 08  → unescape → checksum_ok → strip CRC
//   → to_hex → frame_type(hex) → walk_power_packets / walk_frame27
//   → decode_power_packet for each type-31 packet.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace tigo_monitor {
namespace protocol {

// ---- framing --------------------------------------------------------------

// The escape sequences the gateway uses inside a frame (7E is the delimiter).
template<class S> S unescape(const S &frame) {
  S result;
  result.reserve(frame.length());
  for (size_t i = 0; i < frame.length(); ++i) {
    if (frame[i] == '\x7E' && i < frame.length() - 1) {
      char next_byte = frame[i + 1];
      switch (next_byte) {
        case '\x00': result.push_back('\x7E'); break;
        case '\x01': result.push_back('\x24'); break;
        case '\x02': result.push_back('\x23'); break;
        case '\x03': result.push_back('\x25'); break;
        case '\x04': result.push_back('\xA4'); break;
        case '\x05': result.push_back('\xA3'); break;
        case '\x06': result.push_back('\xA5'); break;
        default:
          result.push_back(frame[i]);
          result.push_back(next_byte);
          break;
      }
      i++;  // skip the escaped byte
    } else {
      result.push_back(frame[i]);
    }
  }
  return result;
}

// CRC-16/CCITT, reflected (polynomial 0x8408, initial value 0x8408), with the
// result byte-swapped — the frame carries it big-endian in its last 2 bytes.
inline const std::array<uint16_t, 256> &crc16_table() {
  static const std::array<uint16_t, 256> table = [] {
    std::array<uint16_t, 256> t{};
    for (uint16_t i = 0; i < 256; ++i) {
      uint16_t crc = i;
      for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
      t[i] = crc;
    }
    return t;
  }();
  return table;
}

inline uint16_t crc16(const uint8_t *data, size_t length) {
  const auto &table = crc16_table();
  uint16_t crc = 0x8408;
  for (size_t i = 0; i < length; i++) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
  return static_cast<uint16_t>((crc >> 8) | (crc << 8));
}

// The CRC carried in the frame's last 2 bytes; 0 if the frame is shorter.
template<class S> uint16_t carried_crc(const S &frame) {
  if (frame.length() < 2) return 0;
  return static_cast<uint16_t>((static_cast<uint8_t>(frame[frame.length() - 2]) << 8) |
                               static_cast<uint8_t>(frame[frame.length() - 1]));
}

template<class S> uint16_t computed_crc(const S &frame) {
  if (frame.length() < 2) return 0;
  return crc16(reinterpret_cast<const uint8_t *>(frame.c_str()), frame.length() - 2);
}

// An unescaped frame, CRC included. Frames over 10000 bytes are rejected: no
// real frame is that long, so it means the delimiters were lost.
template<class S> bool checksum_ok(const S &frame) {
  if (frame.length() < 2 || frame.length() > 10000) return false;
  return carried_crc(frame) == computed_crc(frame);
}

template<class S> S to_hex(const S &frame) {
  static const char digits[] = "0123456789ABCDEF";
  S hex;
  hex.reserve(frame.length() * 2);
  for (unsigned char byte : frame) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0x0F]);
  }
  return hex;
}

// A fixed-width hex field read in place. Unlike std::stoi it cannot throw on
// malformed bus data (exceptions are disabled on ESP-IDF, so a throw is a
// panic); out-of-range or non-hex input reads as 0 or a partial value.
template<class S> int parse_hex_field(const S &s, size_t pos, size_t len) {
  char buf[12];
  if (len >= sizeof(buf) || pos + len > s.length()) return 0;
  memcpy(buf, s.c_str() + pos, len);
  buf[len] = '\0';
  return static_cast<int>(strtol(buf, nullptr, 16));
}

// Splits a raw byte stream into frames: the bytes between a 7E 07 start and a
// 7E 08 end, still escaped and with the CRC. Feed it bytes; on_frame(frame) is
// called for each complete frame. Counts what it had to throw away.
template<class S> class FrameSplitter {
 public:
  static constexpr size_t MAX_BUFFER = 16384;

  template<class OnFrame> void feed(char byte, OnFrame on_frame) {
    if (buf_.size() >= MAX_BUFFER) {  // lost sync; start over
      buf_.clear();
      started_ = false;
      overflows_++;
    }
    buf_.push_back(byte);
    bool delimiter = buf_.size() >= 2 && buf_[buf_.size() - 2] == '\x7E';
    if (!delimiter) return;
    char kind = buf_.back();
    if (kind == '\x07' && !started_) {
      started_ = true;
      buf_.assign("\x7E\x07", 2);
    } else if (kind == '\x08') {
      if (started_ && buf_.size() >= 4) {
        S frame = buf_.substr(2, buf_.size() - 4);
        if (!frame.empty()) on_frame(frame);
      } else {
        ends_without_start_++;  // a frame whose start we never saw
      }
      started_ = false;
      buf_.clear();
    }
  }

  uint32_t ends_without_start() const { return ends_without_start_; }
  uint32_t overflows() const { return overflows_; }

 private:
  S buf_;
  bool started_ = false;
  uint32_t ends_without_start_ = 0;
  uint32_t overflows_ = 0;
};

// ---- frame contents ------------------------------------------------------

enum class FrameType { too_short, power, command, receive_request, unknown };

// hex: the frame as hex, CRC stripped. The type is the 4 chars at offset 4.
template<class S> FrameType frame_type(const S &hex) {
  if (hex.length() < 10) return FrameType::too_short;
  const char *seg = hex.c_str() + 4;
  if (memcmp(seg, "0149", 4) == 0) return FrameType::power;
  if (memcmp(seg, "0B10", 4) == 0 || memcmp(seg, "0B0F", 4) == 0) return FrameType::command;
  if (memcmp(seg, "0148", 4) == 0) return FrameType::receive_request;
  return FrameType::unknown;
}

// Length in hex chars of a power frame's header, from its 4-char status word
// (hex chars 8-11). The chars are read in their original order — every
// downstream offset is calibrated against that, so keep it byte-identical.
template<class S> int header_length_chars(const S &hex, size_t pos = 8) {
  char status_hex[5] = {0};
  size_t n = pos < hex.length() ? hex.length() - pos : 0;
  if (n > 4) n = 4;
  memcpy(status_hex, hex.c_str() + pos, n);
  unsigned int status = static_cast<unsigned int>(strtoul(status_hex, nullptr, 16));
  int length = 2;  // status word
  if ((status & (1 << 0)) == 0) length += 1;
  if ((status & (1 << 1)) == 0) length += 1;
  if ((status & (1 << 2)) == 0) length += 2;
  if ((status & (1 << 3)) == 0) length += 2;
  if ((status & (1 << 4)) == 0) length += 1;
  length += 1;  // bit 5
  length += 2;  // bit 6
  return length * 2;
}

// Walks the packets in a power frame (hex, CRC stripped). on_packet(type, packet)
// gets the 2-char packet type and the whole packet as hex. Returns false if it
// stopped at a truncated packet, true if it reached the end cleanly.
template<class S, class OnPacket> bool walk_power_packets(const S &hex, OnPacket on_packet) {
  size_t start = 8 + header_length_chars(hex);
  if (start > hex.length()) return false;
  S payload = hex.substr(start);
  size_t pos = 0;
  while (pos < payload.length()) {
    if (pos + 14 > payload.length()) return false;
    int length = parse_hex_field(payload, pos + 12, 2);
    size_t packet_chars = static_cast<size_t>(length) * 2 + 14;
    if (pos + packet_chars > payload.length()) return false;
    on_packet(payload.substr(pos, 2), payload.substr(pos, packet_chars));
    pos += packet_chars;
  }
  return true;
}

// The command byte of a command frame (hex chars 14-15), e.g. "27"; payload
// starts at char 18. Layout: dest(4) type(4) len(4) cmd(4) seq(2) payload.
template<class S> S command_byte(const S &hex) { return hex.length() >= 16 ? hex.substr(14, 2) : S(); }
static constexpr size_t COMMAND_PAYLOAD_OFFSET = 18;

// Frame 27 — the gateway's node table, in chunks:
//   [starting_index:4 hex] [num_entries:4 hex] then per entry
//   [long_address:16 hex] [short_address:4 hex].
// on_entry(long_addr, short_addr) gets NUL-terminated buffers. Returns false
// if the header itself is missing.
struct Frame27Header {
  int starting_index = 0;
  int num_entries = 0;
};

template<class S, class OnEntry>
bool walk_frame27(const S &hex, size_t offset, Frame27Header &header, OnEntry on_entry) {
  if (offset + 8 > hex.length()) return false;
  header.starting_index = parse_hex_field(hex, offset, 4);
  header.num_entries = parse_hex_field(hex, offset + 4, 4);
  size_t pos = offset + 8;
  char long_addr[17];
  char addr[5];
  for (int i = 0; i < header.num_entries && pos + 20 <= hex.length(); i++, pos += 20) {
    memcpy(long_addr, hex.c_str() + pos, 16);
    long_addr[16] = '\0';
    memcpy(addr, hex.c_str() + pos + 16, 4);
    addr[4] = '\0';
    on_entry(static_cast<const char *>(long_addr), static_cast<const char *>(addr));
  }
  return true;
}

// ---- power packets ---------------------------------------------------------

// One optimizer's reading, decoded from a type-31 packet. Two layouts exist:
// legacy (data length 0x0D, pre-CCA 4.x) and new (0x0F, CCA 4.x+), which
// appends 2 pad bytes after RSSI but moves nothing — both need >= 40 chars.
struct PowerReading {
  char addr[5] = {0};          // short address, hex
  char pv_node_id[5] = {0};
  char slot_counter[5] = {0};
  bool is_new_format = false;
  int data_length = 0;
  float voltage_in = 0, voltage_out = 0;
  float current_in = 0, current_out = 0;
  float power_in = 0, power_out = 0;
  float temperature = 0;
  int duty_cycle = 0;          // 0-255
  float efficiency = 0;        // percent
  float load_factor = 0;       // duty cycle as 0..1
  int rssi = 0;
  bool is_optimizer = true;    // false for monitor-only (TS4-A-S) modules
};

enum class PowerDecode { ok, too_short_for_header, too_short };

// packet: the whole type-31 packet as hex. calibration multiplies every power
// value (power_calibration_ in the firmware).
template<class S> PowerDecode decode_power_packet(const S &packet, float calibration, PowerReading &r) {
  if (packet.length() < 14) return PowerDecode::too_short_for_header;
  memcpy(r.addr, packet.c_str() + 2, 4);
  memcpy(r.pv_node_id, packet.c_str() + 6, 4);
  r.data_length = parse_hex_field(packet, 12, 2);
  r.is_new_format = r.data_length == 15;
  if (packet.length() < 40) return PowerDecode::too_short;

  r.voltage_in = parse_hex_field(packet, 14, 3) * 0.05f;
  r.voltage_out = parse_hex_field(packet, 17, 3) * 0.10f;
  r.duty_cycle = parse_hex_field(packet, 20, 2);
  r.current_in = parse_hex_field(packet, 22, 3) * 0.005f;
  int temperature_raw = parse_hex_field(packet, 25, 3);
  if (temperature_raw & 0x800) temperature_raw -= 0x1000;  // 12-bit two's complement
  r.temperature = temperature_raw * 0.1f;
  memcpy(r.slot_counter, packet.c_str() + 34, 4);
  r.rssi = parse_hex_field(packet, 38, 2);

  // Monitor-only modules report no output voltage; their output mirrors input.
  bool monitor_only = r.voltage_out <= 0.001f && r.voltage_in > 1.0f;
  r.is_optimizer = !monitor_only;
  float duty_norm = static_cast<float>(r.duty_cycle) / 255.0f;
  if (duty_norm < 0.001f) duty_norm = 0.0f;
  r.power_in = r.voltage_in * r.current_in * calibration;

  if (monitor_only) {
    r.voltage_out = r.voltage_in;
    r.current_out = r.current_in;
    r.power_out = r.power_in;
    r.duty_cycle = 255;
    r.efficiency = 100.0f;
    r.load_factor = 1.0f;
  } else {
    r.current_out = duty_norm <= 0.0f ? r.current_in : r.current_in / duty_norm;
    r.power_out = r.voltage_out * r.current_out * calibration;
    r.efficiency = r.power_in > 0.0f ? (r.power_out / r.power_in) * 100.0f : 0.0f;
    r.load_factor = duty_norm;
  }
  return PowerDecode::ok;
}

// ---- addresses -------------------------------------------------------------

// The check character Tigo appends to a short address (e.g. "1234" → "1234K").
inline char crc4_char(const char *hex, size_t len) {
  static const uint8_t table[256] = {
    0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,
    0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,
    0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,
    0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,
    0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,
    0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,
    0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4,0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,
    0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,
    0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,
    0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,
    0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,
    0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,
    0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,
    0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,
    0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,
    0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4,
  };
  static const char chars[] = "GHJKLMNPRSTVWXYZ";
  uint8_t crc = 0x2;
  for (size_t i = 0; i + 1 < len; i += 2) {
    char byte_hex[3] = {hex[i], hex[i + 1], '\0'};
    uint8_t byte_val = static_cast<uint8_t>(strtoul(byte_hex, nullptr, 16));
    crc = table[static_cast<uint8_t>(byte_val ^ (crc << 4))];
  }
  return chars[crc];
}

}  // namespace protocol
}  // namespace tigo_monitor
}  // namespace esphome
