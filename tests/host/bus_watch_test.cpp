// Host tests for bus_watch.h: naming what is said on the bus, pairing each
// request with its reply, and spotting
// another device reconfiguring the TAP.
#include "bus_watch.h"

#include <cstdio>
#include <cstring>
#include <string>

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
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    unsigned v;
    std::sscanf(s + i, "%2x", &v);
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

static Frame request(uint16_t cmd, const Bytes &payload, uint16_t dest = 0x1201) { return {dest, cmd, payload}; }
static Frame reply(uint16_t cmd, const Bytes &payload, uint16_t from = 0x1201) {
  return {static_cast<uint16_t>(from | 0x8000), cmd, payload};
}
static Bytes text_query(uint16_t node, const char *t) {
  Bytes a = {static_cast<uint8_t>(node >> 8), static_cast<uint8_t>(node)};
  a.insert(a.end(), t, t + std::strlen(t));
  return a;
}

static void test_names() {
  CHECK(std::string(cmd_name(0x0148)) == "Poll");
  CHECK(std::string(cmd_name(0x0149)) == "Poll reply");
  CHECK(std::string(cmd_name(0x000C)) == "SET_CHANNEL");
  CHECK(std::string(cmd_name(0x0006)) == "Refused");
  CHECK(std::string(cmd_name(0x0146)) == "Query not taken");
  CHECK(std::string(cmd_name(0x0B0F)) == "Tunnel");
  CHECK(std::string(cmd_name(0x0B10)) == "Tunnel reply");
  CHECK(std::string(cmd_name(0x0777)) == "");
  CHECK(std::string(sub_name(0x41)) == "SetPgw");
  CHECK(std::string(sub_name(0x2F)) == "DiscState reply");
  CHECK(std::string(sub_name(0x2B)) == "EraseFlash");
  CHECK(std::string(sub_name(0x0E)) == "GatewayCfg reply");
  CHECK(std::string(sub_name(0x99)) == "");
}

// Our master's own frames, the CCA's steady state and the bring-up are never
// an alarm: only what classify_request says no master here would ever send.
static void test_no_alarm_for_what_a_master_here_sends() {
  CHECK(reconfiguration(request(0x0148, hex("00016fc404"))).empty());
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x2D, 1, discovery_renewal_args(44)))).empty());
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x0D, 1, {0x00, 0x00}))).empty());
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x06, 1, text_query(2, "^00Info\r")))).empty());
  // The bring-up: SET_485_ID (key + 1235), RESET (key), the key-only STORE.
  CHECK(reconfiguration(request(0x0014, hex("372492661235"), 0xFFFF)).empty());
  CHECK(reconfiguration(request(0x0010, hex("37249266"), 0xFFFF)).empty());
  CHECK(reconfiguration(request(0x0012, hex("37249266"))).empty());
  // A TAP's reply is never a reconfiguration, whatever it holds.
  CHECK(reconfiguration(reply(0x0B10, hex("000e000e0101"))).empty());
}

static void test_alarm_names_the_reconfiguration() {
  Bytes cfg = {0x01, 0x00};
  cfg.resize(38, 0x00);
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x0D, 7, cfg))) ==
        "GatewayCfg write (the TAP's channel, network ID and key)");
  CHECK(reconfiguration(request(0x000C, {0x00, 0x00})) == "SET_CHANNEL");
  CHECK(reconfiguration(request(0x0070, hex("010807fff800"))) == "Channel scan");
  CHECK(reconfiguration(request(0x0522, hex("0001"))) == "TAP firmware (0x0522)");
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x2B, 1, {}))) == "EraseFlash (the optimizer table)");
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x29, 1, hex("0002")))) == "SetTable");
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x1D, 1, {}))) == "ResetNode");
  CHECK(reconfiguration(request(0x011E, {})) == "CLEAR_ADT");
  CHECK(reconfiguration(request(0x0012, hex("3724926600"))) == "STORE");
  CHECK(reconfiguration(request(CMD_TUNNEL, tunnel_request(0x06, 1, text_query(2, "^00Discharge_01\r")))) ==
        "An optimizer setting write (^00Discharge_01)");
  CHECK(reconfiguration(request(0x0200, hex("37249266"))) == "A frame carrying the setup key (0x0200)");
  // A broadcast (to 0xFFFF, the 0x8000 bit set) is a request, not a reply.
  CHECK(reconfiguration(request(0x000C, {0x00, 0x00}, 0xFFFF)) == "SET_CHANNEL");
}

