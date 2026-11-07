// Host tests for bus_watch.h: naming what is said on the bus, and spotting
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
  CHECK(reconfiguration(request(0x0014, hex("3724926612 35"), 0xFFFF)).empty());
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
}

static void test_log_skips_polls_and_names_frames() {
  BusWatch w;
  w.on_frame(request(0x0148, hex("00016fc404")), false, 1000);
  w.on_frame(reply(0x0149, hex("0000")), false, 1001);
  CHECK(w.events().empty() && w.polls() == 2);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x2E, 5, {})), true, 2000);
  w.on_frame(reply(0x0B10, hex("000e002f05010384002c0028002c00")), false, 2010);
  CHECK(w.events().size() == 2);
  const BusEvent &a = w.events()[0], &b = w.events()[1];
  CHECK(a.unix_ms == 2000 && a.ours && !a.from_tap && a.addr == 0x1201 && a.cmd == CMD_TUNNEL && a.sub == 0x2E);
  CHECK(a.name == "Tunnel · DiscState" && a.len == 5 && a.reconfig.empty());
  CHECK(b.from_tap && !b.ours && b.sub == 0x2F && b.name == "Tunnel reply · DiscState reply");
  // Text: the query itself, and VERSION's reply; nothing else's payload.
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x06, 6, text_query(2, "^00Info\r"))), false, 3000);
  CHECK(w.events().back().text == "^00Info" && w.events().back().node == 2);
  w.on_frame(reply(0x000B, Bytes{'M', 'g', 'a', 't', 'e', ' ', 'G', '8', '.', '5', '9', '\r'}), false, 3001);
  CHECK(w.events().back().text == "Mgate G8.59");
  w.on_frame(reply(0x0B10, hex("000e0027070002") /* a table page */), false, 3002);
  CHECK(w.events().back().text.empty());
  w.on_frame(request(0x003C, hex("3724926604c05b0000ab12cd1202")), false, 3003);
  CHECK(w.events().back().text.empty() && w.events().back().name == "SET_ID_BY_MAC");
}

// The device's own UI already shows every address and barcode, so text is
// shown as it is.
static void test_text_is_shown_as_it_is() {
  BusWatch w;
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x06, 1, text_query(2, "^00X 04C05B0000AB12CD ab12\r"))), false, 1);
  CHECK(w.events().back().text == "^00X 04C05B0000AB12CD ab12");
}

// Every payload in hex, but for the GatewayCfg block, which holds the
// network key: hidden in the write and in the TAP's reply, read or not.
static void test_payload_hex_hides_only_the_config_block() {
  BusWatch w;
  w.on_frame(request(0x003C, hex("3724926604c05b0000ab12cd1202")), false, 1);
  CHECK(w.events().back().hex == "37 24 92 66 04 C0 5B 00 00 AB 12 CD 12 02");
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x0D, 3, {0x00, 0x01})), false, 2);
  CHECK(w.events().back().hex == "00 00 00 0D 03 00 01");  // a read asks for nothing secret
  Bytes cfg = {0x01, 0x00};
  for (int i = 0; i < 36; i++) cfg.push_back(static_cast<uint8_t>(0xA0 + i));
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0x0D, 4, cfg)), false, 3);
  CHECK(w.events().back().hex == "00 00 00 0D 04 01 00 + 36 bytes hidden (the TAP's network config and key)");
  Bytes reply_payload = {0x00, 0x0E, 0x00, 0x0E, 0x03};
  for (int i = 0; i < 37; i++) reply_payload.push_back(static_cast<uint8_t>(0xB0 + i));
  w.on_frame(reply(0x0B10, reply_payload), false, 4);
  CHECK(w.events().back().hex == "00 0E 00 0E 03 + 37 bytes hidden (the TAP's network config and key)");
  for (const BusEvent &e : w.events()) CHECK(e.hex.find("A5") == std::string::npos && e.hex.find("B5") == std::string::npos);
  w.on_frame(request(CMD_TUNNEL, tunnel_request(0xE1, 5, hex("12345678aabbccdd"))), false, 5);
  CHECK(w.events().back().hex == "00 00 00 E1 05 + 8 bytes hidden (a list of networks, perhaps with keys)");
  w.on_frame(request(0x000A, {}), false, 6);
  CHECK(w.events().back().hex.empty());
}

static void test_reconfiguration_is_kept_on_the_event() {
  BusWatch w;
  std::string what = w.on_frame(request(0x000C, {0x20, 0x00}), false, 5000);
  CHECK(what == "SET_CHANNEL");
  CHECK(w.events().back().reconfig == "SET_CHANNEL");
}

static void test_log_is_bounded() {
  BusWatch w;
  for (int i = 0; i < 1000; i++) w.on_frame(request(0x000A, {}), false, i);
  CHECK(w.events().size() == BusWatch::KEEP && w.events().front().unix_ms == 1000 - BusWatch::KEEP);
}

int main() {
  test_names();
  test_no_alarm_for_what_a_master_here_sends();
  test_alarm_names_the_reconfiguration();
  test_log_skips_polls_and_names_frames();
  test_text_is_shown_as_it_is();
  test_payload_hex_hides_only_the_config_block();
  test_reconfiguration_is_kept_on_the_event();
  test_log_is_bounded();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
