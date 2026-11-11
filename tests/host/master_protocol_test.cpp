// Host test for components/tigo_monitor/tigo_master.h, the frames a bus master
// sends to the TAP and the replies it reads back.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/master_protocol_test.cpp -o /tmp/master_protocol_test && /tmp/master_protocol_test
//
// The byte strings are frames from a 20-minute capture of the reference rig's
// bus with its CCA running (gateway 0x1201). Only polls and discovery frames are
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
  // The CCA's discovery renewal: seq 0x31, 44 nodes.
  CHECK(tunnel_request(0x2D, 0x31, discovery_renewal_args(44)) == hex("0000002d31babe020384002c0100"));
  CHECK(encode_frame(0x1201, 0x0B0F, tunnel_request(0x2D, 0x31, discovery_renewal_args(44))) ==
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

static void test_discovery_window() {
  TunnelReply r;
  DiscoveryWindow l;
  // Running, 416 s left; counts A/B/C 44/40/44.
  CHECK(parse_tunnel_reply(hex("000e002ffd0101a0002c0028002c00"), r));
  CHECK(r.sub == 0x2F && r.seq == 0xFD);
  CHECK(parse_discovery_window(r, l) && l.running && l.remaining_s == 416 && l.node_count == 44);
  // Lapsed: state 0, count A drops to 0, count C still 44.
  CHECK(parse_tunnel_reply(hex("000e002f0000000000000028002c00"), r));
  CHECK(parse_discovery_window(r, l) && !l.running && l.remaining_s == 0 && l.node_count == 44);
  // Not a discovery reply, and a truncated one.
  CHECK(parse_tunnel_reply(hex("000d0041fc"), r) && !parse_discovery_window(r, l));
  CHECK(parse_tunnel_reply(hex("000e002ffd0101a0002c"), r) && !parse_discovery_window(r, l));
  CHECK(!parse_tunnel_reply(hex("000e00"), r));
}

// Six consecutive polls from the capture, each with the TAP's reply: the
// cursor steps 0, 0, 0, 4, 4, 2, the number of power packets in each reply.
// The 0x2F reply, in order: flag, remaining, the count echoed from the
// renewal, joined, in the table (the owner's analysis; the roof's 44/40/44).
static void test_discovery_window_counts() {
  TunnelReply r;
  DiscoveryWindow l;
  r.sub = SUB_DISCOVERY_STATUS_REPLY;
  r.data = hex("010384002c0028002c");
  CHECK(parse_discovery_window(r, l) && l.running && l.remaining_s == 900);
  CHECK(l.requested == 44 && l.joined == 40 && l.node_count == 44);
  r.data = hex("010384000100000001");  // the bench: one in the table, not joined yet
  CHECK(parse_discovery_window(r, l) && l.requested == 1 && l.joined == 0 && l.node_count == 1);
  r.data = hex("010384002c0028002c");
  r.data.pop_back();  // 8 bytes: one short
  CHECK(!parse_discovery_window(r, l));
  CHECK(discovery_renewal_args(44) == hex("babe020384002c0100"));
}

// A TAP with an empty table reads 0 in the table, and the renewal never
// carries 0 (the CCA never sent it): a window there is never renewed.
static void test_no_renewal_for_an_empty_table() {
  WindowKeeper k;
  CHECK(k.due(0).kind == WindowAction::check);
  DiscoveryWindow closed;
  closed.running = false;
  closed.remaining_s = 0;
  closed.node_count = 0;
  k.on_window(closed, 1);
  for (uint32_t t = 1; t < 200000; t += 500) CHECK(k.due(t).kind != WindowAction::renew);
  CHECK(k.renewals() == 0);
}

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

// The owner's "let it close", for testing: a closed window is still read
// every period, never renewed, and nothing counts as refused; turned back on,
// the next due() renews.
static void test_window_keeper_not_renewing() {
  WindowKeeper k;
  k.set_renewing(false);
  CHECK(!k.renewing());
  CHECK(k.due(0).kind == WindowAction::check);
  k.on_window({false, 0, 44}, 0);
  for (uint32_t t = 1; t < 200000; t += 500) CHECK(k.due(t).kind != WindowAction::renew);
  CHECK(k.renewals() == 0 && !k.stuck());
  k.on_window({false, 0, 44}, 200000);
  CHECK(k.due(200500).kind == WindowAction::none);
  CHECK(k.due(220000).kind == WindowAction::check);
  k.set_renewing(true);
  WindowAction a = k.due(220001);
  CHECK(a.kind == WindowAction::renew && a.node_count == 44);
}

static void test_window_keeper() {
  // A daemon that restarts mid-window checks first; it never renews blind.
  WindowKeeper k;
  CHECK(k.due(0).kind == WindowAction::check);
  CHECK(!k.ever_read());
  k.on_window({true, 416, 44}, 0);
  CHECK(k.ever_read() && k.last().remaining_s == 416);
  CHECK(k.due(1).kind == WindowAction::none);
  CHECK(k.due(19999).kind == WindowAction::none);
  CHECK(k.due(20000).kind == WindowAction::check);

  // Lapsed: renew with the node count it reported, at most once a second,
  // until a reading shows time on the discovery window.
  const uint32_t t = 440000;
  k.on_window({false, 0, 44}, t);
  WindowAction a = k.due(t);
  CHECK(a.kind == WindowAction::renew && a.node_count == 44);
  CHECK(k.due(t + 500).kind == WindowAction::none);
  CHECK(k.due(t + 1000).kind == WindowAction::renew);
  CHECK(k.renewals() == 2);
  k.on_window({true, 900, 44}, t + 1100);
  CHECK(k.due(t + 2100).kind == WindowAction::none);
  CHECK(k.due(t + 1100 + 20000).kind == WindowAction::check);

  // Running but at 0 renews too, and a changed node count is carried.
  k.on_window({true, 0, 45}, 600000);
  a = k.due(600000);
  CHECK(a.kind == WindowAction::renew && a.node_count == 45);

  // A discovery window about to run out is checked again just after it should, not a
  // whole period later: the CCA's renewal came 2 s after the discovery read 0.
  WindowKeeper soon;
  CHECK(soon.due(0).kind == WindowAction::check);
  soon.on_window({true, 12, 44}, 0);
  CHECK(soon.due(12000).kind == WindowAction::none);
  CHECK(soon.due(12500).kind == WindowAction::check);
  soon.on_window({false, 0, 44}, 12500);
  CHECK(soon.due(12500).kind == WindowAction::renew);

  // No reply is no reading: it never causes a renewal.
  WindowKeeper q;
  CHECK(q.due(0).kind == WindowAction::check);
  q.on_no_reply(0);
  CHECK(q.due(500).kind == WindowAction::none);
  CHECK(q.due(20000).kind == WindowAction::check);
  CHECK(q.renewals() == 0);
}

static void test_allow_list() {
  // What the CCA sends in steady state, in the shapes it sends them.
  CHECK(classify_request(CMD_POLL, poll_payload(0x6FC4)) == Allow::yes);
  CHECK(classify_request(CMD_POLL, poll_payload(0x6FC4, true)) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_STATUS, 7, {})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_START, 0x31, discovery_renewal_args(44))) ==
        Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x26, 0x2C, {0x00, 0x0E})) == Allow::yes);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 0x05, hex("001c5e303056657273696f6e0d"))) == Allow::yes);

  // Near misses are not the same thing: a renewal for 901 s, an ASCII
  // command that isn't a ^00 read, a poll of the wrong length.
  Bytes odd = discovery_renewal_args(44);
  odd[4] = 0x85;
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_START, 1, odd)) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, hex("001c5e3030536574"))) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, hex("001c"))) == Allow::bench_only);
  CHECK(classify_request(CMD_POLL, hex("00016fc40400")) == Allow::bench_only);
  // Reads and other subs the roof build doesn't use.
  CHECK(classify_request(0x000A, {0x00}) == Allow::bench_only);
  CHECK(classify_request(0x0300, {0x00, 0x00}) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0023"))) == Allow::bench_only);

  // The writes never sent, in steady state or bench: store, clear-roster,
  // whitelist, netrun, and the gateway mode (SET_GATEWAY_MODE 0x0054, its
  // response 0x0055: the CCA's opcode table). (The re-addressing writes
  // 0x0010/0x0014/0x003C are startup_only; see test_startup_allow.)
  for (uint16_t cmd : {0x0012, 0x011E, 0x0182, 0x0058, 0x0054, 0x0055})
    CHECK(classify_request(cmd, {}) == Allow::never && !may_send(cmd, {}, true, true, true, true));
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
  // 0x000E, the H-series TAP's form of 0x0E02 (the 3.6 CCA's), the same way.
  CHECK(classify_request(0x000E, {}) == Allow::startup_only);
  CHECK(classify_request(0x000E, {0x00}) == Allow::bench_only);
  // STORE only as the CCA's key-only frame, and only in a bring-up.
  CHECK(classify_request(0x0012, hex("37249266")) == Allow::startup_only);
  CHECK(classify_request(0x0012, {}) == Allow::never);
  CHECK(classify_request(0x0012, hex("3724926600")) == Allow::never);
  CHECK(classify_request(0x0012, hex("feed")) == Allow::never);
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
  // Sub 0x2B is EraseFlash, the whole optimizer table. Never, bench or not,
  // but BA BE exactly as the CCA sends it, and that only as a table write
  // (an owner's removal, table_edit_test.cpp).
  for (const Bytes &args : {Bytes{}, hex("babe02038400300100")})
    CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x2B, 1, args)) == Allow::never &&
          !may_send(CMD_TUNNEL, tunnel_request(0x2B, 1, args), true, true, true));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x2B, 1, {0xBA, 0xBE})) == Allow::table_write &&
        !may_send(CMD_TUNNEL, tunnel_request(0x2B, 1, {0xBA, 0xBE}), true, true, true, true));
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
  // A '^' text in the write form (name, '_', values) is never sent unless it
  // is the one Smrt_ above: the CCA's sendbeacon CGI passes any text through,
  // so Mppt_, Discharge_ and the rest are reachable on a CCA and must not be
  // here, bench or not.
  for (const char *t : {"^00Mppt_1.1\r", "^00Discharge_01\r", "^00StartPWM_1\r", "^22Smrt_\r", "^00X_\r"}) {
    Bytes args = {0x00, 0x22};
    for (const char *c = t; *c; c++) args.push_back(static_cast<uint8_t>(*c));
    CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, args)) == Allow::never &&
          !may_send(CMD_TUNNEL, tunnel_request(0x06, 1, args), true, true, true));
  }
  // A read the CCA never sent stays bench-only: no '_', so not a write.
  Bytes other_read = {0x00, 0x22};
  for (char c : std::string("^00Status\r")) other_read.push_back(static_cast<uint8_t>(c));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x06, 1, other_read)) == Allow::bench_only);
}

