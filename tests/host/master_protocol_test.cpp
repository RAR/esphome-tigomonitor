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

#include <algorithm>
#include <cstring>
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

  // A lease about to run out is checked again just after it should, not a
  // whole period later: the CCA's renewal came 2 s after the lease read 0.
  LeaseKeeper soon;
  CHECK(soon.due(0).kind == LeaseAction::check);
  soon.on_lease({true, 12, 44}, 0);
  CHECK(soon.due(12000).kind == LeaseAction::none);
  CHECK(soon.due(12500).kind == LeaseAction::check);
  soon.on_lease({false, 0, 44}, 12500);
  CHECK(soon.due(12500).kind == LeaseAction::renew);

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
  CHECK(classify_request(0x000A, {0x00}) == Allow::bench_only);
  CHECK(classify_request(0x0300, {0x00, 0x00}) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0023"))) == Allow::bench_only);

  // The writes never sent, in steady state or bench: store, clear-roster,
  // whitelist, netrun. (The re-addressing writes 0x0010/0x0014/0x003C are
  // startup_only; see test_startup_allow.)
  for (uint16_t cmd : {0x0012, 0x011E, 0x0182, 0x0058})
    CHECK(classify_request(cmd, {}) == Allow::never);
  CHECK(classify_request(0x000A, hex("37249266")) == Allow::never);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0037249266"))) == Allow::never);

  CHECK(may_send(CMD_POLL, poll_payload(1), false));
  CHECK(!may_send(0x0300, {}, false) && may_send(0x0300, {}, true));
  CHECK(!may_send(0x0058, {}, false) && !may_send(0x0058, {}, true));
}

// The rest of what the CCA was seen sending in 24 h: its bring-up reads,
// the empty 0x22 broadcasts, all five text queries, and the two writes it
// makes to optimizers that answer a certain way.
static void test_more_allow() {
  CHECK(classify_request(0x000A, {}) == Allow::yes);  // VERSION
  CHECK(classify_request(0x0E02, {}) == Allow::startup_only);
  CHECK(classify_request(0x0E02, {0x00}) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x00})) == Allow::startup_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x01})) == Allow::startup_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x02})) == Allow::bench_only);
  // 0x22 to the broadcast address and to the TAP, empty. Carrying anything
  // more is how taptap saw an older CCA send "PV off": never.
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x00})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x01})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x05})) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x01, 0x01})) == Allow::never);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x00, 0x00, 0x00})) == Allow::never);
  for (const char *q : {"^00Tests\r", "^00Mppt\r", "^00Info\r"}) {
    Bytes a = {0x00, 0x14};
    a.insert(a.end(), q, q + strlen(q));
    CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, a)) == Allow::yes);
  }
  // The writes: only the CCA's two, only with the writes setting.
  Bytes w255 = {0x00, 0x14};
  for (char c : std::string(QUERY_W255)) w255.push_back(static_cast<uint8_t>(c));
  Bytes smart = {0x00, 0x22};
  for (char c : std::string(QUERY_SMART_S)) smart.push_back(static_cast<uint8_t>(c));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, w255)) == Allow::optimizer_write);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, smart)) == Allow::optimizer_write);
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(0x06, 1, w255), true, true) &&
        may_send(CMD_TUNNEL, tunnel_request(0x06, 1, w255), false, false, /*writes=*/true));
  // Any other write-looking text is never sent: '#' commands, Smrt_ variants.
  Bytes w254 = w255;
  w254[w254.size() - 2] = '4';
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, w254)) == Allow::never);
  Bytes smart_n = smart;
  smart_n[2 + 16] = 'N';
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, smart_n)) == Allow::never);
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

