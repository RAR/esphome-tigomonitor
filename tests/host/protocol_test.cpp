// Host test for components/tigo_monitor/tigo_protocol.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/protocol_test.cpp -o /tmp/protocol_test && /tmp/protocol_test
//
// Two kinds of check: frames built by hand and decoded end to end, and the
// pre-extraction implementations from tigo_monitor.cpp (the ref_ functions,
// copied verbatim apart from logging) compared against the header on random
// input, so the move out of the component changed no behaviour.
// Addresses and barcodes here are made up; none belong to a real installation.

#include "tigo_protocol.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::protocol;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-3f)

// ---- reference implementations (tigo_monitor.cpp before the extraction) ----

static std::string ref_remove_escape_sequences(const std::string &frame) {
  std::string result;
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
        default: result.push_back(frame[i]); result.push_back(next_byte); break;
      }
      i++;
    } else {
      result.push_back(frame[i]);
    }
  }
  return result;
}

static uint16_t ref_crc_table[256];
static void ref_generate_crc_table() {
  for (uint16_t i = 0; i < 256; ++i) {
    uint16_t crc = i;
    for (uint8_t j = 8; j > 0; --j) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    ref_crc_table[i] = crc;
  }
}
static uint16_t ref_compute_crc16_ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0x8408;
  for (size_t i = 0; i < length; i++) {
    uint8_t index = (crc ^ data[i]) & 0xFF;
    crc = (crc >> 8) ^ ref_crc_table[index];
  }
  crc = (crc >> 8) | (crc << 8);
  return crc;
}

static std::string ref_frame_to_hex_string(const std::string &frame) {
  std::string hex_str;
  for (unsigned char byte : frame) {
    char hex_chars[3];
    std::snprintf(hex_chars, sizeof(hex_chars), "%02X", byte);
    hex_str += hex_chars;
  }
  return hex_str;
}

static int ref_calculate_header_length(const std::string &hex_frame) {
  char status_hex[5] = {0};
  size_t n = std::min<size_t>(4, hex_frame.length());
  memcpy(status_hex, hex_frame.c_str(), n);
  unsigned int status = static_cast<unsigned int>(strtoul(status_hex, nullptr, 16));
  int length = 2;
  if ((status & (1 << 0)) == 0) length += 1;
  if ((status & (1 << 1)) == 0) length += 1;
  if ((status & (1 << 2)) == 0) length += 2;
  if ((status & (1 << 3)) == 0) length += 2;
  if ((status & (1 << 4)) == 0) length += 1;
  length += 1;
  length += 2;
  return length * 2;
}

static const uint8_t ref_tigo_crc_table[256] = {
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
  0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4
};
static char ref_compute_tigo_crc4(const std::string &hex_string) {
  uint8_t crc = 0x2;
  for (size_t i = 0; i < hex_string.length(); i += 2) {
    if (i + 1 >= hex_string.length()) break;
    uint8_t byte_val = std::stoul(hex_string.substr(i, 2), nullptr, 16);
    crc = ref_tigo_crc_table[byte_val ^ (crc << 4)];
  }
  return "GHJKLMNPRSTVWXYZ"[crc];
}

// ---- helpers for building frames ----