// The frames the CCA's local UI sends to reconfigure a TAP (2026-11-06):
// never, in every mode, bench included.
static void test_tap_reconfiguring_frames_are_never_sent() {
  auto never_anywhere = [](uint16_t cmd, const Bytes &p) {
    return classify_request(cmd, p) == Allow::never && !may_send(cmd, p, true, true, true, true);
  };
  // GatewayCfg write: 01 00 and the 36-byte block. The two reads stay as they were.
  Bytes cfg = {0x01, 0x00};
  cfg.resize(2 + 36, 0x00);
  CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(0x0D, 3, cfg)));
  CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x01, 0x00})));
  CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x01})));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x00})) == Allow::startup_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x01})) == Allow::startup_only);
  // SET_CHANNEL, any payload; the channel scan; the TAP firmware family.
  for (const Bytes &p : {Bytes{}, Bytes{0x00, 0x00}, Bytes{0x20, 0x00}, Bytes{0x14, 0x00}})
    CHECK(never_anywhere(0x000C, p));
  CHECK(never_anywhere(0x0070, hex("010807fff800")));
  CHECK(never_anywhere(0x0070, hex("000807fff800")));
  CHECK(never_anywhere(0x0072, {}));
  for (uint16_t cmd = 0x0520; cmd <= 0x0527; cmd++) {
    CHECK(never_anywhere(cmd, {}));
    CHECK(never_anywhere(cmd, {0x00, 0x01}));
  }
  // The tunnel subs that write a TAP's or a node's config, or erase: ResetNode,
  // PeriodicCfg Write, SetKnownNetworks, EraseStackTrace, 0x1B, EraseFlash and
  // SetTable (Phase 2 would change this line, and only on the bench).
  for (uint8_t sub : {uint8_t{0x1D}, uint8_t{0x13}, uint8_t{0xE1}, uint8_t{0xE5}, uint8_t{0x1B}, SUB_ERASE_FLASH,
                      uint8_t{0x29}})
    for (const Bytes &args : {Bytes{}, Bytes{0x00, 0x02}, Bytes{0x00, 0x02, 0x80, 0x00}})
      CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(sub, 1, args)));
  CHECK(SUB_ERASE_FLASH == 0x2B);
  // The CCA's other messages to an optimizer besides SetPgw (0x41): Own star,
  // Ignore star, Kick (evicts a node its TAP doesn't list), firmware request
  // (0x45) and firmware delivery (0x46), and 0x47 beside them. In none of our
  // captures (owner's analysis, 2026-11-10).
  for (uint8_t sub = 0x42; sub <= 0x47; sub++)
    for (const Bytes &args : {Bytes{}, Bytes{0x00, 0x02}, Bytes{0x00, 0x02, 0x80, 0x00}})
      CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(sub, 1, args)));
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x40, 1, {})) == Allow::bench_only);
  // An optimizer's own Leave command makes it leave its network (owner's
  // analysis, 2026-11-10): never, in any of the text forms the CCA uses.
  for (const char *t : {"Leave", "Leave\r", "!Leave", "^00Leave", "^00Leave\r", "#Leave"}) {
    Bytes a = {0x00, 0x1c};
    a.insert(a.end(), t, t + strlen(t));
    CHECK(never_anywhere(CMD_TUNNEL, tunnel_request(0x06, 1, a)));
  }
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x48, 1, {})) == Allow::bench_only);
  // Neighbours stay as they were: the reads around them are bench-only.
  CHECK(classify_request(0x000B, {}) == Allow::bench_only);
  CHECK(classify_request(0x0071, {}) == Allow::bench_only);
  CHECK(classify_request(0x0528, {}) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x17, 1, {})) == Allow::bench_only);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x27, 1, {})) == Allow::bench_only);
}

static void test_next_request() {
  WindowKeeper window;
  PollCursor cursor(0x6FC4);
  uint8_t seq = 0xFD;
  // The first thing a master sends is a discovery check, with the next seq.
  Request r = next_request(window, cursor, seq, 0);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_DISCOVERY_STATUS, 0xFE, {}));
  // Then polls, with the cursor, until the next check is due.
  r = next_request(window, cursor, seq, 10);
  CHECK(r.cmd == CMD_POLL && r.payload == poll_payload(0x6FC4));
  cursor.on_timeout();
  r = next_request(window, cursor, seq, 20);
  CHECK(r.payload == poll_payload(0x6FC4, true));
  // A lapsed reading: the renewal goes out next. Seq skips FF and 00, as the
  // CCA's does.
  window.on_window({false, 0, 44}, 30);
  r = next_request(window, cursor, seq, 40);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_DISCOVERY_START, 0x01, discovery_renewal_args(44)));
  CHECK(next_seq(0xFE) == 0x01 && next_seq(0x01) == 0x02);
}