// Fable review fixes.
static void test_review_fixes() {
  // A zero node count is never renewed, and the allow-list refuses it.
  LeaseKeeper zero;
  zero.due(0);
  zero.on_lease({false, 0, 0}, 0);
  CHECK(zero.due(0).kind != LeaseAction::renew);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_LEASE_RENEW, 1, lease_renewal_args(0))) == Allow::bench_only);

  // "Not running" with time left is not a reason to renew: only 0 s is.
  LeaseKeeper odd;
  odd.due(0);
  odd.on_lease({false, 300, 44}, 0);
  CHECK(odd.due(0).kind != LeaseAction::renew);
  CHECK(odd.due(5000).kind != LeaseAction::renew);

  // Renewals the TAP doesn't take stop after 3, flagged, until a reading
  // shows time on the lease again.
  LeaseKeeper stubborn;
  stubborn.due(0);
  uint32_t t = 0;
  int renewals = 0;
  for (int i = 0; i < 10; i++, t += 1000) {
    stubborn.on_lease({false, 0, 44}, t);
    if (stubborn.due(t).kind == LeaseAction::renew) renewals++;
  }
  CHECK(renewals == 3 && stubborn.stuck());
  stubborn.on_lease({true, 900, 44}, t);
  CHECK(!stubborn.stuck());
  stubborn.on_lease({false, 0, 44}, t + 900000);
  CHECK(stubborn.due(t + 900000).kind == LeaseAction::renew);

  // A check that gets no reply is retried a second later, up to 3 times.
  LeaseKeeper lost;
  CHECK(lost.due(0).kind == LeaseAction::check);
  lost.on_no_reply(0);
  CHECK(lost.due(999).kind == LeaseAction::none);
  CHECK(lost.due(1000).kind == LeaseAction::check);
  lost.on_no_reply(1000);
  CHECK(lost.due(2000).kind == LeaseAction::check);
  lost.on_no_reply(2000);
  CHECK(lost.due(3000).kind == LeaseAction::check);
  lost.on_no_reply(3000);
  CHECK(lost.due(4000).kind == LeaseAction::none);  // back to the 20 s period
  CHECK(lost.due(23000).kind == LeaseAction::check);

  // Poll replies carry the cursor: its low byte always, all of it in the
  // longer header (status bits 0-4 clear). From the capture: cursor 6FD6.
  ReplyCursor rc;
  CHECK(reply_cursor(hex("41ffcc4be7"), rc) && rc.low == 0xCC && !rc.full);
  CHECK(reply_cursor(hex("41e0000e000102006fd64c00"), rc) && rc.low == 0xD6 && rc.full && rc.cursor == 0x6FD6);
  PollCursor pc(0x6FCC);
  CHECK(pc.matches(Frame{0x9201, CMD_POLL_REPLY, hex("41ffcc4be7")}));
  CHECK(!pc.matches(Frame{0x9201, CMD_POLL_REPLY, hex("41ffcd4be7")}));
  // A full cursor that differs from ours is the TAP's: adopt it.
  PollCursor fresh(0x0000);
  CHECK(!fresh.matches(Frame{0x9201, CMD_POLL_REPLY, hex("41e0000e000102006fd64c00")}));
  fresh.on_reply(Frame{0x9201, CMD_POLL_REPLY, hex("41e0000e000102006fd64c00")});
  CHECK(fresh.next() == 0x6FD6 && fresh.resyncs() == 1);
}

// Gateway discovery: a TAP that comes up at the factory address is re-addressed
// to the configured gateway id, exactly as the CCA did at the roof power-up.
static void test_discovery() {
  Discovery d(0x1235, 0x1201);
  // 1. SET_485_ID broadcast to the factory address.
  DiscoveryRequest r = d.next();
  CHECK(r.send && r.dest == 0x0000 && r.cmd == 0x0014 && r.payload == hex("372492661235"));
  d.on_reply(Frame{0x8000, 0x0015, {}});
  // 2. GET_MAC to the factory address, which hands back the MAC.
  r = d.next();
  CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x0038 && r.payload.empty());
  d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  // 3. SET_ID_BY_MAC, that MAC to the configured id.
  r = d.next();
  CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x003C && r.payload == hex("3724926604c05b3000a1b2c31201"));
  d.on_reply(Frame{0x9235, 0x003D, {}});
  // 4. Verify with a read at the new id.
  r = d.next();
  CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x003A && r.payload.empty());
  d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  // 5. Then, as the CCA does, five more GET_MACs at the factory address,
  // waiting out each one's backoff, for a second TAP still there.
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) {
    r = d.next();
    CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x0038 && r.payload.empty() && r.wait_ms == 800);
  }
  r = d.next();
  CHECK(r.done && !r.failed && d.extra_taps().empty());

  // A verify reply at the wrong id fails rather than proceeding.
  Discovery bad(0x1235, 0x1201);
  bad.on_reply(Frame{0x8000, 0x0015, {}});
  bad.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  bad.on_reply(Frame{0x9235, 0x003D, {}});
  bad.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c39999")});
  CHECK(bad.next().failed);
}