// Every payload in hex, but for the GatewayCfg block, which holds the
// network key: hidden in the write and in the TAP's reply, read or not.
static void test_payload_hex_hides_only_the_config_block() {
  CHECK(bus_hex(request(0x003C, hex("3724926604c05b0000ab12cd1202"))) == "37 24 92 66 04 C0 5B 00 00 AB 12 CD 12 02");
  CHECK(bus_hex(request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x01}))) == "00 00 00 0D 03 00 01");
  Bytes cfg = {0x01, 0x00};
  for (int i = 0; i < 36; i++) cfg.push_back(static_cast<uint8_t>(0xA0 + i));
  std::string w = bus_hex(request(CMD_TUNNEL, tunnel_request(0x0D, 4, cfg)));
  CHECK(w == "00 00 00 0D 04 01 00 + 36 bytes hidden (the TAP's network config and key)");
  Bytes rp = {0x00, 0x0E, 0x00, 0x0E, 0x03};
  for (int i = 0; i < 37; i++) rp.push_back(static_cast<uint8_t>(0xB0 + i));
  std::string r = bus_hex(reply(0x0B10, rp));
  CHECK(r == "00 0E 00 0E 03 + 37 bytes hidden (the TAP's network config and key)");
  CHECK(w.find("A5") == std::string::npos && r.find("B5") == std::string::npos);
  CHECK(bus_hex(request(CMD_TUNNEL, tunnel_request(0xE1, 5, hex("12345678aabbccdd")))) ==
        "00 00 00 E1 05 + 8 bytes hidden (a list of networks, perhaps with keys)");
  CHECK(bus_hex(request(0x000A, {})).empty());
}

static const Exchange &last(const BusWatch &w) { return w.exchanges().back(); }

// A 0x0149 payload with one type-07 packet: an optimizer's text answer.
static Bytes poll_reply_with_answer(uint16_t node, const char *text) {
  Bytes p = {0x00, 0x1F, 0x00, 0x00, 0x00};
  size_t n = std::strlen(text);
  Bytes pkt = {0x07, static_cast<uint8_t>(node >> 8), static_cast<uint8_t>(node), 0x00, 0x00, 0x00, static_cast<uint8_t>(n)};
  pkt.insert(pkt.end(), text, text + n);
  p.insert(p.end(), pkt.begin(), pkt.end());
  return p;
}

// A request and its reply make one exchange, timed; polls are only counted.
static void test_a_request_and_its_reply_are_one_exchange() {
  BusWatch w;
  w.on_frame(request(0x0148, hex("00016fc404")), false, 1000);
  w.on_frame(reply(0x0149, hex("001f000000")), false, 1011);
  CHECK(w.exchanges().empty() && w.polls() == 1);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 5, {})), true, 2000);
  w.on_frame(reply(0x0B10, hex("000e002f05010198002c0028002c00")), false, 2012);
  CHECK(w.exchanges().size() == 1);
  const Exchange &e = last(w);
  CHECK(e.unix_ms == 2000 && e.ours && !e.other_master && e.addr == 0x1201);
  CHECK(e.name == "Tunnel" && e.sub_name == "DiscState" && e.category == "discovery");
  CHECK(e.result == "answered" && e.rt_ms == 12 && !e.late);
  CHECK(e.summary == "Open, 408 s left · 40 of the 44 in the table have joined");
  CHECK(e.request_hex == "00 00 00 2E 05" && e.reply_hex == "00 0E 00 2F 05 01 01 98 00 2C 00 28 00 2C 00");
}

