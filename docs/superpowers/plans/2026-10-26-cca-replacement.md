# CCA Replacement (phases 1–3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** tigomonitord can run the TAP's RS485 bus itself, with the CCA unplugged. It renews the run lease, polls telemetry and keeps the dashboard fed, and it is proven first on a spare TAP, then on the roof.

**Architecture:** A shared header-only codec (`tigo_master.h`) builds and parses master frames, so host tests cover it and the ESP32 could use it later. The Pi daemon gains a `master` bus role. A `BusMaster` owns the serial port read-write, runs one request/reply transaction at a time, feeds every byte it reads to the existing `Monitor`, and schedules polls, lease checks and renewals from the daemon's main loop. Listen mode stays the default and keeps opening the port read-only.

**Tech Stack:** C++17, header-only shared code under `components/tigo_monitor/`, the Linux daemon in `linux/src/`, host tests in `tests/host/`, Python mocks in `linux/tools/`, e2e scripts in `linux/scripts/` run in the `tigod-dev` container.

**Spec:** `docs/superpowers/specs/2026-10-26-cca-replacement-design.md`. Read its "What the roof CCA actually sends" section first. Background: `~/tigo-protocol-analysis/docs/ROOF_STEADY_STATE.md`. The reference capture is `build/roof/steady-20261026.bin` (git-ignored; get it from the plan's author or re-capture with the `--capture` flag).

## Global Constraints

- Wire format: `00 FF FF` preamble, `7E 07`, escaped body, `7E 08`. Body = DEST(2, big-endian) CMD(2, BE) PAYLOAD CRC16. The CRC uses the existing `protocol::crc16` over the unescaped DEST+CMD+PAYLOAD and goes on the wire **low byte first**. The escape set is `{7E,24,23,25,A4,A3,A5}` → `7E <index 0..6>`.
- A reply's DEST is `request_dest | 0x8000`. The roof gateway id is 0x1201 (requests to 0x1201, replies from 0x9201).
- Tunnel: request CMD 0x0B0F, reply CMD 0x0B10. Request payload is `00 00 00 <sub> <seq> <args…>`; reply payload is `00 <0d|0e> 00 <reply sub> <seq> <data…>`, where the reply sub is usually sub+1, but a renewal (0x2D) is answered with a 0x2F lease reading. seq is one byte, wraps, and is never reused within 255 requests.
- Lease check: sub 0x2E, no args, every 20 s. The 0x2F reply's data, after seq, is: state(1: 01 running, 00 lapsed), lease remaining in s (2, BE), count A (2), count B (2), count C (2), 00. In the capture, A/B/C read 44/40/44 while running and 0/40/44 at lapse. **The node count is C**: the CCA's renewal carried 44 when A read 0.
- Lease renewal: sub 0x2D, args `BA BE 02 03 84 <node_count BE16> 01 00`, sent **only after a lease check reads 0**, exactly as the CCA does. The node count comes from the latest 0x2F reply, never a constant.
- Telemetry poll: CMD 0x0148, payload `00 01 <cursor BE16> 04`, about every 10 ms. Replies are CMD 0x0149, decoded by the existing `Monitor::feed`.
- The roof build may send only: 0x0148; tunnel subs 0x2E, 0x2D (the exact renewal shape), 0x26 (roster pages) and 0x06 with an ASCII `^00` read. Other reads need `master_bench` on. Writes are never sent by anything in this plan: any payload containing `37 24 92 66`, and CMDs 0x0010/0x0012/0x0014/0x003C/0x011E/0x0182/0x0058.
- Nothing renews the lease except the main loop's call into `BusMaster::tick()`. No timer thread, no watchdog that renews.
- Listen mode is unchanged and still opens the port `O_RDONLY`.
- Fixtures and committed files must contain no real serials, barcodes, MACs, site IDs or IPs. Gateway id 0x1201, the node ids and the lease bytes are fine to commit. Roster replies (sub 0x27) and Frame 27 carry MACs and barcodes, so they're never committed.
- Commit style per the `shipping-tigo-changes` skill: `type(scope): subject`, a body with a verification line, no attribution trailers.

## Review Focus

1. **A stalled daemon** (thread wedged, process SIGSTOPped): the lease must lapse, not renew. Test in Task 4 (no renewal unless `tick()` runs), and in Task 7's mock e2e (SIGSTOP the daemon, the mock sees no 0x2D).
2. **A second master on the bus** (master mode switched on with the CCA still plugged in): request frames the daemon didn't send must stop all transmitting and raise a visible error. Test in Task 6 (`BusMaster` sees a foreign 0x0148 and goes to `Fault::second_master`) and Task 7 (the mock injects one).
3. **A corrupt or missing reply:** a bad CRC or a timeout must not advance the poll cursor, must not count as a lease reading, and lease renewal is retried at most once a second. Tests in Tasks 3, 4 and 6.
4. **The node count changes** (an optimizer added or lost): the renewal carries the latest 0x2F count. Test in Task 4.
5. **A daemon restart mid-lease:** after a restart, the first lease action is a check (0x2E), never a blind renewal. Test in Task 4.

---

### Task 1: Master frame codec (shared header)

**Files:**
- Create: `components/tigo_monitor/tigo_master.h`
- Test: `tests/host/master_protocol_test.cpp`
- Modify: `linux/scripts/build.sh` (add `master_protocol_test` to the host-test loop), `.github/workflows/pi.yml` (add `components/tigo_monitor/tigo_master.h` to both `paths:` lists)

**Interfaces:**
- Consumes: `protocol::crc16(const uint8_t*, size_t)` and `protocol::unescape` from `tigo_protocol.h`.
- Produces (namespace `esphome::tigo_monitor::master`, `Bytes = std::vector<uint8_t>`):
  - `Bytes encode_frame(uint16_t dest, uint16_t cmd, const Bytes &payload)`: the whole wire frame, preamble included.
  - `struct Frame { uint16_t dest; uint16_t cmd; Bytes payload; }`
  - `bool decode_frame(const std::string &escaped_body, Frame &out)`: takes the bytes between `7E 07` and `7E 08`, as `FrameSplitter` yields them. Returns false on a short frame or a bad CRC.
  - `Bytes tunnel_request(uint8_t sub, uint8_t seq, const Bytes &args)`: the 0x0B0F payload.
  - `struct TunnelReply { uint8_t sub; uint8_t seq; Bytes data; }`
  - `bool parse_tunnel_reply(const Bytes &payload, TunnelReply &out)`
  - `struct Lease { bool running; uint16_t remaining_s; uint16_t node_count; }`
  - `bool parse_lease(const TunnelReply &r, Lease &out)` (false unless sub 0x2F with at least 9 data bytes; node_count = count C)
  - `Bytes lease_renewal_args(uint16_t node_count)`: `BA BE 02 03 84 <nc hi> <nc lo> 01 00`
  - `Bytes poll_payload(uint16_t cursor)`: `00 01 <hi> <lo> 04`

- [ ] **Step 1: Write the failing tests.** Exact bytes from the roof capture:

```cpp
// 0x0148 poll, cursor 0x6FC4, as the CCA sent it.
CHECK(encode_frame(0x1201, 0x0148, poll_payload(0x6FC4)) == hex("00ffff7e07120101480001 6fc4 04 <crc lo> <crc hi> 7e08"));
// The CCA's renewal, seq 0x31, 44 nodes.
CHECK(tunnel_request(0x2D, 0x31, lease_renewal_args(44)) == hex("0000002d31babe020384002c0100"));
// Lease reply at 416 s, and lapsed.
Lease l; TunnelReply r;
CHECK(parse_tunnel_reply(hex("000e002ffd0101a0002c0028002c00"), r) && parse_lease(r, l) && l.running && l.remaining_s == 416 && l.node_count == 44);
CHECK(parse_tunnel_reply(hex("000e002f0000000000000028002c00"), r) && parse_lease(r, l) && !l.running && l.remaining_s == 0 && l.node_count == 44);
// Round trip, including a payload byte that needs escaping (0x7E, 0x24).
Frame f; CHECK(decode_frame(body_of(encode_frame(0x1201, 0x0B0F, {0x7E, 0x24, 0x00})), f) && f.cmd == 0x0B0F && f.payload == Bytes{0x7E, 0x24, 0x00});
// A flipped bit fails the CRC.
```

Fill in the 0x0148 frame's CRC by running the analysis repo's `tggw_crc16.build_tggw_packet(0x1201, 0x0148, bytes.fromhex("00016fc404"))`. The bytes in the capture are the authority; the test asserts the capture's frame. `hex()` and `body_of()` are test-file helpers.

- [ ] **Step 2: Run it and see it fail.** Run in the container: `g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor tests/host/master_protocol_test.cpp -o build/linux/master_protocol_test && build/linux/master_protocol_test`. Expected: compile error, `tigo_master.h` not found.
- [ ] **Step 3: Implement the interfaces above in `tigo_master.h`.** The escape table and CRC byte order come from Global Constraints. Reuse `protocol::crc16` and `protocol::unescape`; add an `escape()` beside them in this header.
- [ ] **Step 4: Run it and see it pass.** Expected: `all passed`. Also run `linux/scripts/build.sh` and check the other three host tests still pass.
- [ ] **Step 5: Commit** `feat(tigo_monitor): a codec for frames the bus master sends`.

### Task 2: Capture tool for master-side analysis

**Files:**
- Create: `linux/tools/bus_frames.py`

**Interfaces:**
- Produces: `bus_frames.py CAPTURE [--requests] [--sub HEX] [--pairs]`. It prints one line per good frame: byte offset, seconds (offset / (file size / duration from `--seconds`, default 1200)), dest, cmd, and payload hex. `--pairs` prints each request with the next reply whose cmd is request+1. It's standalone (its own CRC and unescape, no import from the analysis repo) so the container and CI can run it.

- [ ] **Step 1:** Write it, using `scratchpad/frames.py` from the planning session as the starting point. That file isn't in the repo: the logic is "split on 7E07…7E08, unescape, check CRC (init 0x8408, low byte first), print".
- [ ] **Step 2: Verify** `python3 linux/tools/bus_frames.py build/roof/steady-20261026.bin --requests | head` lists 0x0148 polls, and a frame count gives `203686 good frames, 0 bad`.
- [ ] **Step 3: Commit** `feat(linux): a tool that lists the frames in a bus capture`.

### Task 3: Poll cursor rule, derived from the capture

The CCA's 0x0148 cursor advances as 0x0149 data arrives (for example `6FC4 → 6FC8 → 6FCC → 6FD0 → 6FD4 → 6FD6`). This task finds the rule and pins it.

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h`
- Test: `tests/host/master_protocol_test.cpp`; `linux/scripts/cursor-check.py` (capture-driven, skipped when the capture is absent)

**Interfaces:**
- Produces: `class PollCursor { public: explicit PollCursor(uint16_t start); uint16_t next() const; void on_reply(const Frame &reply_0149); void on_timeout(); };`

- [ ] **Step 1: Derive the rule.** Use `bus_frames.py --pairs`, filtered to 0x0148/0x0149. Work out how the next request's cursor follows from the previous request's cursor and the 0x0149 reply between them. Reply bytes 1–3 (`ff c4`, `fe 04 c4`, `fe 02 d4`, …) look like a status plus the echoed cursor low byte. Write the rule as a short comment above `PollCursor`.
- [ ] **Step 2: Write the failing test.** `linux/scripts/cursor-check.py` replays every 0x0148→0x0149→0x0148 triple from the capture through a Python port of the rule, then prints `agree N of M`. Pass is **≥ 99.9 %** of 101,712 polls; list the disagreements. Add 6 consecutive hand-picked triples from the capture to `master_protocol_test.cpp` as C++ checks of `PollCursor`. Also check that a bad CRC (`decode_frame` false) and a timeout leave `next()` unchanged.
- [ ] **Step 3: Run both and see them fail.**
- [ ] **Step 4: Implement `PollCursor`.**
- [ ] **Step 5: Run both and see them pass.** Expected: `agree ≥ 101610 of 101712` and `all passed`.
- [ ] **Step 6: Commit** `feat(tigo_monitor): advance the telemetry poll cursor as the CCA does`, with the agreement figure in the body.

If no rule reaches 99.9 %, stop and report to the owner with the disagreement list. Task 6 can't poll correctly without it.

### Task 4: Lease keeper

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h`
- Test: `tests/host/master_protocol_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LeaseAction { enum Kind { none, check, renew } kind; uint16_t node_count; };
  class LeaseKeeper {
   public:
    LeaseAction due(uint32_t now_ms);       // what to send now, if anything
    void on_lease(const Lease &l, uint32_t now_ms);
    void on_no_reply(uint32_t now_ms);
    const Lease &last() const; uint32_t renewals() const; bool ever_read() const;
  };
  ```
  Rules: `check` every 20,000 ms. The first action after construction is always `check`. `renew` only when the last reading had `remaining_s == 0` or `!running`, at most once per 1,000 ms, until a reading shows `remaining_s > 0`. `node_count` comes from the last reading.

- [ ] **Step 1: Write the failing tests.** Drive time by hand:
  - new keeper, `due(0)` is `check`, never `renew` (restart mid-lease);
  - after `on_lease({true, 416, 44}, 0)`, `due(19999)` is `none` and `due(20000)` is `check`;
  - after `on_lease({false, 0, 44}, t)`, `due(t)` is `renew` with node_count 44, then `due(t+500)` is `none`, then `due(t+1000)` is `renew`;
  - after `on_lease({true, 900, 44}, …)`, there are no further renews;
  - a reading with node_count 45 makes the next renewal carry 45;
  - `on_no_reply` changes nothing about renewals: no reading, no renew.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement `LeaseKeeper`.**
- [ ] **Step 4: Run them and see them pass.**
- [ ] **Step 5: Commit** `feat(tigo_monitor): keep the TAP's run lease as the CCA does`.

### Task 5: Request allow-list

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h`
- Test: `tests/host/master_protocol_test.cpp`

**Interfaces:**
- Produces: `enum class Allow { yes, bench_only, never }; Allow classify_request(uint16_t cmd, const Bytes &payload);` and `bool may_send(uint16_t cmd, const Bytes &payload, bool bench)`, which is true for `yes`, true for `bench_only` only when `bench` is set, and false for `never`.
- Classification, from Global Constraints:
  - **`yes`:** 0x0148 with a 5-byte payload; 0x0B0F subs 0x2E, 0x26, 0x06 with `^00`, and 0x2D only in the exact `lease_renewal_args` shape.
  - **`never`:** the listed write CMDs, or any payload containing `37 24 92 66`. `may_send` returns false for these whatever `bench` is.
  - **`bench_only`:** the read CMDs 0x000A, 0x000E, 0x0038, 0x0056, 0x0308, 0x001C, 0x0118, 0x0142, 0x0042, 0x0300; tunnel subs 0x41 and 0x22; and anything else.

- [ ] **Step 1: Write the failing tests,** one per class, including:
  - a 0x2D with node count 44 but `03 85` instead of `03 84` → `bench_only`;
  - a 0x0B0F sub 0x06 carrying `^00Set…` → `bench_only`;
  - a 0x0148 with a 6-byte payload → `bench_only`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
- [ ] **Step 5: Commit** `feat(tigo_monitor): refuse bus requests a roof master must not send`.

### Task 6: BusMaster in the daemon

**Files:**
- Create: `linux/src/bus_master.h`, `linux/src/bus_master.cpp`
- Modify: `linux/src/serial_port.h/.cpp` (an `open_port_rw(const std::string&, int baud)` beside the read-only `open_port`; listen mode keeps `O_RDONLY`), `linux/CMakeLists.txt`
- Test: `tests/host/` can't open ports, so this task's tests are in Task 7's e2e. The pure scheduling decision gets a host test here.

**Interfaces:**
- Consumes: `encode_frame`, `decode_frame`, `PollCursor`, `LeaseKeeper`, `may_send` (Tasks 1, 3–5); `Monitor::feed(const char*, size_t)`; `FrameSplitter`.
- Produces:
  ```cpp
  enum class Fault { none, no_port, no_reply, second_master, refused };
  struct MasterStatus { bool active; Fault fault; Lease lease; uint32_t renewals; uint32_t polls;
                        uint32_t timeouts; int64_t last_reply_unix; std::string detail; };
  class BusMaster {
   public:
    BusMaster(Monitor &mon, uint16_t gateway_id, bool bench);
    bool open(const std::string &device, int baud);  // O_RDWR
    void tick(uint32_t now_ms);   // main loop, every ~10 ms: one transaction at most
    void close();
    MasterStatus status() const;  // thread-safe copy
  };
  ```
- Behaviour:
  - **A transaction** writes one frame, then reads until a `7E 08` ends a frame whose DEST is `gateway_id|0x8000`, or 50 ms pass. Every byte read is passed to `Monitor::feed`, so 0x0149 decoding, the node table and history work unchanged.
  - **Order of work in `tick`:** a due lease action first, then otherwise a 0x0148 poll with `PollCursor::next()`.
  - **Every outgoing frame** goes through `may_send`. A refusal sets `Fault::refused` and logs the cmd; nothing is sent.
  - **A second master:** a decoded request frame (DEST without 0x8000) that isn't an exact echo of the frame we just wrote (an adapter may hear its own transmission) sets `Fault::second_master`, stops all transmitting until the daemon restarts or the role is changed, and logs at error.
  - **A timeout** counts in `timeouts`. Ten consecutive timeouts set `Fault::no_reply`, but polling continues.
  - **The main loop must drive `tick`** (Task 8 moves its 1 s sleep to 10 ms in master mode). There is no thread of its own.

- [ ] **Step 1: Host-test the scheduling choice.** Factor it as `enum class Next { lease_check, lease_renew, poll }; Next choose(const LeaseKeeper&, uint32_t now_ms)` in `tigo_master.h`. Tests: a due check beats a poll; a renew beats a poll; otherwise poll.
- [ ] **Step 2: Run it and see it fail; implement; run it and see it pass.**
- [ ] **Step 3: Implement `BusMaster` and `open_port_rw`.** It builds with no new warnings under `linux/scripts/build.sh`.
- [ ] **Step 4: Commit** `feat(linux): a bus master that polls the TAP and keeps its lease`.

### Task 7: Mock roof TAP and master e2e

**Files:**
- Create: `linux/tools/mock_tap.py`, `linux/scripts/master-e2e.sh`
- Modify: `.github/workflows/pi.yml` (a step running `master-e2e.sh`)

**Interfaces:**
- Produces:
  - **`mock_tap.py PTY_LINK [--lease S] [--nodes N] [--foreign-after S] [--stats FILE]`.** It opens a pty pair (Python `pty.openpty`), symlinks the slave to PTY_LINK, and answers frames addressed to 0x1201:
    - 0x0148 → a synthetic 0x0149 built with `linux/tools/synth_bus.py`'s packet code, honouring the cursor;
    - 0x2E → a lease reply counting down in real time from `--lease`;
    - 0x2D → resets the lease to 900 s, recording the node count it was given;
    - anything else → logged in `--stats`.
    - `--foreign-after S` injects a request frame of its own after S seconds.
    - On exit it writes stats JSON: polls, checks, renewals with their node counts, refused/unknown frames, and time of the first frame.
  - **`master-e2e.sh`.** It starts `mock_tap.py --lease 5`, then the daemon with `bus_role: master` on the pty, and checks:
    - panels appear in `/api/devices`;
    - the first lease frame is a check, not a renewal;
    - a renewal with node count `--nodes` arrives within 2 s of the lease reading 0;
    - `/api/master` reports `active: true`, the lease and renewals ≥ 1;
    - no frame outside the allow-list reached the mock;
    - after `kill -STOP` on the daemon for 8 s, the mock records **no renewal** during the stop, and the lease reads 0 (Review Focus 1);
    - a run with `--foreign-after 3` ends with `/api/master` fault `second_master` and no frames sent after it (Review Focus 2).

- [ ] **Step 1: Write the mock and the script.** The script fails because the daemon has no master role yet (Task 8).
- [ ] **Step 2: Run it after Task 8.** Expected: `master e2e passed`.
- [ ] **Step 3: Commit** `test(linux): a mock roof TAP for the bus master`.

### Task 8: Settings, API and UI for the master role

**Files:**
- Modify:
  - `linux/src/settings.h/.cpp`: `std::string bus_role = "listen"` (`"listen"` | `"master"`), `bool master_bench = false`, `int gateway_id = 0x1201`;
  - `linux/src/main.cpp`: build `BusMaster` instead of `run_serial` when the role is master; tick every 10 ms in master mode; restart input on a role change;
  - `linux/src/http_server.cpp`: `GET /api/master` → `MasterStatus` as JSON; `/api/settings` accepts `bus_role`, `master_bench` and `gateway_id`, validated (role in the two values, gateway_id 1–0x7FFF);
  - `components/tigo_server/web/app.html`: Tools card "RS485 adapter and web UI" gets a "Bus role" select. Choosing "Master" shows a confirm: *"Master mode transmits on the bus. Unplug the CCA first. Two masters on one bus collide. Continue?"* Diagnostics shows lease remaining, renewals, polls/s, timeouts and the fault;
  - `site/screenshots/fixtures.mjs`: not needed. `/api/master` and `/api/settings` are Pi-only and not in fixtures, per the project rule.
- Test: `master-e2e.sh` (Task 7), plus a `cca-e2e.sh` check that `bus_role: "both"` gets 400.

- [ ] **Step 1: Write the settings validation test** in `cca-e2e.sh`.
- [ ] **Step 2: Run it and see it fail; implement; run `cca-e2e.sh`, `e2e.sh`, `bridge-e2e.sh` and `master-e2e.sh`; all pass.**
- [ ] **Step 3: Drive the Tools role select in headless Chromium,** as done for the bridge picker: the confirm appears, Cancel leaves the role at listen, OK saves master.
- [ ] **Step 4: Commit** `feat(linux): a master bus role, off by default`.

### Task 9: Bench session with the spare TAP (manual, owner present)

**Files:**
- Create: `docs/cca-replacement-bench.md` (an engineering note: setup, commands, results with numbers)

- [ ] **Step 1: Hardware.** Spare TAP on the Pi's Waveshare HAT: A/B plus a **shared ground**, with the TAP powered from a bench supply. Measure the roof TAP's supply on the CCA cable first (spec open question 3) and match it. No CCA on this bus.
- [ ] **Step 2: Reads, in bench mode.** `master_bench: true`, role master, `gateway_id` set to the spare TAP's id (0x1235 per the analysis repo), run with `--capture`. Bench mode asks VERSION first and logs it, the first 20 poll reply headers are logged, and any NAK is logged once per command. Then:
  - **VERSION 0x000A:** a reply proves TX works;
  - **0x0148 polls:** a gateway with no mesh refuses them with 0x0006 (the analysis repo's spare did, at any cursor), and Diagnostics should say so. How the TAP treats our starting cursor of 0 can only be seen on the roof;
  - **0x2E:** a gateway with no mesh refuses the tunnel with 0x0006, as the analysis repo's standalone G5 unit did. So the lease can't be tested on the bench; it waits for the roof.
- [ ] **Step 3: Record results** in the note: reply or not, bytes, timing. **Stop here if VERSION gets no reply.** Work through openTAPtoX's checklist (spec, "Working TX hardware") before going further.
- [ ] **Step 4: Commit** the note.

### Task 10: Start-up capture and roof takeover runbook

**Files:**
- Create: `docs/cca-replacement-roof.md` (runbook and results)

- [ ] **Step 1: Capture a CCA cold start** (owner present): service stopped, start `cat /dev/serial0` as in the steady capture, plug the CCA in, record until the dashboard shows production, about 10 minutes. Run `bus_frames.py --requests` and list every request type that isn't in the steady-state table.
- [ ] **Step 2: Gate.** **First, the start-up question.** The analysis repo found that a newer gateway (H1.0006, GW-H158, the roof's model line) doesn't start beaconing from running mode alone, unlike the older G5.26. Netrun start is a beacon-clock-timed broadcast in lmudcd, not a replayable argument. So check whether the cold start contains a netrun start (0x0058) or anything else the master doesn't send before the first 0x0148 gets data. If it does, the master as built can't bring the array up after a power cut, and phase 3 waits for a design decision with the owner. The steady-state CCA also sends tunnel sub 0x41 (a telemetry poll spec, bench-only here). If Task 9 or a dry run shows panels going quiet without it, it moves to `yes` with its captured shape; that's the owner's decision. If the cold start sends requests beyond the allow-list (likely setup commands), list them in the runbook with what the analysis repo says they are. The owner decides whether phase 3 needs them, since the TAP stores its setup across power cuts. **Don't add them to the allow-list without that decision.**
- [ ] **Step 3: Write the runbook.**
  - **Before:** daylight, production steady, the Pi on the master build, the TAP's power supply ready, the CCA reachable to plug back in.
  - **Takeover:** unplug the CCA, power the TAP from the Pi side, set the role to master.
  - **Pass:**
    1. production returns within the time the CCA's cold start took in Step 1;
    2. panels update on the dashboard;
    3. at least one lease renewal shows in `/api/master`;
    4. **with the Pi's power pulled, the array drops to safe mode.** Time how long the drop takes; this is open question 1.
  - **Rollback:** Pi off, CCA back in, confirm production.
- [ ] **Step 4: Run it with the owner.** Record the numbers in the runbook. Commit as `docs: roof takeover results`.

Phase 4 (commissioning without a CCA) is a separate plan once Task 10 passes.