// The scan after re-addressing: a TAP that answers at the factory address is
// a second one. It is noted, once, and nothing is sent to it.
static void test_discovery_scan() {
  Discovery d(0x1235, 0x1201);
  d.on_reply(Frame{0x8000, 0x0015, {}});
  d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  d.on_reply(Frame{0x9235, 0x003D, {}});
  d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) {
    DiscoveryRequest r = d.next();
    CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x0038);
    if (i < 3) d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000d4e5f61235")});  // the same one, three times
    if (i == 3) d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});  // our own MAC: not another TAP
  }
  CHECK(d.next().done);
  CHECK(d.extra_taps().size() == 1 && d.extra_taps()[0] == hex("04c05b3000d4e5f6"));

  // A TAP already at its id gets the scan alone: no re-addressing writes.
  Discovery s(0x1235, 0x1201, /*scan_only=*/true);
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) {
    DiscoveryRequest r = s.next();
    CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x0038 && r.payload.empty());
  }
  CHECK(s.next().done && s.extra_taps().empty());
}

// The discovery writes are allowed only in startup mode; the truly dangerous
// writes never are, startup or not.
static void test_startup_allow() {
  for (uint16_t cmd : {0x0014, 0x003C, 0x0010}) {  // SET_485_ID, SET_ID_BY_MAC, RESET
    CHECK(classify_request(cmd, hex("37249266")) == Allow::startup_only);
    CHECK(!may_send(cmd, hex("37249266"), false, false));
    CHECK(!may_send(cmd, hex("37249266"), true, false));   // bench isn't startup
    CHECK(may_send(cmd, hex("37249266"), false, true));    // startup only
  }
  // GET_MAC reads are available in startup (they're bench_only reads).
  CHECK(may_send(0x0038, {}, false, true) && may_send(0x003A, {}, false, true));
  // These stay never, even in startup: store, clear-roster, whitelist, netrun.
  for (uint16_t cmd : {0x0012, 0x011E, 0x0182, 0x0058}) {
    CHECK(classify_request(cmd, hex("37249266")) == Allow::never);
    CHECK(!may_send(cmd, hex("37249266"), true, true));
  }
  // A foreign command smuggling the key is still never.
  CHECK(classify_request(0x0200, hex("37249266")) == Allow::never);
  // Steady-state sends are unchanged by the startup flag.
  CHECK(may_send(CMD_POLL, poll_payload(1), false, false));
}

// A 0x41 as the CCA sends it, with an invented tail: the roof's isn't
// committed. tuid, then 00 00 00 <stream> 00 and 17 more bytes.
static Bytes args41(uint16_t tuid, uint8_t stream, uint8_t tail = 0x11) {
  Bytes a = {static_cast<uint8_t>(tuid >> 8), static_cast<uint8_t>(tuid), 0x00, 0x00, 0x00, stream, 0x00};
  for (int i = 0; i < 17; i++) a.push_back(static_cast<uint8_t>(tail + i));
  return a;
}

static Frame cca41(uint8_t seq, uint16_t tuid, uint8_t stream, uint8_t tail = 0x11) {
  Frame f;
  f.dest = 0x1201;
  f.cmd = CMD_TUNNEL;
  f.payload = tunnel_request(SUB_HOUSEKEEPING, seq, args41(tuid, stream, tail));
  return f;
}

static void test_housekeeping_allow() {
  CHECK(classify_request(CMD_SESSION, {0x01}) == Allow::yes);
  CHECK(classify_request(CMD_SESSION, {0x00}) == Allow::startup_only);
  CHECK(classify_request(CMD_SESSION, {0x02}) == Allow::bench_only);
  CHECK(classify_request(CMD_SESSION, {}) == Allow::bench_only);
  CHECK(!may_send(CMD_SESSION, {0x00}, false, false) && may_send(CMD_SESSION, {0x00}, false, true));
  for (uint8_t stream : {0x01, 0x02, 0x04})
    CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, 9, args41(0x0001, stream))) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, 9, args41(0x0001, 0x03))) == Allow::bench_only);
  Bytes longer = args41(0x0001, 0x01);
  longer.push_back(0);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, 9, longer)) == Allow::bench_only);
  Bytes marked = args41(0x0001, 0x01);
  marked[2] = 0x01;
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_HOUSEKEEPING, 9, marked)) == Allow::bench_only);
}

