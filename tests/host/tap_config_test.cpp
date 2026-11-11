// Host test for tap_config.h: the radio settings block, the values chosen
// for a spare TAP, the send gate's shape for the write, and ProvisionJob
// against a fake TAP that keeps a RAM copy and a saved copy as the owner's
// analysis describes (the write changes RAM, the stored read saves it).
//
// Every PAN, key and MAC here is made up. The map is the one the CCA wrote
// on the bench on 2026-11-06, bytes 5-7 included as it sent them.

#include "tap_config.h"
#include "bus_watch.h"

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

static const Bytes TAP_MAC = hex("04C05B0000A1B2C3");

static RadioBlock made_up() {
  RadioBlock b;
  b.channel = 23;
  b.pan = 0x4C3D;
  b.map = {0x0C, 0x02, 0x02, 0x01, 0x00, 0xFF, 0x84, 0x01, 0x1C, 0x03};
  for (int i = 0; i < 16; i++) b.key[i] = static_cast<uint8_t>(0x11 * (i % 15 + 1));
  return b;
}

// The CCA's write, field by field, as the 11-06 capture shows its shape:
// 01 00, then channel, PAN, map, SLT, key, optimization, SLm. The PAN and key
// here are made up; the capture's own stay out of the repo.
static void test_block_layout() {
  Bytes want = hex("0017" "4C3D" "0C020201" "00FF8401" "1C03" "5A"
                   "112233445566778899AABBCCDDEEFF11" "00000200" "3C");
  CHECK(made_up().encode() == want);
  CHECK(want.size() == RADIO_BLOCK_SIZE);
  RadioBlock back;
  CHECK(RadioBlock::decode(want, back) && back == made_up());
  Bytes args = radio_write_args(made_up());
  CHECK(args.size() == 38 && args[0] == 0x01 && args[1] == 0x00);
  // No key in what a person sees.
  std::string seen = radio_write_text(made_up());
  CHECK(seen.find("key hidden") != std::string::npos);
  CHECK(seen.find("112233") == std::string::npos && seen.find("4C3D") != std::string::npos);
}

static void test_values() {
  std::array<uint8_t, 10> m{};
  CHECK(map_for_profile(12, m) && m == (std::array<uint8_t, 10>{12, 2, 2, 1, 0, 0, 0, 0, 28, 3}));
  CHECK(!map_for_profile(24, m));  // its parameters aren't known
  // Channels: two clear of each in use, so 20 rules out 19-21.
  CHECK(channel_for({}) == 11);
  CHECK(channel_for({11}) == 13);
  CHECK(channel_for({12, 15}) == 17);
  // PAN: the MAC's low 16 bits, stepped by 0x1000 past one in use.
  CHECK(pan_for(TAP_MAC, {}) == 0xB2C3);
  CHECK(pan_for(TAP_MAC, {0xB2C3}) == 0xC2C3);
  CHECK(pan_for(hex("04C05B000000FFFF"), {}) == 0x0FFF);  // never 0xFFFF
  // A key with no 00 or FF, from whatever the draw gives.
  int n = 0;
  auto k = key_from([&n]() { return static_cast<uint8_t>(n++ % 2 ? 0xFF : 0x42); });
  for (uint8_t b : k) CHECK(b == 0x42);
}

// The gate: the write only in its exact shape, only for a provisioning
// request; the stored-copy write and anything malformed never.
static void test_gate() {
  Bytes write = tunnel_request(0x0D, 1, radio_write_args(made_up()));
  CHECK(classify_request(CMD_TUNNEL, write) == Allow::provision);
  CHECK(!may_send(CMD_TUNNEL, write, true, true, true, true, true));             // everything else on, not provisioning
  CHECK(may_send(CMD_TUNNEL, write, false, false, false, false, false, true));
  // 01 01, the stored copy: never, even provisioning.
  Bytes stored = radio_write_args(made_up());
  stored[1] = 0x01;
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 1, stored)) == Allow::never);
  // A channel the TAP refuses, or a short block: never.
  RadioBlock bad = made_up();
  bad.channel = 27;
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 1, radio_write_args(bad))) == Allow::never);
  Bytes cut = radio_write_args(made_up());
  cut.pop_back();
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 1, cut)) == Allow::never);
  // The reads and the reset: a provisioning request may send them; nothing
  // else outside a bring-up may.
  Bytes save = tunnel_request(0x0D, 1, {0x00, 0x01});
  CHECK(may_send(CMD_TUNNEL, save, false, false, false, false, false, true));
  CHECK(!may_send(CMD_TUNNEL, save, true));
  CHECK(may_send(CMD_GET_MAC_NOW, {}, false, false, false, false, false, true));
  CHECK(!may_send(CMD_GET_MAC_NOW, {}, true));
  Bytes key = {0x37, 0x24, 0x92, 0x66};
  CHECK(may_send(CMD_RESET, key, false, false, false, false, false, true));
  CHECK(!may_send(CMD_RESET, key, true));
  // Provisioning doesn't open anything else: a table erase, SetKnownNetworks.
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(SUB_ERASE_FLASH, 1, {0xBA, 0xBE}), false, false, false, false, false, true));
  CHECK(!may_send(CMD_TUNNEL, tunnel_request(0xE1, 1, {0x01}), false, false, false, false, false, true));
  // A write heard from another device is still a reconfiguration.
  Frame heard{0x1201, CMD_TUNNEL, write};
  CHECK(reconfiguration(heard).find("GatewayCfg write") != std::string::npos);
  // And the watch raises it as one only when it isn't ours: the bench's own
  // write is named in the log, but no alarm.
  BusWatch w;
  CHECK(w.on_frame(heard, true, 1000).empty());
  CHECK(w.exchanges().back().sub_name == "GatewayCfg write");
  CHECK(w.exchanges().back().request_hex.find("112233") == std::string::npos);  // the key hidden
  CHECK(w.on_frame(heard, false, 5000).find("GatewayCfg write") != std::string::npos);
}

