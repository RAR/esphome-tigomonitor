// Host test for tap_config.h: the radio settings block, the values chosen
// for a spare TAP, the send gate's shape for the write, and ProvisionJob
// against a fake TAP that keeps a RAM copy and a saved copy as the owner's
// analysis describes (the write changes RAM, the stored read saves it).
//
// Every PAN, key and MAC here is made up. The map is the one the CCA wrote
// on the bench, with bytes 5-7 zero as the current CCA writes them.

#include "tap_config.h"
#include "bus_watch.h"

#include <cstdio>
#include <functional>
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
  b.map = {0x0C, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x01};
  for (int i = 0; i < 16; i++) b.key[i] = static_cast<uint8_t>(0x11 * (i % 15 + 1));
  return b;
}

// The CCA's write, field by field, as the 11-06 capture shows its shape:
// 01 00, then channel, PAN, map, SLT, key, optimization, SLm. The PAN and key
// here are made up; the capture's own stay out of the repo.
static void test_block_layout() {
  Bytes want = hex("0017" "4C3D" "0C020201" "00000000" "1C01" "5A"
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
  CHECK(map_for_profile(12, m) && m == (std::array<uint8_t, 10>{12, 2, 2, 1, 0, 0, 0, 0, 28, 1}));
  // The 300 map: 24-4-2-1, the same 28/1 (owner's analysis, 2026-11-11).
  CHECK(map_for_profile(24, m) && m == (std::array<uint8_t, 10>{24, 4, 2, 1, 0, 0, 0, 0, 28, 1}));
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
  // The gateway Store: a provisioning request's (a G8 saves only with it).
  CHECK(may_send(CMD_STORE, key, true, false, false, false, false, true));
  CHECK(may_send(CMD_STORE, key, false, false, false, false, false, true));
  CHECK(!may_send(CMD_STORE, key, true));
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
  // The stored read answers with something other than RAM (the roof's G8.65
  // on 2026-11-13: "after the save the TAP runs something else").
  std::function<void(RadioBlock &)> stored_shows;
  std::vector<Bytes> got;
  int resets = 0;
  // The G8.59's form: the active block (read 00 00, the write and its echo)
  // is 46 bytes; the stored read (00 01) answers with 36.
  bool wide = false;
  // A G8 (the analyst's trace, 2026-11-13): the stored read answers with
  // the stored copy, and only the gateway Store (0x0012 + the key) saves
  // RAM to it, answering 0x13 with 19 bytes.
  bool g8 = false;
  bool store_silent = false;
  RadioBlock stored_copy;
  int stores = 0;
  bool answer(const Request &req, Frame &reply) {
    if (req.cmd == CMD_STORE) {
      stores++;
      if (store_silent) return false;
      stored_copy = ram;
      reply.cmd = CMD_STORE + 1;
      reply.payload = Bytes(19, 0xA5);
      return true;
    }
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
    if (args.size() == (wide ? 48u : 38u) && args[0] == 0x01) RadioBlock::decode(Bytes(args.begin() + 2, args.end()), ram);
    if (args == Bytes{0x00, 0x01}) save_armed = true;
    RadioBlock shown = ram;
    if (args == Bytes{0x00, 0x01}) {
      if (g8) shown = stored_copy;
      shown.wide = false;
      if (stored_shows) stored_shows(shown);
    }
    Bytes data = shown.encode();
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
    if (!r.cmd) continue;  // a turn left to the master
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

// A TAP that already has a profile (a CCA's, say, for 120 optimizers) keeps
// it; one without gets the 40-optimizer default. The result says which.
static void test_provision_keeps_the_taps_profile() {
  {
    FakeTap tap;
    tap.ram = made_up();
    tap.ram.map = {20, 2, 2, 1, 0, 0x10, 0x20, 0x30, 28, 1};
    RadioBlock want = made_up();
    std::array<uint8_t, 10> m{};
    map_for_profile(12, m);
    want.map = m;
    want.keep_tap_profile = true;
    ProvisionJob job(TAP_MAC, want, false, false);
    run(job, tap);
    CHECK(job.ok() && tap.ram.map[0] == 20 && tap.ram.map[8] == 28 && tap.ram.map[9] == 1);
    CHECK(job.message().find("up to 120 optimizers") != std::string::npos);
  }
  {  // blank: the default
    FakeTap tap;
    tap.ram = made_up();
    tap.ram.map = {};
    RadioBlock want = made_up();
    std::array<uint8_t, 10> m{};
    map_for_profile(12, m);
    want.map = m;
    want.keep_tap_profile = true;
    ProvisionJob job(TAP_MAC, want, false, false);
    run(job, tap);
    CHECK(job.ok() && tap.ram.map[0] == 12 && job.message().find("up to 40 optimizers") != std::string::npos);
  }
  CHECK(profile_capacity(12) == 40 && profile_capacity(14) == 75 && profile_capacity(20) == 120 &&
        profile_capacity(24) == 300 && profile_capacity(7) == 0);
}

// Changing only the profile: the TAP's channel, PAN and key are kept from
// its own read, so its joined optimizers see the same network.
static void test_provision_keeps_the_network_changes_the_profile() {
  FakeTap tap;
  tap.ram = made_up();
  tap.ram.channel = 13;
  tap.ram.pan = 0x7E21;
  for (int i = 0; i < 16; i++) tap.ram.key[i] = static_cast<uint8_t>(0x30 + i);
  RadioBlock want;  // what the request carries: placeholders and the new profile
  want.channel = 11;
  want.pan = 1;
  map_for_profile(14, want.map);
  want.keep_network = true;
  ProvisionJob job(TAP_MAC, want, false, false);
  run(job, tap);
  CHECK(job.ok());
  CHECK(tap.ram.channel == 13 && tap.ram.pan == 0x7E21 && tap.ram.key[0] == 0x30 && tap.ram.key[15] == 0x3F);
  CHECK(tap.ram.map[0] == 14 && job.message().find("up to 75 optimizers") != std::string::npos);
  CHECK(job.message().find("3031") == std::string::npos);  // the key still hidden
}

// A profile change the TAP runs but doesn't store (the roof's G8.65,
// 2026-11-13: the live read showed the new map, the stored read the old).
// Not a failure: it says the TAP runs the new profile now and goes back to
// the stored one when it restarts, and skips any reset, which would undo it.
// A change of the network itself that isn't stored still fails.
static void test_provision_profile_not_stored() {
  RadioBlock want;
  want.channel = 11;
  want.pan = 1;
  map_for_profile(20, want.map);
  want.keep_network = true;
  {
    FakeTap tap;
    tap.ram = made_up();
    map_for_profile(14, tap.ram.map);
    std::array<uint8_t, 10> old = tap.ram.map;
    tap.stored_shows = [old](RadioBlock &b) { b.map = old; };
    ProvisionJob job(TAP_MAC, want, /*reset=*/true, false);
    run(job, tap);
    std::string m = job.message();
    CHECK(job.ok());
    CHECK(tap.resets == 0);
    CHECK(tap.ram.map[0] == 20);
    CHECK(m.find("up to 120 optimizers") != std::string::npos);
    CHECK(m.find("didn't save it") != std::string::npos);
    CHECK(m.find("up to 75") != std::string::npos);  // what it goes back to
    CHECK(m.find("restarts") != std::string::npos);
  }
  {
    FakeTap tap;
    tap.stored_shows = [](RadioBlock &b) { b.channel = 15; };
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("after the save the TAP runs something else") != std::string::npos);
  }
}

// The bench's Store test (owner, 2026-11-13): asked to, on a TAP that kept
// the old profile, the job sends the gateway Store and reads the stored copy
// again, saying what it now holds.
static void test_saves_with_store() {
  CHECK(saves_with_store("Mgate Version G8.65 May  9 2025 13:52:34 GW-H150.6.0S0.12"));
  CHECK(saves_with_store("Mgate Version G8.59 Jan  1 2024"));
  CHECK(!saves_with_store("Mgate Version H1.0007 Apr 22 2025 15:28:25 GW-H158.7.00"));
  CHECK(saves_with_store("G8.65 May  9 2025"));
  CHECK(!saves_with_store("Mgate Version XG8.1"));
  CHECK(!saves_with_store(""));
}

static void test_provision_store() {
  RadioBlock want;
  want.channel = 11;
  want.pan = 1;
  map_for_profile(20, want.map);
  want.keep_network = true;
  want.store = true;
  {
    FakeTap tap;
    tap.g8 = true;
    tap.ram = made_up();
    map_for_profile(14, tap.ram.map);
    tap.stored_copy = tap.ram;
    ProvisionJob job(TAP_MAC, want, false, false);
    run(job, tap);
    std::string m = job.message();
    CHECK(job.ok());
    CHECK(tap.stores == 1);
    CHECK(tap.stored_copy.map[0] == 20);
    // read, write, stored read, check, then the stored read again after the Store
    CHECK(tap.got.size() == 5);
    if (tap.got.size() == 5) CHECK(Bytes(tap.got[4].begin() + 5, tap.got[4].end()) == (Bytes{0x00, 0x01}));
    CHECK(m.find("Store answered 0x13") != std::string::npos);
    CHECK(m.find("19 bytes") != std::string::npos);
    CHECK(m.find("saved with the Store") != std::string::npos);
    CHECK(m.find("112233") == std::string::npos);
  }
  {  // no answer to the Store
    FakeTap tap;
    tap.g8 = true;
    tap.store_silent = true;
    tap.ram = made_up();
    map_for_profile(14, tap.ram.map);
    tap.stored_copy = tap.ram;
    ProvisionJob job(TAP_MAC, want, false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("didn't answer the Store") != std::string::npos);
  }
  {  // a G8, not asked: it saves only with the Store, so the job sends it
    FakeTap tap;
    tap.g8 = true;
    tap.ram = made_up();
    tap.stored_copy = tap.ram;
    RadioBlock g8 = made_up();
    g8.channel = 15;  // a whole new network: the stored copy differs until the Store
    g8.saved_by_store = true;
    ProvisionJob job(TAP_MAC, g8, false, false);
    run(job, tap);
    CHECK(job.ok());
    CHECK(tap.stores == 1 && tap.stored_copy.channel == 15);
    CHECK(job.message().find("saved with the Store") != std::string::npos);
  }
  {  // a G8 whose Store saves nothing: stopped, said
    FakeTap tap;
    tap.g8 = true;
    tap.ram = made_up();
    tap.stored_copy = tap.ram;
    RadioBlock g8 = made_up();
    g8.channel = 15;
    g8.saved_by_store = true;
    std::function<void(RadioBlock &)> old = [](RadioBlock &b) { b.channel = 11; };
    tap.stored_shows = old;
    ProvisionJob job(TAP_MAC, g8, false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("after the Store") != std::string::npos);
  }
  {  // not asked: no Store
    FakeTap tap;
    tap.g8 = true;
    tap.ram = made_up();
    map_for_profile(14, tap.ram.map);
    tap.stored_copy = tap.ram;
    RadioBlock plain = want;
    plain.store = false;
    ProvisionJob job(TAP_MAC, plain, false, false);
    run(job, tap);
    CHECK(job.ok() && tap.stores == 0);
  }
}

// Sizing, the CCA's rule: the smallest map whose capacity covers the TAP's
// table (burden). A TAP keeps its own map only while that still covers it.
// Bytes 5-7 are always zero: the CCA's map parser never writes them.
static RadioBlock sized(uint16_t burden) {
  RadioBlock b = made_up();
  map_for_profile(12, b.map);
  b.keep_tap_profile = true;
  b.burden = burden;
  return b;
}
static std::array<uint8_t, 10> after(const std::array<uint8_t, 10> &tap_map, uint16_t burden) {
  FakeTap tap;
  tap.ram = made_up();
  tap.ram.map = tap_map;
  ProvisionJob job(TAP_MAC, sized(burden), false, false);
  run(job, tap);
  CHECK(job.ok());
  return tap.ram.map;
}
static void test_provision_sizes_the_map() {
  const std::array<uint8_t, 10> m40 = {12, 2, 2, 1, 0, 0xE7, 0xB4, 0x76, 28, 1};  // the CCA's, junk in 5-7
  const std::array<uint8_t, 10> m120 = {20, 2, 2, 1, 0, 0, 0, 0, 28, 1};
  const std::array<uint8_t, 10> blank = {};
  CHECK(after(m40, 40) == (std::array<uint8_t, 10>{12, 2, 2, 1, 0, 0, 0, 0, 28, 1}));  // fits: kept, 5-7 zeroed
  CHECK(after(m40, 41)[0] == 14);    // outgrown: the next size
  CHECK(after(m120, 30)[0] == 20);   // bigger than needed: kept, never shrunk under it
  CHECK(after(blank, 0)[0] == 12);   // none: the smallest
  CHECK(after(blank, 130) == (std::array<uint8_t, 10>{24, 4, 2, 1, 0, 0, 0, 0, 28, 1}));
  CHECK(profile_for(0) == 12 && profile_for(40) == 12 && profile_for(41) == 14 && profile_for(75) == 14 &&
        profile_for(76) == 20 && profile_for(121) == 24 && profile_for(300) == 24);
}

// The CCA's period for a map (its Calculate button; the owner's analysis of
// its firmware), and the cycle it rounds to: every value the owner read off
// the CCA on 2026-11-11.
static void test_map_period() {
  auto mp = [](std::initializer_list<int> levels, int star) {
    std::array<uint8_t, 10> m{};
    int i = 0;
    for (int v : levels) m[i++] = static_cast<uint8_t>(v);
    m[8] = 28;
    m[9] = static_cast<uint8_t>(star);
    return map_period_slots(m);
  };
  CHECK(mp({12}, 1) == 32 && mp({13}, 1) == 33 && mp({12, 2}, 1) == 68);
  CHECK(mp({12, 2, 2, 1}, 1) == 354 && mp({13, 2, 2, 1}, 1) == 386);
  CHECK(mp({12, 2, 2, 2}, 1) == 513 && mp({12, 2, 3, 1}, 1) == 513 && mp({12, 2, 2, 1, 1}, 1) == 598);
  CHECK(mp({12, 2, 2, 1}, 2) == 387 && mp({12, 2, 2, 1}, 3) == 424 && mp({12, 2, 2, 1}, 4) == 471);
  // The cycle, in hyperslots (200 a second): the next step up.
  CHECK(period_hyperslots(354) == 400 && period_hyperslots(418) == 600 && period_hyperslots(629) == 800);
  CHECK(period_hyperslots(5000) == 6000 && period_hyperslots(7000) == 0);
  std::array<uint8_t, 10> m{};
  map_for_profile(14, m);
  CHECK(map_cycle_text(m) == "every 3 s");
  map_for_profile(12, m);
  CHECK(map_cycle_text(m) == "every 2 s");
}

// After its read, the job gives the master one turn (its poll, as the CCA
// sent between its read and its write) before writing.
static void test_provision_gives_a_turn_before_writing() {
  FakeTap tap;
  ProvisionJob job(TAP_MAC, made_up(), false, false);
  uint8_t seq = 0;
  std::vector<int> due_cmds;  // 0: a turn left to the master
  for (int i = 0; i < 20 && !job.finished(); i++) {
    Request r = job.due(0, seq);
    due_cmds.push_back(r.cmd ? (r.cmd == CMD_TUNNEL ? r.payload[5] : 0x100) : 0);
    if (!r.cmd) continue;
    Frame reply;
    bool ok = tap.answer(r, reply);
    job.on_reply(r, ok, reply, 0);
  }
  CHECK(job.ok());
  // GET_MAC, the read (00), a turn left (0), the write (01), the save, the check.
  CHECK(due_cmds.size() >= 4 && due_cmds[0] == 0x100 && due_cmds[1] == 0x00 && due_cmds[2] == 0 && due_cmds[3] == 0x01);
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
  {  // status 1 with an echo of exactly what was sent: carried on, and said
    // (the bench H1.0007 answers a whole 36-byte write so; 2026-11-11)
    FakeTap tap;
    tap.status = 1;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(job.ok() && tap.got.size() == 4 && tap.save_armed);
    CHECK(job.message().find("status 1") != std::string::npos);
  }
  {  // status 1 and an echo that differs: stopped before the save
    FakeTap tap;
    tap.status = 1;
    tap.echo_wrong = true;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && tap.got.size() == 2);
  }
  {  // an echo that differs
    FakeTap tap;
    tap.echo_wrong = true;
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    CHECK(!job.ok() && job.message().find("echo differs") != std::string::npos);
  }
  {  // a stored read that differs: says which fields, never the key or PAN
    FakeTap tap;
    tap.stored_shows = [](RadioBlock &b) {
      b.map[0] = 0x0E;
      b.map[6] = 0x84;
      b.pan ^= 1;
      b.key[0] ^= 1;
    };
    ProvisionJob job(TAP_MAC, made_up(), false, false);
    run(job, tap);
    std::string m = job.message();
    CHECK(!job.ok() && m.find("after the save the TAP runs something else") != std::string::npos);
    CHECK(m.find("map") != std::string::npos && m.find("0e") != std::string::npos);
    CHECK(m.find("the PAN") != std::string::npos && m.find("the key") != std::string::npos);
    CHECK(m.find("36 bytes") != std::string::npos);
    char pan[8];
    std::snprintf(pan, sizeof(pan), "%04X", made_up().pan);
    CHECK(m.find(pan) == std::string::npos);  // the PAN itself never shown
    CHECK(m.find("channel") == std::string::npos);  // what agrees isn't listed
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

// The G8.59's 46-byte block: 10 bytes nobody has explained yet sit between
// the key and optimization. Made-up values.
static RadioBlock made_up_wide() {
  RadioBlock b = made_up();
  b.wide = true;
  b.extra = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA};
  return b;
}

static void test_wide_layout() {
  Bytes want = hex("0017" "4C3D" "0C020201" "00000000" "1C01" "5A"
                   "112233445566778899AABBCCDDEEFF11" "A1A2A3A4A5A6A7A8A9AA" "00000200" "3C");
  CHECK(made_up_wide().encode() == want);
  CHECK(want.size() == RADIO_BLOCK_WIDE);
  RadioBlock back;
  CHECK(RadioBlock::decode(want, back) && back.wide && back == made_up_wide());
  CHECK(back.optimization == 0x200 && back.slm == 0x3C);
  // The settings are the same as the 36-byte form's; only the form differs.
  CHECK(back.same_settings(made_up()) && back != made_up());
  CHECK(radio_write_args(made_up_wide()).size() == 48);
  // The gate takes either form, and nothing in between.
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 1, radio_write_args(made_up_wide()))) == Allow::provision);
  Bytes odd = radio_write_args(made_up_wide());
  odd.pop_back();
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x0D, 1, odd)) == Allow::never);
  // No key, and none of the unexplained bytes, in what a person sees.
  std::string seen = radio_write_text(made_up_wide());
  CHECK(seen.find("112233") == std::string::npos && seen.find("A1A2") == std::string::npos);
  // The bus watch reads the TAP's channel and SLm from either form.
  Frame req{0x1201, CMD_TUNNEL, tunnel_request(0x0D, 1, {0x00, 0x00})};
  Frame rep{0x9201, CMD_TUNNEL_REPLY, {0x00, 0x0E, 0x00, 0x0E, 0x01}};
  Bytes data = want;
  data.push_back(0x00);
  rep.payload.insert(rep.payload.end(), data.begin(), data.end());
  TapRadio r;
  CHECK(parse_gateway_cfg(req, rep, r) && r.channel == 23 && r.slt == 0x5A && r.slm == 0x3C);
  // And the profile it runs, from the live read (00 00) only: the stored
  // read (00 01) can hold another (the roof's G8.65, 2026-11-13).
  RadioBlock b20 = made_up_wide();
  map_for_profile(20, b20.map);
  Bytes d20 = b20.encode();
  d20.push_back(0x00);
  Frame rep20{0x9201, CMD_TUNNEL_REPLY, {0x00, 0x0E, 0x00, 0x0E, 0x01}};
  rep20.payload.insert(rep20.payload.end(), d20.begin(), d20.end());
  TapRadio live;
  CHECK(parse_gateway_cfg(req, rep20, live) && live.profile == 120);
  Frame stored_req{0x1201, CMD_TUNNEL, tunnel_request(0x0D, 2, {0x00, 0x01})};
  TapRadio stored;
  CHECK(parse_gateway_cfg(stored_req, rep20, stored) && stored.profile == 0);
}