static void test_results() {
  BusWatch w;
  // No reply: the next request closes it.
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 1, {})), true, 100);
  w.on_frame(request(0x0148, hex("00016fc404")), true, 200);
  CHECK(last(w).result == "no_reply" && last(w).rt_ms == -1 && last(w).summary == "Nothing back");
  w.on_frame(reply(0x0149, hex("001f000000")), false, 210);
  // A tunnel reply with another seq is not this request's.
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 2, {})), true, 300);
  w.on_frame(reply(0x0B10, hex("000e002f09010198002c0028002c00")), false, 310);
  CHECK(w.exchanges()[w.exchanges().size() - 2].result == "waiting");
  CHECK(last(w).result == "unasked" && last(w).summary == "A reply to no request heard");
  // A refused poll is counted; a refused request and an undelivered query are exchanges.
  w.on_frame(request(0x0148, hex("00016fc404")), true, 400);
  CHECK(w.exchanges()[w.exchanges().size() - 2].result == "no_reply");
  w.on_frame(reply(0x0006, {}), false, 405);
  CHECK(w.stats(500).polls_refused == 1);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x06, 3, text_query(7, "^00Version\r"))), true, 600);
  w.on_frame(reply(0x0146, {0x00, 0x02}), false, 609);
  CHECK(last(w).result == "not_delivered" && last(w).rt_ms == 9 && last(w).node == 7 && last(w).category == "queries");
  CHECK(last(w).summary == "^00Version · the TAP couldn't deliver it (0x0146): the optimizer may be off or rejoining");
  w.on_frame(request(0x000A, {}), true, 700);
  w.on_frame(reply(0x0006, {}), false, 703);
  CHECK(last(w).result == "refused" && last(w).summary == "The TAP refused it (0x0006)");
  // A broadcast expects no reply.
  w.on_frame(request(0x0014, hex("372492661235"), 0xFFFF), true, 800);
  CHECK(last(w).result == "broadcast" && last(w).addr == 0xFFFF && last(w).summary == "Every TAP to 1235");
  // Still open, but long past: no reply.
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 4, {})), true, 900);
  CHECK(w.exchanges_at(950).back().result == "waiting");
  CHECK(w.exchanges_at(2000).back().result == "no_reply");
}

// A reply that comes after the next request still finds its own.
static void test_a_late_reply_finds_its_request() {
  BusWatch w;
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x26, 8, {0x00, 0x02})), true, 1000);
  w.on_frame(request(0x0148, hex("00016fc404")), true, 1050);
  w.on_frame(reply(0x0B10, hex("0010002708000200010004c05b0000ab12cd0002")), false, 1080);
  CHECK(last(w).result == "answered" && last(w).rt_ms == 80 && last(w).late);
  CHECK(last(w).summary == "1 entry from index 0002");
  w.on_frame(reply(0x0149, hex("001f000000")), false, 1090);  // and the poll still gets its own
  CHECK(w.stats(1100).polls_lost == 0 && w.stats(1100).polls == 1);
}

static void test_summaries() {
  BusWatch w;
  auto ask = [&](uint16_t cmd, const Bytes &p, uint16_t rcmd, const Bytes &r, int64_t t) {
    w.on_frame(request(cmd, p), true, t);
    if (rcmd) w.on_frame(reply(rcmd, r), false, t + 10);
    return last(w).summary;
  };
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x2D, 1, discovery_renewal_args(44)), 0x0B10, hex("0005002e01"), 0) ==
        "Renew for 900 s, 44 in the table");
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x2E, 2, {}), 0x0B10, hex("000e002f02000000000000280028"), 100) ==
        "Closed · 40 of the 40 in the table have joined");
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x26, 3, {0x00, 0x0E}), 0x0B10, hex("0009002703000e0000"), 200) ==
        "The end of the table, from index 000E");
  CHECK(ask(0x0B00, {0x01}, 0x0B01, {0x01}, 300) == "PV on · the TAP confirmed");
  CHECK(ask(0x0B00, {0x00}, 0x0B01, {0x01}, 400) == "PV off · the TAP answered on");
  CHECK(ask(0x000A, {}, 0x000B, Bytes{'M', 'g', 'a', 't', 'e', ' ', 'V', 'e', 'r', 's', 'i', 'o', 'n', ' ', 'G', '8', '\r'},
            500) == "Mgate G8");
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x22, 4, {0x00, 0x00}), 0x0B10, hex("0005002304"), 600) == "To every optimizer");
  // SetPgw: the target node, then 00 00 00 <stream> 00 and content copied from a CCA.
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x41, 7, hex("00270000000400aa00000000040500000000000000000000")), 0x0B10,
            hex("000d004107"), 650) == "Preferred gateway for node 0027 · stream 04");
  CHECK(last(w).node == 0x27 && last(w).category == "housekeeping");
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x0D, 5, {0x00, 0x01}), 0x0B10, hex("0006000e0501"), 700) ==
        "Read the TAP's network config (part 1)");
  CHECK(ask(0x003C, hex("3724926604c05b0000ab12cd1202"), 0x003D, {}, 800) == "TAP 04C05B0000AB12CD to 1202");
  CHECK(ask(0x003A, {}, 0x003B, hex("04c05b0000ab12cd1201"), 900) == "04C05B0000AB12CD at 1201");
  CHECK(ask(0x0010, hex("37249266"), 0, {}, 1000) == "Reset");
  CHECK(ask(0x0012, hex("37249266"), 0x0013, {}, 1100) == "Store the TAP's RS485 id");
  CHECK(last(w).category == "bringup");
  // The RESET got nothing back, but keeps saying what it was.
  CHECK(w.exchanges()[w.exchanges().size() - 2].result == "no_reply" &&
        w.exchanges()[w.exchanges().size() - 2].summary == "Reset");
  Bytes cfg = {0x01, 0x00};
  cfg.resize(38, 0x00);
  CHECK(ask(CMD_TUNNEL, tunnel_request(0x0D, 6, cfg), 0x0B10, hex("0006000e0601"), 1200) ==
        "GatewayCfg write (the TAP's channel, network ID and key)");
  CHECK(last(w).category == "reconfig" && last(w).reconfig == last(w).summary && last(w).sub_name == "GatewayCfg write");
}