static std::string from_hex(const std::string &hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back((char) strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
  return out;
}

static std::string escape(const std::string &raw) {
  std::string out;
  for (char c : raw) {
    switch ((unsigned char) c) {
      case 0x7E: out += "\x7E"; out.push_back('\x00'); break;
      case 0x24: out += "\x7E\x01"; break;
      case 0x23: out += "\x7E\x02"; break;
      case 0x25: out += "\x7E\x03"; break;
      case 0xA4: out += "\x7E\x04"; break;
      case 0xA3: out += "\x7E\x05"; break;
      case 0xA5: out += "\x7E\x06"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

// hex body (no CRC) → bytes on the wire: CRC appended, escaped, delimited.
static std::string wire(const std::string &hex_body) {
  std::string raw = from_hex(hex_body);
  uint16_t crc = crc16(reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
  raw.push_back((char) (crc >> 8));
  raw.push_back((char) (crc & 0xFF));
  return std::string("\x7E\x07", 2) + escape(raw) + std::string("\x7E\x08", 2);
}

// A legacy power packet: addr 0042, vin 37.5 V, vout 40.0 V, duty 255,
// iin 8.0 A, temperature -1.0 C, slot 1A2B, RSSI 200.
static const std::string LEGACY_PACKET = "31" "0042" "0007" "00" "0D" "2EE" "190" "FF" "640" "FF6" "830064" "1A2B" "C8";
// Status word FFFF → 10-char header, so the payload starts at char 18.
static const std::string POWER_HEADER = "1234" "0149" "FFFF" "000000";

static void test_power_frame_end_to_end() {
  std::printf("power frame end to end\n");
  std::string new_packet = "31" "0043" "0008" "00" "0F" "2EE" "000" "80" "640" "0FA" "830064" "0001" "7E" "0000";
  std::string stream = "garbage" + wire(POWER_HEADER + LEGACY_PACKET + new_packet) + "more";

  FrameSplitter<std::string> splitter;
  std::vector<std::string> frames;
  for (char c : stream) splitter.feed(c, [&](const std::string &f) { frames.push_back(f); });
  CHECK(frames.size() == 1);
  if (frames.size() != 1) return;

  std::string frame = unescape(frames[0]);
  CHECK(checksum_ok(frame));
  frame.resize(frame.size() - 2);
  std::string hex = to_hex(frame);
  CHECK(frame_type(hex) == FrameType::power);

  std::vector<PowerReading> readings;
  bool clean = walk_power_packets(hex, [&](const std::string &type, const std::string &packet) {
    CHECK(type == "31");
    PowerReading r;
    CHECK(decode_power_packet(packet, 1.0f, r) == PowerDecode::ok);
    readings.push_back(r);
  });
  CHECK(clean);
  CHECK(readings.size() == 2);
  if (readings.size() != 2) return;

  const PowerReading &a = readings[0];
  CHECK(std::string(a.addr) == "0042");
  CHECK(std::string(a.pv_node_id) == "0007");
  CHECK(!a.is_new_format);
  CHECK_NEAR(a.voltage_in, 37.5f);
  CHECK_NEAR(a.voltage_out, 40.0f);
  CHECK(a.duty_cycle == 255);
  CHECK_NEAR(a.current_in, 8.0f);
  CHECK_NEAR(a.temperature, -1.0f);
  CHECK(std::string(a.slot_counter) == "1A2B");
  CHECK(a.rssi == 200);
  CHECK(a.is_optimizer);
  CHECK_NEAR(a.power_in, 300.0f);
  CHECK_NEAR(a.power_out, 320.0f);

  // vout 0 with vin > 1 V: a monitor-only module, output mirrors input.
  const PowerReading &b = readings[1];
  CHECK(b.is_new_format);
  CHECK(!b.is_optimizer);
  CHECK_NEAR(b.voltage_out, b.voltage_in);
  CHECK_NEAR(b.power_out, b.power_in);
  CHECK(b.duty_cycle == 255);
  CHECK_NEAR(b.temperature, 25.0f);
  CHECK(b.rssi == 0x7E);
}

static void test_calibration_and_short_packets() {
  std::printf("calibration and short packets\n");
  PowerReading r;
  CHECK(decode_power_packet(LEGACY_PACKET, 1.1f, r) == PowerDecode::ok);
  CHECK_NEAR(r.power_in, 330.0f);
  CHECK_NEAR(r.power_out, 352.0f);
  CHECK(decode_power_packet(LEGACY_PACKET.substr(0, 39), 1.0f, r) == PowerDecode::too_short);
  CHECK(decode_power_packet(std::string("31004200"), 1.0f, r) == PowerDecode::too_short_for_header);

  // A packet whose length field runs past the frame stops the walk.
  std::string hex = POWER_HEADER + LEGACY_PACKET.substr(0, 30);
  int seen = 0;
  CHECK(!walk_power_packets(hex, [&](const std::string &, const std::string &) { seen++; }));
  CHECK(seen == 0);
}

static void test_frame27() {
  std::printf("frame 27\n");
  std::string hex = std::string("1234") + "0B10" + "0000" + "0027" + "01" + "0002" + "0002" +
                    "04C05B40000ABCDE" + "0042" + "04C05B40000ABCDF" + "0043";
  std::string frame = unescape(wire(hex).substr(2, wire(hex).size() - 4));
  CHECK(checksum_ok(frame));
  frame.resize(frame.size() - 2);
  std::string back = to_hex(frame);
  CHECK(back == hex);
  CHECK(frame_type(back) == FrameType::command);
  CHECK(command_byte(back) == "27");

  Frame27Header h;
  std::vector<std::pair<std::string, std::string>> entries;
  CHECK(walk_frame27(back, COMMAND_PAYLOAD_OFFSET, h,
                     [&](const char *l, const char *a) { entries.emplace_back(l, a); }));
  CHECK(h.starting_index == 2);
  CHECK(h.num_entries == 2);
  CHECK(entries.size() == 2);
  if (entries.size() == 2) {
    CHECK(entries[0].first == "04C05B40000ABCDE" && entries[0].second == "0042");
    CHECK(entries[1].first == "04C05B40000ABCDF" && entries[1].second == "0043");
  }
  // A truncated entry is skipped rather than read past the end.
  entries.clear();
  CHECK(walk_frame27(back.substr(0, back.size() - 5), COMMAND_PAYLOAD_OFFSET, h,
                     [&](const char *l, const char *a) { entries.emplace_back(l, a); }));
  CHECK(entries.size() == 1);
  CHECK(!walk_frame27(back.substr(0, 20), COMMAND_PAYLOAD_OFFSET, h, [](const char *, const char *) {}));
}

static void test_splitter() {
  std::printf("frame splitter\n");
  FrameSplitter<std::string> s;
  std::vector<std::string> frames;
  auto collect = [&](const std::string &f) { frames.push_back(f); };
  // An end before any start is counted, not emitted.
  for (char c : std::string("ab\x7E\x08", 4)) s.feed(c, collect);
  CHECK(frames.empty());
  CHECK(s.ends_without_start() == 1);
  // Escaped bytes inside a frame (7E 00 is a literal 7E) do not end it.
  std::string f = std::string("\x7E\x07", 2) + "x" + std::string("\x7E\x00", 2) + "y" + std::string("\x7E\x08", 2);
  for (char c : f) s.feed(c, collect);
  CHECK(frames.size() == 1);
  if (!frames.empty()) CHECK(frames[0] == std::string("x\x7E\x00y", 4));
  // A start with no end eventually overflows and resets.
  for (char c : std::string("\x7E\x07", 2)) s.feed(c, collect);
  for (size_t i = 0; i < FrameSplitter<std::string>::MAX_BUFFER + 10; i++) s.feed('z', collect);
  CHECK(s.overflows() == 1);
  frames.clear();
  for (char c : wire(POWER_HEADER + LEGACY_PACKET)) s.feed(c, collect);
  CHECK(frames.size() == 1);
}

static void test_against_reference() {
  std::printf("matches the pre-extraction implementation\n");
  ref_generate_crc_table();
  std::mt19937 rng(74);
  const char hexd[] = "0123456789ABCDEF";
  const char interesting[] = {'\x7E', '\x00', '\x01', '\x06', '\x07', '\x24', '\xA5', 'Q'};
  for (int iter = 0; iter < 20000; iter++) {
    // Escape-heavy random bytes.
    std::string raw;
    size_t n = rng() % 40;
    for (size_t i = 0; i < n; i++) raw.push_back(rng() % 2 ? interesting[rng() % 8] : (char) (rng() % 256));
    CHECK(unescape(raw) == ref_remove_escape_sequences(raw));
    CHECK(to_hex(raw) == ref_frame_to_hex_string(raw));
    CHECK(crc16(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()) ==
          ref_compute_crc16_ccitt(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));

    std::string status;
    for (int i = 0; i < 4; i++) status.push_back(hexd[rng() % 16]);
    CHECK(header_length_chars(std::string("12340149") + status) == ref_calculate_header_length(status));

    std::string addr;
    for (int i = 0; i < 4; i++) addr.push_back(hexd[rng() % 16]);
    CHECK(crc4_char(addr.c_str(), addr.size()) == ref_compute_tigo_crc4(addr));
  }
}

int main() {
  test_power_frame_end_to_end();
  test_calibration_and_short_packets();
  test_frame27();
  test_splitter();
  test_against_reference();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