// On a TAP with the 46-byte form, the job writes that form, keeping the
// TAP's own 10 unexplained bytes as they are rather than making them up.
static void test_provision_wide() {
  FakeTap tap;
  tap.wide = true;
  tap.ram = made_up_wide();
  tap.ram.channel = 15;
  tap.ram.extra = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  ProvisionJob job(TAP_MAC, made_up(), false, false);
  run(job, tap);
  CHECK(job.ok());
  CHECK(tap.got.size() == 4);
  if (tap.got.size() == 4) CHECK(tap.got[1].size() == 5 + 48);
  CHECK(tap.ram.wide && tap.ram.channel == 23);
  CHECK(tap.ram.extra == (std::array<uint8_t, 10>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  CHECK(tap.ram.same_settings(made_up()));
}

// The report period, tunnel 0x22 (owner's analysis, 2026-11-12): read
// 00 <sel>, write 01 <sel> <code>, the reply 0x23 <code>; codes 1-11.
static void test_period_gate() {
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x00, 0x03})) == Allow::provision);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x01, 0x0B})) == Allow::provision);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x02, 0x03})) == Allow::never);  // no third setting
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x00, 0x00})) == Allow::never);
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x00, 0x0C})) == Allow::never);  // the scan's 12
  CHECK(classify_request(CMD_TUNNEL, tunnel_request(0x22, 1, {0x01, 0x00, 0x03, 0x00})) == Allow::never);
  Bytes w = tunnel_request(0x22, 1, {0x01, 0x00, 0x03});
  CHECK(!may_send(CMD_TUNNEL, w, true, true, true, true, true));  // only as a provisioning request
  CHECK(may_send(CMD_TUNNEL, w, false, false, false, false, false, true));
  CHECK(period_code_hyperslots(1) == 200 && period_code_hyperslots(3) == 600 && period_code_hyperslots(11) == 6000);
  CHECK(period_code_hyperslots(0) == 0 && period_code_hyperslots(12) == 0);
  CHECK(period_code_text(3) == "3 s" && period_code_text(7) == "10 s");
}