static void test_learner() {
  Learned41 l;
  CHECK(l.on_frame(cca41(1, 0x0001, 0x01)));
  CHECK(!l.on_frame(cca41(2, 0x0001, 0x01)));  // the same frame again, another seq
  CHECK(l.on_frame(cca41(3, 0x0027, 0x01)));
  CHECK(l.on_frame(cca41(4, 0x0009, 0x04)));
  // The TAP's reply, a request of another shape, and another sub teach nothing.
  Frame reply = cca41(5, 0x0028, 0x01);
  reply.dest = 0x9201;
  CHECK(!l.on_frame(reply));
  Frame odd = cca41(6, 0x0028, 0x03);
  CHECK(!l.on_frame(odd));
  Frame lease;
  lease.dest = 0x1201;
  lease.cmd = CMD_TUNNEL;
  lease.payload = tunnel_request(SUB_LEASE_CHECK, 7, {});
  CHECK(!l.on_frame(lease));
  CHECK(l.entries().size() == 3);
  CHECK(l.entries()[0].stream == 0x01 && l.entries()[0].tuid == 0x0001 && l.entries()[0].args == args41(0x0001, 0x01));
  // New content for a target replaces the old, in place.
  CHECK(l.on_frame(cca41(8, 0x0001, 0x01, 0x40)));
  CHECK(l.entries().size() == 3 && l.entries()[0].args == args41(0x0001, 0x01, 0x40));
  // Capped per stream.
  Learned41 many;
  for (int i = 0; i < 100; i++) many.on_frame(cca41(1, static_cast<uint16_t>(0x100 + i), 0x04));
  CHECK(many.entries().size() == Learned41::PER_STREAM);
  // Loading what was saved gives the same set, refusing malformed entries.
  Learned41 back;
  std::vector<Learned41::Entry> saved = l.entries();
  saved.push_back({0x01, 0x0055, hex("0055")});
  back.load(saved);
  CHECK(back.entries().size() == 3);
}

// Pull every request due at now_ms.
static std::vector<Request> drain(Housekeeping &hk, uint32_t now_ms, uint8_t &seq) {
  std::vector<Request> out;
  for (Request r = hk.due(now_ms, seq); r.cmd; r = hk.due(now_ms, seq)) out.push_back(r);
  return out;
}

static uint16_t tuid_of(const Request &r) { return static_cast<uint16_t>(r.payload[5] << 8 | r.payload[6]); }
static uint8_t stream_of(const Request &r) { return r.payload[10]; }

