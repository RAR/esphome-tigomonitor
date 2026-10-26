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

// Six consecutive polls from the capture, each with the TAP's reply: the
// cursor steps 0, 0, 0, 4, 4, 2, the number of power packets in each reply.
static void test_poll_cursor() {
  const char *replies[] = {
      "41ffcc4be7",
      "41ffcc4be9",
      "41ffcc4beb",
      "41fe04cc4bed3100260081620f32a10eae4e61878400644bb84801003100260081620f31710cb250b1878400644960480100310029003a68"
      "0f31a111b551917f83006444b03c0100310029003a680f31a111b551817f83006447083c0100",
      "41ffd04bf4310029003a680f31a10fb351517f83006449603c0100310029003a680f312111b751f17f8300644bb83c010031000c0082140f"
      "32018cff73315c0100644bb872010031000c0082140f31a18aff74715c0100644960720100",
      "41fe02d44bfc310028003f6e0f32010fb24fd1768400644bb8480100310028003f6e0f31510bb250b1768400644960480100",
  };
  const uint16_t next[] = {0x6FCC, 0x6FCC, 0x6FCC, 0x6FD0, 0x6FD4, 0x6FD6};
  PollCursor cursor(0x6FCC);
  CHECK(cursor.next() == 0x6FCC && !cursor.retry());
  CHECK(poll_payload(cursor.next(), cursor.retry()) == hex("00016fcc04"));
  for (int i = 0; i < 6; i++) {
    Frame reply{0x9201, CMD_POLL_REPLY, hex(replies[i])};
    cursor.on_reply(reply);
    CHECK(cursor.next() == next[i]);
  }
  // No reply: the cursor stays and the next poll is marked as a retry
  // (second byte 00), as the CCA does; the next answer clears it.
  cursor.on_timeout();
  CHECK(cursor.next() == 0x6FD6 && cursor.retry());
  CHECK(poll_payload(cursor.next(), cursor.retry()) == hex("00006fd604"));
  cursor.on_reply(Frame{0x9201, CMD_POLL_REPLY, hex("41e0000e000102006fd64c00")});
  CHECK(cursor.next() == 0x6FD6 && !cursor.retry());
  // A reply that isn't a power frame changes nothing, and the cursor wraps.
  cursor.on_reply(Frame{0x9201, CMD_TUNNEL_REPLY, hex("000e002ffd0101a0002c0028002c00")});
  CHECK(cursor.next() == 0x6FD6);
  PollCursor end(0xFFFF);
  end.on_reply(Frame{0x9201, CMD_POLL_REPLY, hex(replies[5])});
  CHECK(end.next() == 0x0001);
}

static void test_lease_keeper() {
  // A daemon that restarts mid-lease checks first; it never renews blind.
  LeaseKeeper k;
  CHECK(k.due(0).kind == LeaseAction::check);
  CHECK(!k.ever_read());
  k.on_lease({true, 416, 44}, 0);
  CHECK(k.ever_read() && k.last().remaining_s == 416);
  CHECK(k.due(1).kind == LeaseAction::none);
  CHECK(k.due(19999).kind == LeaseAction::none);
  CHECK(k.due(20000).kind == LeaseAction::check);

  // Lapsed: renew with the node count it reported, at most once a second,
  // until a reading shows time on the lease.
  const uint32_t t = 440000;
  k.on_lease({false, 0, 44}, t);
  LeaseAction a = k.due(t);
  CHECK(a.kind == LeaseAction::renew && a.node_count == 44);
  CHECK(k.due(t + 500).kind == LeaseAction::none);
  CHECK(k.due(t + 1000).kind == LeaseAction::renew);
  CHECK(k.renewals() == 2);
  k.on_lease({true, 900, 44}, t + 1100);
  CHECK(k.due(t + 2100).kind == LeaseAction::none);
  CHECK(k.due(t + 1100 + 20000).kind == LeaseAction::check);

  // Running but at 0 renews too, and a changed node count is carried.
  k.on_lease({true, 0, 45}, 600000);
  a = k.due(600000);
  CHECK(a.kind == LeaseAction::renew && a.node_count == 45);

  // No reply is no reading: it never causes a renewal.
  LeaseKeeper q;
  CHECK(q.due(0).kind == LeaseAction::check);
  q.on_no_reply(0);
  CHECK(q.due(500).kind == LeaseAction::none);
  CHECK(q.due(20000).kind == LeaseAction::check);
  CHECK(q.renewals() == 0);
}

static void test_allow_list() {
  // What the CCA sends in steady state, in the shapes it sends them.
  CHECK(classify_request(CMD_POLL, poll_payload(0x6FC4)) == Allow::yes);
  CHECK(classify_request(CMD_POLL, poll_payload(0x6FC4, true)) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_LEASE_CHECK, 7, {})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_LEASE_RENEW, 0x31, lease_renewal_args(44))) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x26, 0x2C, {0x00, 0x0E})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 0x05, hex("001c5e303056657273696f6e0d"))) == Allow::yes);

  // Near misses are not the same thing: a renewal for 901 s, an ASCII
  // command that isn't a ^00 read, a poll of the wrong length.
  Bytes odd = lease_renewal_args(44);
  odd[4] = 0x85;
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_LEASE_RENEW, 1, odd)) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, hex("001c5e3030536574"))) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, hex("001c"))) == Allow::bench_only);
  CHECK(classify_request(CMD_POLL, hex("00016fc40400")) == Allow::bench_only);
  // Reads and other subs the roof build doesn't use.
  CHECK(classify_request(0x000A, {}) == Allow::bench_only);
  CHECK(classify_request(0x0300, {0x00, 0x00}) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0023"))) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x00})) == Allow::bench_only);

  // Writes, by command or by the setup key, are never sent.
  for (uint16_t cmd : {0x0010, 0x0012, 0x0014, 0x003C, 0x011E, 0x0182, 0x0058})
    CHECK(classify_request(cmd, {}) == Allow::never);
  CHECK(classify_request(0x000A, hex("37249266")) == Allow::never);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0037249266"))) == Allow::never);

  CHECK(may_send(CMD_POLL, poll_payload(1), false));
  CHECK(!may_send(0x000A, {}, false) && may_send(0x000A, {}, true));
  CHECK(!may_send(0x0058, {}, false) && !may_send(0x0058, {}, true));
}

static void test_next_request() {
  LeaseKeeper lease;
  PollCursor cursor(0x6FC4);
  uint8_t seq = 0xFD;
  // The first thing a master sends is a lease check, with the next seq.
  Request r = next_request(lease, cursor, seq, 0);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_LEASE_CHECK, 0xFE, {}));
  // Then polls, with the cursor, until the next check is due.
  r = next_request(lease, cursor, seq, 10);
  CHECK(r.cmd == CMD_POLL && r.payload == poll_payload(0x6FC4));
  cursor.on_timeout();
  r = next_request(lease, cursor, seq, 20);
  CHECK(r.payload == poll_payload(0x6FC4, true));
  // A lapsed reading: the renewal goes out next. Seq skips FF and 00, as the
  // CCA's does.
  lease.on_lease({false, 0, 44}, 30);
  r = next_request(lease, cursor, seq, 40);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_LEASE_RENEW, 0x01, lease_renewal_args(44)));
  CHECK(next_seq(0xFE) == 0x01 && next_seq(0x01) == 0x02);
}

int main() {
  test_codec();
  test_lease();
  test_poll_cursor();
  test_lease_keeper();
  test_allow_list();
  test_next_request();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