// A write's reply holds the code from before it, as the bench's H1's
// first did (2026-11-12); linked: one write moves both, as 01 once followed 00.
struct PeriodTap {
  uint8_t code[2] = {2, 2};
  bool linked = true;
  bool deaf_to_writes = false;  // answers, but keeps its code
  bool silent = false;
  std::vector<Bytes> got;
  bool answer(const Request &req, Frame &reply) {
    Bytes args(req.payload.begin() + 5, req.payload.end());
    got.push_back(args);
    if (silent || args.size() < 2 || args[1] > 1) return false;
    uint8_t shown = code[args[1]];
    if (args[0] == 0x01 && args.size() == 3 && !deaf_to_writes) {
      if (linked) code[0] = code[1] = args[2];
      else code[args[1]] = args[2];
    }
    reply.cmd = CMD_TUNNEL_REPLY;
    reply.payload = {0x00, 0x0E, 0x00, 0x23, req.payload[4], shown};
    return true;
  }
};

static void run(PeriodJob &job, PeriodTap &tap) {
  uint8_t seq = 0;
  for (int i = 0; i < 20 && !job.finished(); i++) {
    Request r = job.due(0, seq);
    if (!r.cmd) continue;
    CHECK(r.provision && r.table_edit);
    CHECK(r.payload[3] == 0x22);
    Frame reply;
    bool ok = tap.answer(r, reply);
    job.on_reply(r, ok, reply, 0);
  }
}