// Fable review fixes.
static void test_review_fixes() {
  // A zero node count is never renewed, and the allow-list refuses it.
  WindowKeeper zero;
  zero.due(0);
  zero.on_window({false, 0, 0}, 0);
  CHECK(zero.due(0).kind != WindowAction::renew);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(SUB_DISCOVERY_START, 1, discovery_renewal_args(0))) ==
        Allow::bench_only);

  // "Not running" with time left is not a reason to renew: only 0 s is.
  WindowKeeper odd;
  odd.due(0);
  odd.on_window({false, 300, 44}, 0);
  CHECK(odd.due(0).kind != WindowAction::renew);
  CHECK(odd.due(5000).kind != WindowAction::renew);

  // Renewals the TAP doesn't take stop after 3, flagged, until a reading
  // shows time on the discovery window again.
  WindowKeeper stubborn;
  stubborn.due(0);
  uint32_t t = 0;
  int renewals = 0;
  uint32_t last = 0;
  for (int i = 0; i < 10; i++, t += 1000) {
    stubborn.on_window({false, 0, 44}, t);
    if (stubborn.due(t).kind == WindowAction::renew) {
      renewals++;
      last = t;
    }
  }
  CHECK(renewals == 3 && stubborn.stuck());
  // Stuck is not for good: one more try each RENEW_BACKOFF_MS, still flagged,
  // so a TAP that refused while browning out is renewed once it is back. It
  // used to stop until the process restarted.
  for (int i = 0; i < 10; i++, t += 5000) {
    stubborn.on_window({false, 0, 44}, t);
    WindowAction a = stubborn.due(t);
    if (a.kind == WindowAction::renew) {
      renewals++;
      CHECK(t - last >= WindowKeeper::RENEW_BACKOFF_MS);
      last = t;
    }
  }
  CHECK(renewals == 4 && stubborn.stuck());
  stubborn.on_window({true, 900, 44}, t);
  CHECK(!stubborn.stuck());
  stubborn.on_window({false, 0, 44}, t + 900000);
  CHECK(stubborn.due(t + 900000).kind == WindowAction::renew);

  // A check that gets no reply is retried a second later, up to 3 times.
  WindowKeeper lost;
  CHECK(lost.due(0).kind == WindowAction::check);
  lost.on_no_reply(0);
  CHECK(lost.due(999).kind == WindowAction::none);
  CHECK(lost.due(1000).kind == WindowAction::check);
  lost.on_no_reply(1000);
  CHECK(lost.due(2000).kind == WindowAction::check);
  lost.on_no_reply(2000);
  CHECK(lost.due(3000).kind == WindowAction::check);
  lost.on_no_reply(3000);
  CHECK(lost.due(4000).kind == WindowAction::none);  // back to the 20 s period
  CHECK(lost.due(23000).kind == WindowAction::check);

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
// Walks a Discovery through the hop after its scan, checking each frame;
// true if it got there.
static bool finish_hop(Discovery &d) {
  int bad = failures;
  DiscoveryRequest r = d.next();
  CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x003C && r.payload == hex("3724926604c05b3000a1b2c31202"));
  d.on_reply(Frame{0x9201, 0x003D, {}});
  r = d.next();
  CHECK(r.send && r.dest == 0x1202 && r.cmd == 0x003A && r.payload.empty());
  d.on_reply(Frame{0x9202, 0x003B, hex("04c05b3000a1b2c31202")});
  r = d.next();
  CHECK(r.send && r.dest == 0x0000 && r.cmd == 0x0010 && r.payload == hex("37249266"));
  d.on_reply(Frame{0x8000, 0x0011, {}});
  r = d.next();
  CHECK(!r.send && !r.done && r.idle_ms == Discovery::RESET_QUIET_MS);
  r = d.next();
  CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x003A && r.wait_ms == Discovery::BACK_WAIT_MS);
  d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  return failures == bad;
}

// Up to the RESET, for the failure cases.
static void to_reset(Discovery &d) {
  d.on_reply(Frame{0x8000, 0x0015, {}});
  d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  d.on_reply(Frame{0x9235, 0x003D, {}});
  d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) d.next();
  d.next();
  d.on_reply(Frame{0x9201, 0x003D, {}});
  d.next();
  d.on_reply(Frame{0x9202, 0x003B, hex("04c05b3000a1b2c31202")});
  d.next();
  d.on_reply(Frame{0x8000, 0x0011, {}});
  d.next();  // the quiet
}

static void test_discovery_hop() {
  // The RESET goes once: no ack (the TAP reset before answering) still
  // moves on to the quiet, never a second RESET.
  Discovery once(0x1235, 0x1201);
  once.on_reply(Frame{0x8000, 0x0015, {}});
  once.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  once.on_reply(Frame{0x9235, 0x003D, {}});
  once.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) once.next();
  once.next();
  once.on_reply(Frame{0x9201, 0x003D, {}});
  once.next();
  once.on_reply(Frame{0x9202, 0x003B, hex("04c05b3000a1b2c31202")});
  CHECK(once.next().cmd == 0x0010);
  DiscoveryRequest q = once.next();
  CHECK(!q.send && q.idle_ms == Discovery::RESET_QUIET_MS);

  // Not back at the configured id after a few tries, nor at the factory
  // address: failed.
  Discovery lost(0x1235, 0x1201);
  to_reset(lost);
  for (int i = 0; i < Discovery::BACK_TRIES; i++) CHECK(lost.next().send);
  for (int i = 0; i < Discovery::STEP_TRIES; i++) {
    DiscoveryRequest r = lost.next();
    CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x003A);
  }
  CHECK(lost.next().failed);
  CHECK(std::string(lost.unanswered()) != "");

  // Back at the factory address after the RESET, as the bench H1.0006 is: a
  // TAP that keeps no stored id. Re-addressed once more, with no hop and no
  // second RESET, then done.
  Discovery dropped(0x1235, 0x1201);
  to_reset(dropped);
  for (int i = 0; i < Discovery::BACK_TRIES; i++) dropped.next();
  DiscoveryRequest r = dropped.next();
  CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x003A && r.payload.empty());
  dropped.on_reply(Frame{0x9235, 0x003B, hex("04c05b3000a1b2c31235")});
  r = dropped.next();
  CHECK(r.send && r.dest == 0x1235 && r.cmd == 0x003C && r.payload == hex("3724926604c05b3000a1b2c31201"));
  dropped.on_reply(Frame{0x9235, 0x003D, {}});
  r = dropped.next();
  CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x003A);
  dropped.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  // Then the key-only STORE the backup CCA sends (bench capture, 2026-11-06),
  // so the TAP keeps the id through its next RESET or power cycle.
  r = dropped.next();
  CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x0012 && r.payload == hex("37249266"));
  dropped.on_reply(Frame{0x9201, 0x0013, {}});
  CHECK(dropped.next().done && dropped.dropped_back() && dropped.stored());
  // An unanswered STORE leaves the TAP re-addressed: done, not stored.
  Discovery unstored(0x1235, 0x1201);
  to_reset(unstored);
  for (int i = 0; i < Discovery::BACK_TRIES; i++) unstored.next();
  unstored.next();
  unstored.on_reply(Frame{0x9235, 0x003B, hex("04c05b3000a1b2c31235")});
  unstored.next();
  unstored.on_reply(Frame{0x9235, 0x003D, {}});
  unstored.next();
  unstored.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  for (int i = 0; i < Discovery::STEP_TRIES; i++) CHECK(unstored.next().cmd == 0x0012);
  CHECK(unstored.next().done && !unstored.stored());
  // A TAP that comes back where it was put never takes that path.
  Discovery kept(0x1235, 0x1201);
  to_reset(kept);
  kept.next();
  kept.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  CHECK(kept.next().done && !kept.dropped_back() && !kept.stored());  // no STORE for a TAP that keeps its id
  // Another TAP's MAC at the factory address is not ours: failed, not re-addressed.
  Discovery other(0x1235, 0x1201);
  to_reset(other);
  for (int i = 0; i < Discovery::BACK_TRIES; i++) other.next();
  other.next();
  other.on_reply(Frame{0x9235, 0x003B, hex("04c05b3000ffffff1235")});
  CHECK(other.next().failed);
  // Back, but late: the third try answers.
  Discovery late(0x1235, 0x1201);
  to_reset(late);
  late.next();
  late.next();
  late.next();
  late.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  CHECK(late.next().done);
  // The hop not taken (the read at 1202 says another id): the RESET goes out
  // anyway, since it is what puts the TAP back at its stored id, and the
  // reads at 1201 decide.
  Discovery stuck(0x1235, 0x1201);
  stuck.on_reply(Frame{0x8000, 0x0015, {}});
  stuck.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  stuck.on_reply(Frame{0x9235, 0x003D, {}});
  stuck.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  for (int i = 0; i < Discovery::SCAN_TRIES; i++) stuck.next();
  stuck.next();
  stuck.on_reply(Frame{0x9201, 0x003D, {}});
  stuck.next();
  stuck.on_reply(Frame{0x9202, 0x003B, hex("04c05b3000a1b2c31201")});
  CHECK(stuck.next().cmd == 0x0010);
}

// Answers in the order Discovery asks for them, for the steps before `upto`.
// Steps: 0 claim, 1 get_mac, 2 set_id, 3 verify, 4 the scan, 5 hop, 6 hop_verify.
static void answer_until(Discovery &d, int upto) {
  if (upto > 0) d.on_reply(Frame{0x8000, 0x0015, {}});
  if (upto > 1) d.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  if (upto > 2) d.on_reply(Frame{0x9235, 0x003D, {}});
  if (upto > 3) d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
  if (upto > 4)
    for (int i = 0; i < Discovery::SCAN_TRIES; i++) d.next();
  if (upto > 5) {
    d.next();
    d.on_reply(Frame{0x9201, 0x003D, {}});
  }
}