// A fake TAP as the analysis describes it: the write sets RAM and echoes
// it; the stored read marks it to be saved and answers from RAM.
struct FakeTap {
  Bytes mac = TAP_MAC;
  int mac_asks = 0;
  RadioBlock ram, saved;
  bool save_armed = false;
  int status = 0;
  bool echo_wrong = false;
  bool silent = false;
  std::vector<Bytes> got;
  int resets = 0;
  bool answer(const Request &req, Frame &reply) {
    if (req.cmd == CMD_RESET) {
      resets++;
      if (save_armed) saved = ram;
      ram = saved;
      return false;
    }
    if (req.cmd == CMD_GET_MAC_NOW) {
      mac_asks++;
      if (silent) return false;
      reply.cmd = CMD_GET_MAC_NOW + 1;
      reply.payload = mac;
      reply.payload.push_back(0x12);
      reply.payload.push_back(0x01);
      return true;
    }
    got.push_back(req.payload);
    if (silent) return false;
    Bytes args(req.payload.begin() + 5, req.payload.end());
    if (args.size() == 38 && args[0] == 0x01) RadioBlock::decode(Bytes(args.begin() + 2, args.end()), ram);
    if (args == Bytes{0x00, 0x01}) save_armed = true;
    Bytes data = ram.encode();
    if (echo_wrong) data[0] ^= 1;
    data.push_back(static_cast<uint8_t>(status));
    reply.cmd = CMD_TUNNEL_REPLY;
    reply.payload = {0x00, 0x0E, 0x00, 0x0E, req.payload[4]};
    reply.payload.insert(reply.payload.end(), data.begin(), data.end());
    return true;
  }
};

static void run(ProvisionJob &job, FakeTap &tap) {
  uint8_t seq = 0;
  for (int i = 0; i < 20 && !job.finished(); i++) {
    Request r = job.due(0, seq);
    if (!r.cmd) break;
    CHECK(r.provision && r.table_edit);
    Frame reply;
    bool ok = tap.answer(r, reply);
    job.on_reply(r, ok, reply, 0);
  }
}

static void test_provision() {
  FakeTap tap;
  ProvisionJob job(TAP_MAC, made_up(), false, false);
  run(job, tap);
  CHECK(job.ok());
  CHECK(tap.mac_asks == 1);  // it asked which TAP it was talking to, first
  // Then read, write, save, check: in that order, nothing else.
  CHECK(tap.got.size() == 4);
  if (tap.got.size() == 4) {
    CHECK(Bytes(tap.got[0].begin() + 5, tap.got[0].end()) == (Bytes{0x00, 0x00}));
    CHECK(Bytes(tap.got[1].begin() + 5, tap.got[1].end()) == radio_write_args(made_up()));
    CHECK(Bytes(tap.got[2].begin() + 5, tap.got[2].end()) == (Bytes{0x00, 0x01}));
    CHECK(Bytes(tap.got[3].begin() + 5, tap.got[3].end()) == (Bytes{0x00, 0x00}));
  }
  CHECK(tap.save_armed && tap.ram == made_up());
  CHECK(tap.resets == 0);
  CHECK(job.message().find("key hidden") != std::string::npos);
  CHECK(job.message().find("112233") == std::string::npos);
}

static void test_provision_with_reset() {
  FakeTap tap;
  ProvisionJob job(TAP_MAC, made_up(), true, false);
  run(job, tap);
  CHECK(job.ok() && tap.resets == 1);
  CHECK(tap.saved == made_up());  // the save was armed before the reset
}

static void test_provision_stops() {
  {  // a refusal
    FakeTap tap;
    tap.status = 6;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("status 6") != std::string::npos);
    CHECK(tap.got.size() == 2);  // no save after a refused write
  }
  {  // an echo that differs
    FakeTap tap;
    tap.echo_wrong = true;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("echo differs") != std::string::npos);
  }
  {  // no answer at all: not even to which TAP it is
    FakeTap tap;
    tap.silent = true;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && tap.got.empty());
  }
  {  // another TAP at that address: nothing else is sent
    FakeTap tap;
    tap.mac = hex("04C05B0000D4E5F6");
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && tap.got.empty());
    CHECK(job.message().find("04C05B0000D4E5F6") != std::string::npos);
  }
}

// A dry run reads the TAP and sends nothing else; it lists what it would.
static void test_dry_run() {
  FakeTap tap;
  ProvisionJob job(TAP_MAC, made_up(), true, true);
  run(job, tap);
  CHECK(job.ok() && tap.got.size() == 1 && tap.resets == 0);
  CHECK(job.dry_text().size() == 4);
  for (const auto &t : job.dry_text()) CHECK(t.find("112233") == std::string::npos);
}

int main() {
  test_block_layout();
  test_values();
  test_gate();
  test_provision();
  test_provision_with_reset();
  test_provision_stops();
  test_dry_run();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