// The bench's H1: one write, and both read back changed.
static void test_period_one_write_moves_both() {
  PeriodTap tap;
  PeriodJob job(TAP_MAC, 3, false);
  run(job, tap);
  CHECK(job.ok());
  std::vector<Bytes> want = {{0x00, 0x00}, {0x00, 0x01}, {0x01, 0x00, 0x03}, {0x00, 0x00}, {0x00, 0x01}};
  CHECK(tap.got == want);
  CHECK(job.before(0) == 2 && job.before(1) == 2);
  CHECK(job.message() == "report period 3 s on both (was 2 s and 2 s)");
}

// Two settings of their own: the second is written once the first's
// read-back shows it still differs.
static void test_period_writes_both_then_reads_back() {
  PeriodTap tap;
  tap.linked = false;
  PeriodJob job(TAP_MAC, 3, false);
  run(job, tap);
  CHECK(job.ok());
  std::vector<Bytes> want = {{0x00, 0x00}, {0x00, 0x01}, {0x01, 0x00, 0x03}, {0x00, 0x00},
                             {0x00, 0x01}, {0x01, 0x01, 0x03}, {0x00, 0x00}, {0x00, 0x01}};
  CHECK(tap.got == want);
  CHECK(job.message() == "report period 3 s on both (was 2 s and 2 s)");
}