static void test_discovery_lost_replies() {
  // A step whose reply never comes is asked STEP_TRIES times, then the
  // bring-up fails, so the core can say so and stop sending (it used to ask
  // forever, with no fault).
  for (int step = 0; step <= 3; step++) {
    Discovery d(0x1235, 0x1201);
    answer_until(d, step);
    DiscoveryRequest first = d.next();
    CHECK(first.send);
    for (int i = 1; i < Discovery::STEP_TRIES; i++) {
      DiscoveryRequest r = d.next();
      CHECK(r.send && r.dest == first.dest && r.cmd == first.cmd);
    }
    CHECK(d.next().failed);
    CHECK(std::string(d.unanswered()) != "");
  }
  // A late answer still counts: the third try answered, so on to the next step.
  Discovery late(0x1235, 0x1201);
  late.next();
  late.next();
  late.next();
  late.on_reply(Frame{0x8000, 0x0015, {}});
  DiscoveryRequest r = late.next();
  CHECK(r.send && r.cmd == 0x0038);
  // Each step gets its own tries: four lost claims and four lost GET_MACs
  // are not a failure.
  Discovery each(0x1235, 0x1201);
  for (int i = 0; i < Discovery::STEP_TRIES - 1; i++) each.next();
  each.on_reply(Frame{0x8000, 0x0015, {}});
  for (int i = 0; i < Discovery::STEP_TRIES - 1; i++) CHECK(each.next().cmd == 0x0038);
  each.on_reply(Frame{0x9235, 0x0039, hex("04c05b3000a1b2c31235")});
  CHECK(each.next().cmd == 0x003C);

  // The hop, or the read at the id one up, unanswered: the TAP may already be
  // at 1202, where nothing looks for it again, so the RESET that puts it back
  // goes out anyway, then the quiet and the reads at 1201.
  for (int step = 5; step <= 6; step++) {
    Discovery d(0x1235, 0x1201);
    answer_until(d, step);
    for (int i = 0; i < Discovery::STEP_TRIES; i++) CHECK(d.next().cmd == (step == 5 ? 0x003C : 0x003A));
    r = d.next();
    CHECK(r.send && r.dest == 0x0000 && r.cmd == 0x0010);
    r = d.next();
    CHECK(!r.send && r.idle_ms == Discovery::RESET_QUIET_MS);
    r = d.next();
    CHECK(r.send && r.dest == 0x1201 && r.cmd == 0x003A);
    d.on_reply(Frame{0x9201, 0x003B, hex("04c05b3000a1b2c31201")});
    CHECK(d.next().done);
  }
}

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
  // 6. The CCA's hop: the same MAC to the id one up, read there, a RESET
  // broadcast, 5 s of quiet, and the TAP is back at the configured id.
  CHECK(finish_hop(d));
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
  CHECK(finish_hop(d) && d.next().done);
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
  // The bring-up's GET_MAC reads, with no payload, are startup reads.
  CHECK(may_send(0x0038, {}, false, true) && may_send(0x003A, {}, false, true));
  CHECK(classify_request(0x0038, {}) == Allow::startup_only && classify_request(0x003A, {}) == Allow::startup_only);
  CHECK(classify_request(0x0038, {0x00}) == Allow::bench_only);
  // Startup is not bench: a command nothing here sends stays refused in a
  // bring-up. (Startup used to let every bench-only command through.)
  CHECK(!may_send(0x0300, {}, false, true));
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(0x41, 1, hex("0023")), false, true));
  CHECK(!may_send(0x0E02, {0x00}, false, true) && may_send(0x0E02, {0x00}, true, false));
  // These stay never, even in startup: clear-roster, whitelist, netrun, and
  // any STORE but the bring-up's key-only one.
  for (uint16_t cmd : {0x011E, 0x0182, 0x0058}) {
    CHECK(classify_request(cmd, hex("37249266")) == Allow::never);
    CHECK(!may_send(cmd, hex("37249266"), true, true));
  }
  CHECK(may_send(0x0012, hex("37249266"), false, true) && !may_send(0x0012, hex("37249266"), true, false));
  CHECK(!may_send(0x0012, hex("372492661201"), true, true));
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
  CHECK(classify_request(CMD_SESSION, {PV_OFF}) == Allow::pv_off);
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
  Frame check;
  check.dest = 0x1201;
  check.cmd = CMD_TUNNEL;
  check.payload = tunnel_request(SUB_DISCOVERY_STATUS, 7, {});
  CHECK(!l.on_frame(check));
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


static uint8_t sub_of(const Request &r) { return r.cmd == CMD_TUNNEL && r.payload.size() > 3 ? r.payload[3] : 0; }
static Bytes args_of(const Request &r) { return Bytes(r.payload.begin() + 5, r.payload.end()); }

// A node table page reply for a 0x26: start, count, and count entries.
static Frame page_reply(const Request &q, uint16_t start, uint16_t count) {
  Bytes p = {0x00, 0x0E, 0x00, 0x27, q.payload[4], static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start),
             static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count)};
  for (uint16_t i = 0; i < count; i++) {
    Bytes e = hex("04c05b4000000000");
    e.push_back(static_cast<uint8_t>((start + i) >> 8));
    e.push_back(static_cast<uint8_t>(start + i));
    p.insert(p.end(), e.begin(), e.end());
  }
  return Frame{0x9201, CMD_TUNNEL_REPLY, p};
}

// Sends what is due at now_ms, answering each 0x26 from a table of 40 nodes
// at 2..41 in pages of 12, as the roof TAP does.
static std::vector<Request> run(Housekeeping &hk, uint32_t now_ms, uint8_t &seq) {
  std::vector<Request> out;
  for (Request r = hk.due(now_ms, seq); r.cmd; r = hk.due(now_ms, seq)) {
    out.push_back(r);
    if (sub_of(r) == 0x26) {
      Bytes a = args_of(r);
      uint16_t idx = static_cast<uint16_t>(a[0] << 8 | a[1]);
      uint16_t start = idx < 2 ? 2 : idx;
      uint16_t count = start > 41 ? 0 : static_cast<uint16_t>(std::min(12, 42 - start));
      hk.on_reply(r, true, page_reply(r, start, count));
    } else {
      hk.on_reply(r, true, Frame{});
    }
  }
  return out;
}

static std::string shape(const std::vector<Request> &rs) {
  std::string s;
  char b[32];
  for (const Request &r : rs) {
    if (r.cmd == CMD_TUNNEL) {
      Bytes a = args_of(r);
      snprintf(b, sizeof(b), "%02x:", sub_of(r));
      s += b;
      for (size_t i = 0; i < a.size() && i < 2; i++) {
        snprintf(b, sizeof(b), "%02x", a[i]);
        s += b;
      }
    } else {
      snprintf(b, sizeof(b), "%04x:", r.cmd);
      s += b;
      for (uint8_t c : r.payload) {
        snprintf(b, sizeof(b), "%02x", c);
        s += b;
      }
    }
    s += r.bring_up ? "* " : " ";
  }
  return s;
}

// The CCA's own order after it re-addresses a TAP (13:47:47 in the capture),
// and its hourly tick. * marks the bring-up-only frames.
static void test_bring_up_plan() {
  HousekeepingPlan plan;
  plan.bring_up_reads = true;
  plan.session_reset = true;
  plan.send_41 = false;
  Housekeeping hk({}, 0, plan, 1);
  hk.set_targets({0x02, 0x03});
  uint8_t seq = 0;
  std::string got = shape(run(hk, 0, seq));
  const char *want =
      "000a: 0e02:* 0b00:00* 0d:0000* 0b00:01 0d:0001* 22:0000 "
      "26:0000 26:000e 26:001a 26:0026 26:002a 26:0000 26:000e 26:001a 26:0026 26:002a ";
  CHECK(got == want);
  if (got != want) std::printf("    got  %s\n    want %s\n", got.c_str(), want);
  // An H-series TAP (the bench H1.0006) refuses 0x0E02 and takes 0x000E, as
  // the 3.6 CCA sends it; the version read just before decides.
  for (const char *v : {"Mgate Version H1.0006\r", "Mgate Version G8.65\r"}) {
    Housekeeping h2({}, 0, plan, 1);
    uint8_t s2 = 0;
    Request r = h2.due(0, s2);
    CHECK(r.cmd == 0x000A);
    Frame vr{0x9201, 0x000B, Bytes(v, v + strlen(v))};
    h2.on_reply(r, true, vr);
    CHECK(h2.due(0, s2).cmd == (v[14] == 'H' ? 0x000E : 0x0E02));
  }
  // The second 0x22 a second after the first, as the CCA's (13:47:48, :49).
  CHECK(shape(run(hk, Housekeeping::BROADCAST_GAP_MS, seq)) == "22:0001 ");
  CHECK(hk.sent_walk_pages() == 10 && hk.sent_broadcast() == 2 && hk.sent_session() == 2);
  // A watchdog bring-up: no 00.
  plan.session_reset = false;
  Housekeeping wd({}, 0, plan, 1);
  CHECK(shape(run(wd, 0, seq)).rfind("000a: 0e02:* 0d:0000* 0b00:01 0d:0001* 22:0000 26:0000", 0) == 0);
  // Without a re-address: no bring-up reads.
  Housekeeping warm({}, 0, HousekeepingPlan{}, 1);
  CHECK(shape(run(warm, 0, seq)).rfind("0b00:01 22:0000 26:0000", 0) == 0);

  // The hourly tick: 0x0B00 01, then the 0x22 pair a second apart.
  for (uint32_t t = 1000; t < Housekeeping::SESSION_EVERY_MS; t += 1000) run(hk, t, seq);
  CHECK(shape(run(hk, Housekeeping::SESSION_EVERY_MS, seq)).rfind("0b00:01 ", 0) == 0);
  CHECK(shape(run(hk, Housekeeping::SESSION_EVERY_MS + 1000, seq)) == "22:0000 ");
  CHECK(shape(run(hk, Housekeeping::SESSION_EVERY_MS + 2000, seq)) == "22:0001 ");
}

