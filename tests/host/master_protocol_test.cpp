// Host test for components/tigo_monitor/tigo_master.h, the frames a bus master
// sends to the TAP and the replies it reads back.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/master_protocol_test.cpp -o /tmp/master_protocol_test && /tmp/master_protocol_test
//
// The byte strings are frames from a 20-minute capture of the reference rig's
// bus with its CCA running (gateway 0x1201). Only polls and lease frames are
// used: they carry no serials, barcodes or MACs.

#include "tigo_master.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::master;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static Bytes hex(const char *s) {
  Bytes out;
  for (; s[0] && s[1]; s += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(s, 2), nullptr, 16)));
  return out;
}

// The escaped body between 7E 07 and 7E 08, as FrameSplitter hands it over.
static std::string body_of(const Bytes &wire) {
  std::string all(wire.begin(), wire.end());
  size_t s = all.find("\x7E\x07"), e = all.rfind("\x7E\x08");
  return all.substr(s + 2, e - s - 2);
}

static void test_codec() {
  // A 0x0148 poll, cursor 0x6FC4, exactly as the CCA sent it. Its CRC low
  // byte is A4, which goes out escaped as 7E 04.
  CHECK(encode_frame(0x1201, 0x0148, poll_payload(0x6FC4)) == hex("00ffff7e071201014800016fc4047e04437e08"));
  // The CCA's lease renewal: seq 0x31, 44 nodes.
  CHECK(tunnel_request(0x2D, 0x31, lease_renewal_args(44)) == hex("0000002d31babe020384002c0100"));
  CHECK(encode_frame(0x1201, 0x0B0F, tunnel_request(0x2D, 0x31, lease_renewal_args(44))) ==
        hex("00ffff7e0712010b0f0000002d31babe020384002c0100be967e08"));

  Frame f;
  CHECK(decode_frame(body_of(hex("00ffff7e071201014800016fc4047e04437e08")), f));
  CHECK(f.dest == 0x1201 && f.cmd == 0x0148 && f.payload == hex("00016fc404"));
  // Round trip through bytes that need escaping.
  CHECK(decode_frame(body_of(encode_frame(0x1201, 0x0B0F, {0x7E, 0x24, 0x00, 0xA5})), f));
  CHECK(f.cmd == 0x0B0F && (f.payload == Bytes{0x7E, 0x24, 0x00, 0xA5}));
  // A flipped bit fails the CRC; a stub is too short to be a frame.
  std::string bad = body_of(hex("00ffff7e071201014800016fc4047e04437e08"));
  bad[5] ^= 0x01;
  CHECK(!decode_frame(bad, f));
  CHECK(!decode_frame(std::string("\x12\x01\x01", 3), f));
}

static void test_lease() {
  TunnelReply r;
  Lease l;
  // Running, 416 s left; counts A/B/C 44/40/44.
  CHECK(parse_tunnel_reply(hex("000e002ffd0101a0002c0028002c00"), r));
  CHECK(r.sub == 0x2F && r.seq == 0xFD);
  CHECK(parse_lease(r, l) && l.running && l.remaining_s == 416 && l.node_count == 44);
  // Lapsed: state 0, count A drops to 0, count C still 44.
  CHECK(parse_tunnel_reply(hex("000e002f0000000000000028002c00"), r));
  CHECK(parse_lease(r, l) && !l.running && l.remaining_s == 0 && l.node_count == 44);
  // Not a lease reply, and a truncated one.
  CHECK(parse_tunnel_reply(hex("000d0041fc"), r) && !parse_lease(r, l));
  CHECK(parse_tunnel_reply(hex("000e002ffd0101a0002c"), r) && !parse_lease(r, l));
  CHECK(!parse_tunnel_reply(hex("000e00"), r));
}

int main() {
  test_codec();
  test_lease();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