static void test_period_writes_only_what_differs() {
  PeriodTap tap;
  tap.linked = false;
  tap.code[0] = 3;
  PeriodJob job(TAP_MAC, 3, false);
  run(job, tap);
  CHECK(job.ok());
  std::vector<Bytes> want = {{0x00, 0x00}, {0x00, 0x01}, {0x01, 0x01, 0x03}, {0x00, 0x00}, {0x00, 0x01}};
  CHECK(tap.got == want);
  PeriodTap set;
  set.code[0] = set.code[1] = 3;
  PeriodJob already(TAP_MAC, 3, false);
  run(already, set);
  CHECK(already.ok() && set.got.size() == 2);
  CHECK(already.message() == "the report period is already 3 s on both: nothing written");
}

static void test_period_stops() {
  PeriodTap deaf;
  deaf.deaf_to_writes = true;
  PeriodJob job(TAP_MAC, 3, false);
  run(job, deaf);
  CHECK(!job.ok() && job.finished());
  CHECK(job.message() == "after the write the TAP still reads 2 s, not 3 s");
  CHECK(deaf.got.size() == 5);  // tried once: read, read, write, read, read
  PeriodTap silent;
  silent.silent = true;
  PeriodJob mute(TAP_MAC, 3, false);
  run(mute, silent);
  CHECK(!mute.ok() && mute.message() == "the TAP didn't answer a read of its report period");
  CHECK(silent.got.size() == 1);
}