// Two walks of the node table every 15 minutes, each page from where the
// reply left off, ending on an empty page.
static void test_walks() {
  HousekeepingPlan plan;
  plan.send_41 = false;
  Housekeeping hk({}, 0, plan, 1);
  uint8_t seq = 0;
  run(hk, 0, seq);
  CHECK(hk.sent_walk_pages() == 10);
  run(hk, Housekeeping::WALK_EVERY_MS - 1, seq);
  CHECK(hk.sent_walk_pages() == 10);
  run(hk, Housekeeping::WALK_EVERY_MS, seq);
  CHECK(hk.sent_walk_pages() == 20);

  CHECK(hk.walks_done() == 4);

  // A page waits WALK_PAGE_WAIT_MS for its reply, not the poll's 50 ms: a
  // 12-entry page is ~40 ms on the wire before the TAP even turns round.
  Housekeeping timed({}, 0, plan, 1);
  bool waited = false;
  for (Request r = timed.due(0, seq); r.cmd; r = timed.due(0, seq)) {
    if (sub_of(r) == 0x26) waited = r.wait_ms == Housekeeping::WALK_PAGE_WAIT_MS;
    if (sub_of(r) == 0x26) break;
    timed.on_reply(r, true, Frame{});
  }
  CHECK(waited && Housekeeping::WALK_PAGE_WAIT_MS >= 500);

  // A page with no reply is asked again, once, and that walk goes on; it
  // used to end the walk, so the pages after it went unread.
  Housekeeping lossy({}, 0, plan, 1);
  int pages = 0;
  std::vector<uint16_t> asked;
  for (Request r = lossy.due(0, seq); r.cmd; r = lossy.due(0, seq)) {
    if (sub_of(r) != 0x26) { lossy.on_reply(r, true, Frame{}); continue; }
    pages++;
    Bytes a = args_of(r);
    uint16_t idx = static_cast<uint16_t>(a[0] << 8 | a[1]);
    asked.push_back(idx);
    if (pages == 2) { lossy.on_reply(r, false, Frame{}); continue; }
    uint16_t start = idx < 2 ? 2 : idx;
    lossy.on_reply(r, true, page_reply(r, start, start > 41 ? 0 : static_cast<uint16_t>(std::min(12, 42 - start))));
  }
  CHECK(pages == 6 + 5 && asked.size() > 2 && asked[1] == asked[2]);
  CHECK(lossy.walks_done() == 2);
  // Two in a row with no reply end that walk; the second still runs.
  Housekeeping lost({}, 0, plan, 1);
  pages = 0;
  for (Request r = lost.due(0, seq); r.cmd; r = lost.due(0, seq)) {
    if (sub_of(r) != 0x26) { lost.on_reply(r, true, Frame{}); continue; }
    pages++;
    if (pages == 2 || pages == 3) { lost.on_reply(r, false, Frame{}); continue; }
    Bytes a = args_of(r);
    uint16_t idx = static_cast<uint16_t>(a[0] << 8 | a[1]);
    uint16_t start = idx < 2 ? 2 : idx;
    lost.on_reply(r, true, page_reply(r, start, start > 41 ? 0 : static_cast<uint16_t>(std::min(12, 42 - start))));
  }
  CHECK(pages == 3 + 5 && lost.walks_done() == 1);
  // A reply that doesn't move forward can't loop: the walk ends.
  Housekeeping stuck({}, 0, plan, 1);
  pages = 0;
  for (Request r = stuck.due(0, seq); r.cmd && pages < 100; r = stuck.due(0, seq)) {
    if (sub_of(r) != 0x26) { stuck.on_reply(r, true, Frame{}); continue; }
    pages++;
    stuck.on_reply(r, true, page_reply(r, 2, 0x0000 + 1));  // always "from 2, one entry"
  }
  CHECK(pages <= 2 * Housekeeping::WALK_MAX_PAGES);
}


// A 0x0149 payload with the given packets after a short header (status
// 0x41FF: no optional fields).
static Bytes poll_reply_with(const std::vector<Bytes> &packets) {
  Bytes p = hex("41ff004b9a");
  for (const Bytes &k : packets) p.insert(p.end(), k.begin(), k.end());
  return p;
}
static Bytes string_packet(uint16_t tuid, const std::string &text) {
  Bytes k = {0x07, static_cast<uint8_t>(tuid >> 8), static_cast<uint8_t>(tuid), 0x00, 0x02, 0xdb,
             static_cast<uint8_t>(text.size())};
  k.insert(k.end(), text.begin(), text.end());
  return k;
}
static Bytes power_from(uint16_t tuid) {  // a 0x31 packet, contents not read
  Bytes k = {0x31, static_cast<uint8_t>(tuid >> 8), static_cast<uint8_t>(tuid), 0x00, 0x00, 0x00, 0x04, 1, 2, 3, 4};
  return k;
}
static std::string query_text(const Request &r) { return std::string(r.payload.begin() + 7, r.payload.end()); }

// awake: a node that keeps reporting through it, once a minute.
static std::vector<Request> ask(Queries &q, uint32_t from_ms, uint32_t to_ms, uint8_t &seq, uint16_t awake = 0) {
  std::vector<Request> out;
  for (uint32_t t = from_ms; t <= to_ms; t += 100) {
    if (awake && t % 60000 == 0) q.on_poll_reply(poll_reply_with({power_from(awake)}), t);
    Request r = q.due(t, seq);
    if (r.cmd) out.push_back(r);
  }
  return out;
}