static void test_housekeeping() {
  Learned41 l;
  l.on_frame(cca41(1, 0x0027, 0x01, 0x21));
  l.on_frame(cca41(1, 0x0006, 0x02, 0x22));
  l.on_frame(cca41(1, 0x0009, 0x04, 0x24));
  std::vector<uint16_t> roster;
  for (uint16_t n = 0x02; n <= 0x29; n++) roster.push_back(n);
  uint8_t seq = 0xFE;
  Housekeeping hk(l.entries(), 1000, /*session_reset=*/false, /*seed=*/7);
  hk.set_targets(roster);
  // At the start: 0x0B00 01, then nothing until each stream's offset.
  std::vector<Request> r = drain(hk, 1000, seq);
  CHECK(r.size() == 1 && r[0].cmd == CMD_SESSION && r[0].payload == Bytes{0x01});
  r = drain(hk, 1000 + Housekeeping::OFFSET_MS[0], seq);
  CHECK(r.size() == 1 && r[0].cmd == CMD_TUNNEL && stream_of(r[0]) == 0x02);
  // The learned content, for a target from the roster.
  uint16_t first02 = r.empty() ? 0 : tuid_of(r[0]);
  CHECK(r.size() == 1 && r[0].payload == tunnel_request(SUB_HOUSEKEEPING, 0x01, args41(first02, 0x02, 0x22)));
  CHECK(first02 >= 0x02 && first02 <= 0x29);
  // Stream 01 starts at 0x0001, as the CCA's does.
  r = drain(hk, 1000 + 15000, seq);
  CHECK(r.size() == 2 && r[0].payload == tunnel_request(SUB_HOUSEKEEPING, 0x02, args41(0x0001, 0x01, 0x21)));
  CHECK(r.size() == 2 && stream_of(r[1]) == 0x04 && tuid_of(r[1]) >= 0x02 && tuid_of(r[1]) <= 0x29);
  uint16_t first04 = r.size() == 2 ? tuid_of(r[1]) : 0;
  CHECK(drain(hk, 1000 + 59000, seq).empty());
  // A minute on, the same targets again.
  r = drain(hk, 1000 + 75000, seq);
  CHECK(r.size() == 3 && tuid_of(r[0]) == first02 && tuid_of(r[1]) == 0x0001 && tuid_of(r[2]) == first04);
  // Each 10-minute slot, a new target per stream; stream 01 is back at
  // 0x0001 every other slot. Over 40 slots stream 04 visits all 40, once each.
  std::vector<uint16_t> seen04 = {first04};
  for (uint32_t slot = 1; slot < 40; slot++) {
    r = drain(hk, 1000 + slot * Housekeeping::SLOT_MS + 15000, seq);
    std::vector<Request> tunnels;
    for (const Request &q : r)
      if (q.cmd == CMD_TUNNEL) tunnels.push_back(q);
    CHECK(tunnels.size() == 3);
    if (tunnels.size() != 3) break;
    CHECK(slot % 2 ? tuid_of(tunnels[1]) != 0x0001 : tuid_of(tunnels[1]) == 0x0001);
    seen04.push_back(tuid_of(tunnels[2]));
    for (const Request &q : tunnels) CHECK(classify_request(q.cmd, q.payload) == Allow::yes);
  }
  std::sort(seen04.begin(), seen04.end());
  CHECK(seen04 == roster);
  CHECK(hk.sent_41() > 0 && hk.sent_session() >= 2);

  // A target that drops out of the roster is never picked again.
  Housekeeping shrink(l.entries(), 0, false, 3);
  shrink.set_targets(roster);
  drain(shrink, 15000, seq);
  shrink.set_targets({0x05});
  for (uint32_t slot = 1; slot < 8; slot++)
    for (const Request &q : drain(shrink, slot * Housekeeping::SLOT_MS + 15000, seq))
      if (q.cmd == CMD_TUNNEL) CHECK(tuid_of(q) == 0x0001 || tuid_of(q) == 0x05);

  // No optimizers heard yet: only stream 01's 0x0001.
  Learned41 only01;
  only01.on_frame(cca41(1, 0x0001, 0x01));
  Housekeeping empty(only01.entries(), 0, false, 1);
  r = drain(empty, 15000, seq);
  CHECK(r.size() == 2 && r[0].cmd == CMD_SESSION && tuid_of(r[1]) == 0x0001);
  // A stream with nothing learned sends the built-in content; a learned one
  // sends what was learned.
  empty.set_targets(roster);
  r = drain(empty, Housekeeping::SLOT_MS + 15000, seq);
  CHECK(r.size() == 3);
  const Bytes learned01 = args41(0, 0x01);
  for (const Request &q : r) {
    Bytes content(q.payload.begin() + 7, q.payload.end());
    Bytes want = stream_of(q) == 0x01 ? Bytes(learned01.begin() + 2, learned01.end())
                                      : Housekeeping::default_content(stream_of(q));
    CHECK(content == want);
  }

  // After a re-address, the CCA's first bring-up sends 00 then 01.
  Housekeeping fresh(l.entries(), 0, /*session_reset=*/true);
  r = drain(fresh, 0, seq);
  CHECK(r.size() == 2 && r[0].payload == Bytes{0x00} && r[1].payload == Bytes{0x01});
  // Nothing learned: the built-in content on all three streams.
  Housekeeping bare({}, 0, false);
  bare.set_targets(roster);
  r = drain(bare, 0, seq);
  CHECK(r.size() == 1 && r[0].cmd == CMD_SESSION);
  r = drain(bare, 15000, seq);
  CHECK(r.size() == 3);
  for (const Request &q : r)
    CHECK(Bytes(q.payload.begin() + 7, q.payload.end()) == Housekeeping::default_content(stream_of(q)));
  // Every hour, 0x0B00 01 again.
  drain(bare, Housekeeping::SESSION_EVERY_MS - 1, seq);
  r = drain(bare, Housekeeping::SESSION_EVERY_MS, seq);
  CHECK(r.size() == 1 && r[0].payload == Bytes{0x01});
}

static void test_next_request_housekeeping() {
  Learned41 l;
  l.on_frame(cca41(1, 0x0001, 0x01));
  Housekeeping hk(l.entries(), 0, false);
  LeaseKeeper lease;
  PollCursor cursor(0);
  uint8_t seq = 0;
  // The lease comes first, then housekeeping, then polls.
  CHECK(next_request(lease, cursor, seq, 0, &hk).payload == tunnel_request(SUB_LEASE_CHECK, 1, {}));
  CHECK(next_request(lease, cursor, seq, 0, &hk).cmd == CMD_SESSION);
  CHECK(next_request(lease, cursor, seq, 0, &hk).cmd == CMD_POLL);
  Request r = next_request(lease, cursor, seq, 6000, &hk);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_HOUSEKEEPING, 2, args41(0x0001, 0x01)));
  // Without it, as before.
  CHECK(next_request(lease, cursor, seq, 7000).cmd == CMD_POLL);
}

int main() {
  test_discovery();
  test_discovery_scan();
  test_startup_allow();
  test_review_fixes();
  test_codec();
  test_lease();
  test_poll_cursor();
  test_lease_keeper();
  test_allow_list();
  test_more_allow();
  test_next_request();
  test_housekeeping_allow();
  test_learner();
  test_housekeeping();
  test_next_request_housekeeping();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