static void test_period_dry_run() {
  PeriodTap tap;
  PeriodJob job(TAP_MAC, 4, true);
  run(job, tap);
  CHECK(job.ok());
  CHECK(tap.got.size() == 2);  // the reads only
  CHECK(tap.code[0] == 2 && tap.code[1] == 2);
  CHECK(job.dry_frames().size() == 2);
  if (job.dry_frames().size() == 2) {
    CHECK(job.dry_frames()[0] == (Bytes{0x22, 0x01, 0x00, 0x04}));
    CHECK(job.dry_frames()[1] == (Bytes{0x22, 0x01, 0x01, 0x04}));
  }
  CHECK(job.message() == "dry run: would set the report period to 4 s on both (now 2 s and 2 s)");
}

int main() {
  test_block_layout();
  test_wide_layout();
  test_provision_wide();
  test_values();
  test_gate();
  test_provision();
  test_provision_with_reset();
  test_provision_gives_a_turn_before_writing();
  test_provision_keeps_the_taps_profile();
  test_provision_keeps_the_network_changes_the_profile();
  test_provision_profile_not_stored();
  test_provision_store();
  test_saves_with_store();
  test_provision_sizes_the_map();
  test_map_period();
  test_provision_stops();
  test_dry_run();
  test_period_gate();
  test_period_one_write_moves_both();
  test_period_writes_both_then_reads_back();
  test_period_writes_only_what_differs();
  test_period_stops();
  test_period_dry_run();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