// The CCA's 0x06 dialogue with optimizers, from the 24 h capture.
static void test_queries() {
  uint8_t seq = 0;
  Queries q(0, /*writes=*/false);
  // A node first heard gets all five reads, as after a CCA reboot, a gap apart.
  q.on_poll_reply(poll_reply_with({power_from(0x14)}), 0);
  std::vector<Request> r = ask(q, 0, 10000, seq);
  CHECK(r.size() == 5);
  std::vector<std::string> texts;
  for (const Request &x : r) {
    CHECK(sub_of(x) == 0x06 && x.payload[5] == 0x00 && x.payload[6] == 0x14);
    CHECK(classify_request(x.cmd, x.payload) == Allow::yes);
    texts.push_back(query_text(x));
  }
  std::sort(texts.begin(), texts.end());
  CHECK((texts == std::vector<std::string>{"^00Info\r", "^00Mppt\r", "^00Smrt\r", "^00Tests\r", "^00Version\r"}));
  CHECK(ask(q, 10100, 3 * 60000 - 100, seq).empty());  // nothing more until the first retry

  // Unanswered: again after 3 min, then 5 more, then 7 more.
  CHECK(ask(q, 3 * 60000, 3 * 60000 + 10000, seq).size() == 5);
  // Version and Tests answer; the other three keep going.
  q.on_poll_reply(poll_reply_with({string_packet(0x14, "Mnode Version K8.0120 (2D)\r"),
                                   string_packet(0x14, "!Tests 1 3 2 0 00 0000 00\r")}),
                  3 * 60000 + 20000);
  r = ask(q, 8 * 60000, 8 * 60000 + 10000, seq);
  CHECK(r.size() == 3);
  for (const Request &x : r) CHECK(query_text(x) != "^00Version\r" && query_text(x) != "^00Tests\r");

  // An Info that asks for the w255: without writes, no write, but Info is
  // read again 40 minutes on, as the CCA does.
  uint32_t t0 = 9 * 60000;
  q.on_poll_reply(poll_reply_with({string_packet(0x14, "!Info 002E 12 0000 0DC1 00 0000 0000 00 00 0000 0FFF 2C8 B")}), t0);
  for (const Request &x : ask(q, t0, t0 + Queries::INFO_AGAIN_MS - 100, seq, 0x14)) CHECK(query_text(x) != "^00Info\r" && query_text(x) != QUERY_W255);
  bool again = false;
  for (const Request &x : ask(q, t0 + Queries::INFO_AGAIN_MS, t0 + Queries::INFO_AGAIN_MS + 5000, seq, 0x14))
    again = again || query_text(x) == "^00Info\r";
  CHECK(again && q.sent_writes() == 0);

  // With writes: Info "00" brings the w255, once; Smrt in mode N brings Smrt_ S.
  Queries w(0, /*writes=*/true);
  w.on_poll_reply(poll_reply_with({power_from(0x22)}), 0);
  ask(w, 0, 10000, seq);
  w.on_poll_reply(poll_reply_with({string_packet(0x22, "!Info 0027 12 0000 0DC1 00 0000 0000 00 00 0000 0FFF 1F9 B"),
                                   string_packet(0x22, "!Smrt 0000 0000 0000 0000 0000 0000 0000 0000 0000 N 00 0000 0000 0000 00")}),
                  20000);
  r = ask(w, 20000, 30000, seq);
  int w255 = 0, smart = 0;
  for (const Request &x : r) {
    if (query_text(x) == QUERY_W255) w255++;
    if (query_text(x) == QUERY_SMART_S) smart++;
    CHECK(may_send(x.cmd, x.payload, false, false, true));
  }
  CHECK(w255 == 1 && smart == 1 && w.sent_writes() == 2);
  // An Info already at FF, a Smrt already S: nothing to write.
  w.on_poll_reply(poll_reply_with({string_packet(0x22, "!Info 0027 12 0000 0D81 00 0000 0000 FF 00 0000 0FFF 161 B"),
                                   string_packet(0x22, "!Smrt 0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01")}),
                  40000);
  for (const Request &x : ask(w, 40000, 50000, seq)) CHECK(query_text(x) != QUERY_W255 && query_text(x) != QUERY_SMART_S);
  // Its next read still says N: an H9 answers N to every read and
  // reports S only in its reply to the write (its firmware, 3.5.0 and
  // 4.0.3), so the write is sent once a session, not after every read.
  w.on_poll_reply(poll_reply_with({string_packet(0x22, "!Smrt 0000 0000 0000 0000 0000 0000 0000 0000 0000 N 00 0000 0000 0000 00")}),
                  60000 + Queries::CHECK_AFTER_MS);
  for (const Request &x : ask(w, 60000 + Queries::CHECK_AFTER_MS, 60000 + 2 * Queries::CHECK_AFTER_MS + 10000, seq, 0x22))
    CHECK(query_text(x) != QUERY_SMART_S);
  CHECK(w.sent_writes() == 2);

  // A node quiet for half an hour is asleep: its queries stop. Heard again,
  // it gets ^00Tests, as each optimizer does at sunrise.
  Queries s(0, false);
  s.on_poll_reply(poll_reply_with({power_from(0x05)}), 0);
  ask(s, 0, 10000, seq);
  CHECK(ask(s, Queries::ASLEEP_MS + 60000, Queries::ASLEEP_MS + 70000, seq).empty());
  uint32_t dawn = 10 * 3600000;
  s.on_poll_reply(poll_reply_with({power_from(0x05)}), dawn);
  r = ask(s, dawn, dawn + 10000, seq);
  CHECK(r.size() == 1 && query_text(r[0]) == "^00Tests\r");
  // A reply with the long header (status bits 0-4 clear: seven more bytes)
  // is read past it, to the packets.
  Queries l(0, false);
  Bytes longer = hex("41e0");
  longer.insert(longer.end(), 10, 0x00);
  Bytes pk = power_from(0x09);
  longer.insert(longer.end(), pk.begin(), pk.end());
  l.on_poll_reply(longer, 0);
  r = ask(l, 0, 100, seq);
  CHECK(r.size() == 1 && r[0].payload[6] == 0x09);
  // Retries stop growing at 30 minutes.
  CHECK(Queries::retry_after_ms(1) == 3 * 60000 && Queries::retry_after_ms(2) == 5 * 60000 &&
        Queries::retry_after_ms(100) == 30 * 60000);
}

// A one-off read the owner asks for on the bench (^00GetOsState): a read's
// shape, so bench-only at the gate; sent until the node answers, the answer
// kept for the log; never on the roof.
static void test_queries_ask() {
  uint8_t seq = 0;
  Queries q(0, false);
  q.on_poll_reply(poll_reply_with({power_from(0x03)}), 0);
  ask(q, 0, 10000, seq);  // its five reads
  CHECK(!q.ask(0x03, "^00Get_State\r", 20000));  // a write's shape: refused
  CHECK(!q.ask(0x03, "#00x\r", 20000));
  CHECK(!q.ask(0x03, "^00Leave\r", 20000));
  CHECK(q.ask(0x03, "^00GetOsState\r", 20000));
  std::vector<Request> r = ask(q, 20000, 30000, seq, 0x03);
  CHECK(r.size() == 1 && query_text(r[0]) == "^00GetOsState\r");
  CHECK(classify_request(r[0].cmd, r[0].payload) == Allow::bench_only);
  CHECK(!may_send(r[0].cmd, r[0].payload, false));
  // Unanswered: asked again on the reads' schedule.
  r = ask(q, 20000 + 3 * 60000, 20000 + 3 * 60000 + 15000, seq, 0x03);  // after the five reads due then
  bool again = false;
  for (const Request &x : r) again = again || query_text(x) == "^00GetOsState\r";
  CHECK(again);
  // Answered: kept, and not asked again.
  q.on_poll_reply(poll_reply_with({string_packet(0x03, "!GetOsState State=OS_Run1, Tuid=3, Freq=15, Pan=0x1234, D=1\r")}), 300000);
  std::vector<std::pair<uint16_t, std::string>> got = q.take_asked();
  CHECK(got.size() == 1 && got[0].first == 0x03 && got[0].second.rfind("!GetOsState State=OS_Run1", 0) == 0);
  CHECK(q.take_asked().empty());
  for (const Request &x : ask(q, 300000, 300000 + 40 * 60000, seq, 0x03)) CHECK(query_text(x) != "^00GetOsState\r");
  // Never answered (a text this firmware doesn't know): given up after
  // Queries::ASK_TRIES, and said so.
  Queries n(0, false);
  n.on_poll_reply(poll_reply_with({power_from(0x04)}), 0);
  ask(n, 0, 10000, seq);
  CHECK(n.ask(0x04, "^00GetPAN\r", 20000));
  int sent = 0;
  for (uint32_t t = 20000; t < 20000 + 3 * 3600000u; t += 60000) {
    n.on_poll_reply(poll_reply_with({power_from(0x04)}), t);  // awake throughout
    for (const Request &x : ask(n, t, t + 59000, seq, 0x04)) sent += query_text(x) == "^00GetPAN\r";
  }
  CHECK(sent == static_cast<int>(Queries::ASK_TRIES));
  std::vector<std::pair<uint16_t, std::string>> gave = n.take_asked();
  CHECK(gave.size() == 1 && gave[0].second == "no answer to ^00GetPAN after 3 tries");
}

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
  WindowKeeper window;
  PollCursor cursor(0);
  uint8_t seq = 0;
  // The discovery window comes first, then housekeeping, then polls.
  CHECK(next_request(window, cursor, seq, 0, &hk).payload == tunnel_request(SUB_DISCOVERY_STATUS, 1, {}));
  CHECK(next_request(window, cursor, seq, 0, &hk).cmd == CMD_SESSION);
  CHECK(next_request(window, cursor, seq, 0, &hk).cmd == CMD_POLL);
  Request r = next_request(window, cursor, seq, 6000, &hk);
  CHECK(r.cmd == CMD_TUNNEL && r.payload == tunnel_request(SUB_HOUSEKEEPING, 2, args41(0x0001, 0x01)));
  // Without it, as before.
  CHECK(next_request(window, cursor, seq, 7000).cmd == CMD_POLL);
}

// After a re-address, the CCA's bring-up reads go before the join window, so
// its 0D 00 00 is the first tunnel request after the reset (seq 01), as the
// CCA's is; the window's status and renewal follow.
static void test_next_request_bring_up_before_the_window() {
  HousekeepingPlan plan;
  plan.bring_up_reads = true;
  plan.session = plan.broadcast = true;
  plan.session_reset = true;  // PV off around the read, as the roof's housekeeping does
  Housekeeping hk({}, 0, plan, 1);
  WindowKeeper window;
  PollCursor cursor(0);
  uint8_t seq = 0;
  std::vector<std::string> got;
  for (int i = 0; i < 9; i++) {
    Request r = next_request(window, cursor, seq, 0, &hk);
    char b[24];
    if (r.cmd == CMD_TUNNEL) snprintf(b, sizeof(b), "T%02X/%02X", r.payload[3], r.payload[4]);
    else snprintf(b, sizeof(b), "%04X", r.cmd);
    got.push_back(b);
  }
  // VERSION, 0E02, PV, the 0D read at seq 01, PV, the stored read at 02, the
  // 0x22; then the window's status.
  CHECK(got[0] == "000A" && got[1] == "0E02" && got[3] == "T0D/01" && got[5] == "T0D/02");
  bool window_after = false;
  for (size_t i = 6; i < got.size(); i++) window_after = window_after || got[i].rfind("T2E/", 0) == 0;
  CHECK(window_after);
  CHECK(std::find(got.begin(), got.begin() + 5, std::string("T2E/01")) == got.begin() + 5);
}