// An optimizer's answer comes back later, in a poll reply: it goes on the
// query that asked.
static void test_an_answer_joins_its_query() {
  BusWatch w;
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x06, 7, text_query(2, "^00Info\r"))), true, 1000);
  w.on_frame(reply(0x0B10, hex("000d000707")), false, 1013);
  CHECK(last(w).result == "delivered" && last(w).summary == "^00Info");
  w.on_frame(request(0x0148, hex("00016fc404")), true, 2000);
  w.on_frame(reply(0x0149, poll_reply_with_answer(3, "!Info other node\r")), false, 2010);
  CHECK(last(w).answer.empty());
  w.on_frame(request(0x0148, hex("00016fc504")), true, 2900);
  w.on_frame(reply(0x0149, poll_reply_with_answer(2, "!Info 0027 12 FF\r")), false, 2913);
  CHECK(last(w).answer == "!Info 0027 12 FF" && last(w).answer_after_ms == 1900);
}

// Listening, every request is the master's. Running the bus, one that isn't
// ours is another master's.
static void test_another_master() {
  BusWatch w;
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 1, {})), false, 1000);
  CHECK(!last(w).other_master);
  w.on_frame(request(0x0148, hex("00016fc404")), true, 2000);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 9, {})), false, 3000);
  CHECK(last(w).other_master);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 9, {})), false, 20000);  // ours quiet 18 s: we've stopped
  CHECK(!last(w).other_master);
}

static void test_stats_cover_five_minutes() {
  BusWatch w;
  int64_t t = 0;
  // 100 polls 20 ms apart: every tenth unanswered, every fifth answered with a packet.
  for (int i = 0; i < 100; i++, t += 20) {
    w.on_frame(request(0x0148, hex("00016fc404")), true, t);
    if (i % 10) w.on_frame(reply(0x0149, i % 5 ? hex("001f000000") : poll_reply_with_answer(2, "x")), false, t + 10);
  }
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 1, {})), true, t);
  w.on_frame(reply(0x0B10, hex("000e002f01010198002c0028002c00")), false, t + 30);
  w.on_frame(request(0x000C, {0x20, 0x00}), false, t + 100);
  BusStats s = w.stats(t + 200);
  CHECK(s.polls == 100 && s.polls_lost == 10 && s.polls_empty == 80);
  CHECK(s.count.at("discovery") == 1 && s.count.at("reconfig") == 1 && s.reconfig == 1 && s.other_master == 1);
  CHECK(s.rt_median_ms == 10 && s.rt_max_ms == 30);
  CHECK(s.discovery == "Open, 408 s left · 40 of the 44 in the table have joined");
  // Five minutes on, all of it has aged out but the discovery state.
  BusStats later = w.stats(t + 5 * 60000 + 1000);
  CHECK(later.polls == 0 && later.count.at("discovery") == 0 && later.rt_median_ms == -1);
  CHECK(later.discovery == s.discovery);
}

static void test_log_is_bounded() {
  BusWatch w;
  for (int i = 0; i < 1000; i++) w.on_frame(request(0x000A, {}), false, i);
  CHECK(w.exchanges().size() == BusWatch::KEEP && w.exchanges().front().unix_ms == 1000 - (int64_t)BusWatch::KEEP);
}

int main() {
  test_names();
  test_no_alarm_for_what_a_master_here_sends();
  test_alarm_names_the_reconfiguration();
  test_payload_hex_hides_only_the_config_block();
  test_a_request_and_its_reply_are_one_exchange();
  test_results();
  test_a_late_reply_finds_its_request();
  test_summaries();
  test_an_answer_joins_its_query();
  test_another_master();
  test_stats_cover_five_minutes();
  test_log_is_bounded();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