// The PV switch: 0x0B00 00 off, 01 on (the CCA's PV-off updater; the roof's
// 0x0B00 [01] -> 0x0B01 [01]). Off goes out at once, is retried until the
// TAP's reply carries it, and no 01 goes out while it is held.
static Frame session_reply(uint8_t v) {
  Frame f;
  f.cmd = CMD_SESSION_REPLY;
  f.payload = {v};
  return f;
}

static void test_pv_switch() {
  // The guard: 00 only while off is held, or in a bring-up.
  CHECK(classify_request(CMD_SESSION, {PV_OFF}) == Allow::pv_off);
  CHECK(!may_send(CMD_SESSION, {PV_OFF}, true, false, true, false));  // bench and writes don't open it
  CHECK(may_send(CMD_SESSION, {PV_OFF}, false, false, false, /*pv_off=*/true));
  CHECK(may_send(CMD_SESSION, {PV_OFF}, false, /*startup=*/true));
  CHECK(classify_request(CMD_SESSION, {PV_ON}) == Allow::yes);
  CHECK(classify_request(CMD_SESSION, {0x02}) == Allow::bench_only);

  // Immediate, one at a time, retried, then flagged.
  PvSwitch pv(false, /*hourly=*/true, 0);
  CHECK(pv.due(0).cmd == 0 && !pv.off() && pv.value() == PV_ON);
  pv.set_off(true, 1000);
  Request r = pv.due(1000);
  CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_OFF});
  CHECK(pv.due(1001).cmd == 0);
  CHECK(pv.last_sent() == PV_OFF && pv.last_reply() == -1 && !pv.confirmed());
  pv.on_reply(r, false, {}, 1001);
  CHECK(pv.due(1001 + PvSwitch::RETRY_MS - 1).cmd == 0);
  uint32_t t = 1001 + PvSwitch::RETRY_MS;
  for (uint32_t i = 0; i < PvSwitch::RETRIES; i++) {
    r = pv.due(t);
    CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_OFF});
    CHECK(!pv.unconfirmed());
    pv.on_reply(r, true, session_reply(PV_ON), t);  // the wrong byte
    t += PvSwitch::RETRY_MS;
  }
  CHECK(pv.unconfirmed() && !pv.confirmed() && pv.last_reply() == PV_ON);
  CHECK(pv.due(t).cmd == 0);  // no more retries until the hour
  // The hourly re-assert, with no housekeeping to send it: 00, never 01.
  r = pv.due(1000 + PvSwitch::HOURLY_MS);
  CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_OFF});
  pv.on_reply(r, true, session_reply(PV_OFF), 1000 + PvSwitch::HOURLY_MS);
  CHECK(pv.confirmed() && !pv.unconfirmed() && pv.last_reply() == PV_OFF);
  CHECK(pv.due(1000 + PvSwitch::HOURLY_MS + PvSwitch::RETRY_MS).cmd == 0);

  // The latest wins: off then on before any reply leaves one pending 01.
  PvSwitch l(false, true, 0);
  l.set_off(true, 0);
  l.set_off(false, 10);
  r = l.due(10);
  CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_ON} && l.due(11).cmd == 0);
  l.on_reply(r, true, session_reply(PV_ON), 11);
  CHECK(l.confirmed());
  // On, and housekeeping sends the hourly: nothing from the switch itself.
  PvSwitch quiet(false, /*hourly=*/false, 0);
  CHECK(quiet.due(PvSwitch::HOURLY_MS).cmd == 0);
  // Off held at start: 00 at once.
  PvSwitch held(true, false, 0);
  r = held.due(0);
  CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_OFF});

  // Housekeeping takes its byte from the switch.
  HousekeepingPlan plan;
  plan.bring_up_reads = true;
  plan.session_reset = true;
  plan.send_41 = false;
  plan.walks = false;
  uint8_t seq = 0;
  PvSwitch off(true, false, 0);
  Housekeeping hk({}, 0, plan, 1);
  hk.set_pv(&off);
  std::string got = shape(run(hk, 0, seq));
  CHECK(got.rfind("000a: 0e02:* 0b00:00* 0d:0000* 0d:0001* 22:0000", 0) == 0);  // 00 once, no 01
  if (got.find("0b00:01") != std::string::npos) std::printf("    got  %s\n", got.c_str());
  for (uint32_t t2 = 1000; t2 < Housekeeping::SESSION_EVERY_MS; t2 += 1000) run(hk, t2, seq);
  CHECK(shape(run(hk, Housekeeping::SESSION_EVERY_MS, seq)).rfind("0b00:00 ", 0) == 0);
  CHECK(shape(run(hk, Housekeeping::SESSION_EVERY_MS + 1000, seq)) == "22:0000 ");
  Housekeeping warm({}, 0, HousekeepingPlan{}, 1);
  warm.set_pv(&off);
  CHECK(shape(run(warm, 0, seq)).rfind("0b00:00 22:0000", 0) == 0);
  // On: today's sequence.
  PvSwitch on(false, false, 0);
  Housekeeping same({}, 0, plan, 1);
  same.set_pv(&on);
  CHECK(shape(run(same, 0, seq)).rfind("000a: 0e02:* 0b00:00* 0d:0000* 0b00:01 0d:0001*", 0) == 0);

  // Silent: a master that took over without hearing a PV state says nothing
  // about it (the TAP keeps what it was last told) until the owner asks.
  PvSwitch mute(false, /*hourly=*/true, 0, /*silent=*/true);
  CHECK(mute.silent() && mute.due(0).cmd == 0 && mute.due(PvSwitch::HOURLY_MS).cmd == 0);
  Housekeeping hush({}, 0, HousekeepingPlan{}, 1);
  hush.set_pv(&mute);
  got = shape(run(hush, 0, seq));
  CHECK(got.find("0b00") == std::string::npos && got.rfind("22:0000", 0) == 0);
  for (uint32_t t2 = 1000; t2 < Housekeeping::SESSION_EVERY_MS; t2 += 1000) run(hush, t2, seq);
  CHECK(shape(run(hush, Housekeeping::SESSION_EVERY_MS, seq)).find("0b00") == std::string::npos);
  // A bring-up after a re-address still sends the CCA's 00 then 01.
  Housekeeping wake({}, 0, plan, 1);
  wake.set_pv(&mute);
  CHECK(shape(run(wake, 0, seq)).rfind("000a: 0e02:* 0b00:00* 0d:0000* 0b00:01 0d:0001*", 0) == 0);
  mute.set_off(true, 10);
  CHECK(!mute.silent() && mute.due(10).payload == Bytes{PV_OFF});

  // A pending switch goes ahead of the discovery window.
  WindowKeeper window;
  PollCursor cursor(0);
  PvSwitch now(false, true, 0);
  now.set_off(true, 0);
  r = next_request(window, cursor, seq, 0, nullptr, nullptr, &now);
  CHECK(r.cmd == CMD_SESSION && r.payload == Bytes{PV_OFF});
  r = next_request(window, cursor, seq, 0, nullptr, nullptr, &now);
  CHECK(r.payload == tunnel_request(SUB_DISCOVERY_STATUS, seq, {}));
}

// ---- the herd (Herd): every TAP on the bus at once

// TAPs on a simulated bus, as the 11-06 bench's answered the CCA: a
// SET_485_ID broadcast moves every one to the factory id, GET_MAC there is
// answered by each, SET_ID_BY_MAC moves the one with that MAC, STORE keeps
// its id, and a RESET puts each back at its stored id, or at the factory id
// if it has none (a new TAP).
struct SimTap {
  Bytes mac;
  uint16_t id;
  uint16_t stored;  // 0: none
};
struct SimBus {
  std::vector<SimTap> taps;
  std::vector<std::pair<uint16_t, uint16_t>> sent;  // dest, cmd
  std::vector<Frame> handle(const DiscoveryRequest &r) {
    sent.push_back({r.dest, r.cmd});
    std::vector<Frame> out;
    auto reply = [&](uint16_t at, uint16_t cmd, Bytes p) {
      Frame f;
      f.dest = at | 0x8000;
      f.cmd = cmd;
      f.payload = std::move(p);
      out.push_back(f);
    };
    auto mac_id = [](const SimTap &t) {
      Bytes p = t.mac;
      p.push_back(static_cast<uint8_t>(t.id >> 8));
      p.push_back(static_cast<uint8_t>(t.id));
      return p;
    };
    if (r.cmd == CMD_SET_485_ID) {
      for (auto &t : taps) t.id = 0x1235;
      reply(0x0000, 0x0015, {});
    } else if (r.cmd == CMD_RESET) {
      for (auto &t : taps) t.id = t.stored ? t.stored : 0x1235;
    } else {
      for (auto &t : taps) {
        if (t.id != r.dest) continue;
        if (r.cmd == CMD_GET_MAC || r.cmd == CMD_GET_MAC_NOW) reply(t.id, r.cmd + 1, mac_id(t));
        if (r.cmd == CMD_STORE) {
          t.stored = t.id;
          reply(t.id, CMD_STORE + 1, {});
        }
        if (r.cmd == CMD_SET_ID_BY_MAC && Bytes(r.payload.begin() + 4, r.payload.begin() + 12) == t.mac) {
          reply(t.id, CMD_SET_ID_BY_MAC + 1, {});
          t.id = static_cast<uint16_t>(r.payload[12] << 8 | r.payload[13]);
          break;
        }
      }
    }
    return out;
  }
  int count(uint16_t cmd) const {
    int n = 0;
    for (auto &s : sent) n += s.second == cmd;
    return n;
  }
};
static Bytes tap_mac(uint8_t last) { return {0x04, 0xC0, 0x5B, 0x00, 0x00, 0x00, 0x00, last}; }

// Runs a herd to its end against the bus; every request's frames checked
// against what the send gate allows a bring-up.
static void run_herd(Herd &h, SimBus &bus) {
  for (int i = 0; i < 2000; i++) {
    DiscoveryRequest r = h.next();
    if (r.done || r.failed) return;
    if (!r.send) continue;
    CHECK(may_send(r.cmd, r.payload, false, /*startup=*/true));
    std::vector<Frame> replies = bus.handle(r);
    for (size_t k = 0; k < replies.size() && (r.collect || k == 0); k++) h.on_reply(replies[k]);
  }
  CHECK(false);  // never ended
}

// One TAP the map knows, at its stored id: one round, five broadcasts, its
// old id back, the scan, the read, the hop one up, the RESET, and home.
static void test_herd_one_known_tap() {
  SimBus bus;
  bus.taps = {{tap_mac(1), 0x1201, 0x1201}};
  Herd h(0x1235, 0x1201, {{tap_mac(1), 0x1201}});
  run_herd(h, bus);
  CHECK(h.done() && h.round() == 1);
  CHECK(h.placed().size() == 1 && h.placed()[0].id == 0x1201 && !h.placed()[0].fresh && h.placed()[0].answered);
  CHECK(bus.count(CMD_SET_485_ID) == 5 && bus.count(CMD_RESET) == 1 && bus.count(CMD_STORE) == 0);
  CHECK(bus.count(CMD_GET_MAC) == 1 + Herd::SCAN_TRIES);
  // The hop went to 1202, one past the only placed id.
  bool hopped = false;
  for (auto &s : bus.sent) hopped = hopped || (s.first == 0x1202 && s.second == CMD_GET_MAC_NOW);
  CHECK(hopped);
  CHECK(bus.taps[0].id == 0x1201);
}

// The 11-06 bench: one TAP known at 1201, one new. The new one gets 1202,
// isn't back there after the RESET (it keeps no id), and the herd runs
// again with a STORE for it, after which both are home.
static void test_herd_a_new_tap_is_stored_in_a_second_round() {
  SimBus bus;
  bus.taps = {{tap_mac(2), 0x0000, 0}, {tap_mac(1), 0x1201, 0x1201}};
  Herd h(0x1235, 0x1201, {{tap_mac(1), 0x1201}});
  run_herd(h, bus);
  CHECK(h.done() && h.round() == 2);
  CHECK(bus.count(CMD_SET_485_ID) == 10 && bus.count(CMD_RESET) == 2 && bus.count(CMD_STORE) == 1);
  const Herd::Placed *n = nullptr, *k = nullptr;
  for (const auto &p : h.placed()) (p.mac == tap_mac(2) ? n : k) = &p;
  CHECK(n && k && n->id == 0x1202 && n->fresh && n->stored && n->answered);
  CHECK(k->id == 0x1201 && !k->fresh && !k->stored && k->answered);
  CHECK(bus.taps[0].id == 0x1202 && bus.taps[0].stored == 0x1202 && bus.taps[1].id == 0x1201);
  // The hops went past both placed ids: 1203 and 1204.
  int hops = 0;
  for (auto &s : bus.sent) hops += (s.first == 0x1203 || s.first == 0x1204) && s.second == CMD_GET_MAC_NOW;
  CHECK(hops >= 2);
}

// A known TAP missing from the bus keeps its id: a new one skips it.
static void test_herd_skips_a_missing_taps_id() {
  SimBus bus;
  bus.taps = {{tap_mac(1), 0x1201, 0x1201}, {tap_mac(3), 0x0000, 0x0000}};
  Herd h(0x1235, 0x1201, {{tap_mac(1), 0x1201}, {tap_mac(2), 0x1202}});
  run_herd(h, bus);
  CHECK(h.done());
  for (const auto &p : h.placed())
    if (p.mac == tap_mac(3)) CHECK(p.id == 0x1203);
}

// The ids skip 1234 and 1235, which a TAP takes as broadcast addresses.
static void test_herd_skips_the_broadcast_ids() {
  SimBus bus;
  bus.taps = {{tap_mac(1), 0x1233, 0x1233}, {tap_mac(2), 0x0000, 0x0000}};
  Herd h(0x1235, 0x1233, {{tap_mac(1), 0x1233}});
  run_herd(h, bus);
  CHECK(h.done());
  for (const auto &p : h.placed())
    if (p.mac == tap_mac(2)) CHECK(p.id == 0x1236);
  for (auto &s : bus.sent) CHECK(!(s.second == CMD_GET_MAC_NOW && s.first == 0x1234));
}

// Not adopting: a TAP the map doesn't know is heard, left at the factory
// address and reported; the known one is moved on as before.
static void test_herd_not_adopting_leaves_a_new_tap() {
  SimBus bus;
  bus.taps = {{tap_mac(2), 0x0000, 0}, {tap_mac(1), 0x1201, 0x1201}};
  Herd h(0x1235, 0x1201, {{tap_mac(1), 0x1201}}, /*adopt_new=*/false);
  run_herd(h, bus);
  CHECK(h.done() && h.round() == 1);
  CHECK(h.placed().size() == 1 && h.placed()[0].mac == tap_mac(1) && h.placed()[0].answered);
  CHECK(h.extras().size() == 1 && h.extras()[0] == tap_mac(2));
  CHECK(bus.taps[0].id == 0x1235 && bus.taps[1].id == 0x1201);
  CHECK(bus.count(CMD_STORE) == 0);
}

// No TAP on the bus: failed, after MAC_TRIES GET_MACs.
static void test_herd_with_no_tap_fails() {
  SimBus bus;
  Herd h(0x1235, 0x1201, {});
  run_herd(h, bus);
  CHECK(h.failed() && bus.count(CMD_GET_MAC) == Herd::MAC_TRIES);
}

int main() {
  test_herd_one_known_tap();
  test_herd_a_new_tap_is_stored_in_a_second_round();
  test_herd_skips_a_missing_taps_id();
  test_herd_skips_the_broadcast_ids();
  test_herd_with_no_tap_fails();
  test_herd_not_adopting_leaves_a_new_tap();
  test_discovery();
  test_discovery_scan();
  test_discovery_hop();
  test_discovery_lost_replies();
  test_startup_allow();
  test_review_fixes();
  test_codec();
  test_discovery_window();
  test_poll_cursor();
  test_window_keeper();
  test_window_keeper_not_renewing();
  test_allow_list();
  test_more_allow();
  test_next_request();
  test_housekeeping_allow();
  test_learner();
  test_housekeeping();
  test_tap_reconfiguring_frames_are_never_sent();
  test_discovery_window_counts();
  test_no_renewal_for_an_empty_table();
  test_next_request_housekeeping();
  test_next_request_bring_up_before_the_window();
  test_bring_up_plan();
  test_walks();
  test_queries();
  test_queries_ask();
  test_pv_switch();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
