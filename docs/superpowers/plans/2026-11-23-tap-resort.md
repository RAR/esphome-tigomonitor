# The Re-sort (Optimizers Choose Their TAP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A re-sort, started by the owner on the Pi, that changes every TAP's network the way the CCA's 11-15 rediscovery did. Every optimizer then joins the TAP it hears, each TAP is trimmed to its own, and the old networks can be put back.

**Architecture:**
- **The job.** A multi-TAP job (`ResortJob`, new header `components/tigo_monitor/resort.h`) runs the CCA's frame order. MasterCore hands it each TAP's turn and keeps the waiting and the trimming after it as rules it applies every tick, from a file the daemon keeps. No timer is the source of truth, so a restart carries on.
- **The windows.** They end `01 01` whenever more than one TAP is run with 0xE1 on.
- **The page.** app.html gets a Re-sort row, a waiting panel, a last step for Add a TAP, and a real Replace a TAP flow.

**Tech Stack:** C++17 shared headers + MasterCore (host tests with g++), the Linux daemon (cpp-httplib, nlohmann::json), Python mocks, sh e2e scripts, vanilla JS in app.html with node site tests.

**Spec:** `docs/superpowers/specs/2026-11-23-tap-resort-design.md` (commit d5f65eb on `feature/tap-resort`). Read it first; it carries the CCA's frame order and the owner's decisions.

## Global Constraints

- **Never output a key or PAN.** That covers log lines, API answers, the page, fixtures, commits, tests' printed output and chat. Keys and PANs appear only in the daemon's memory, on the TAPs, and in the old-networks file.
- **The old-networks file:**
  - in the state directory, mode 0600, written with `write_file_atomic` (temp + fsync + rename);
  - written before the first network frame;
  - deleted when the re-sort ends;
  - never in a backup, export or log.
- **Never send a frame the CCA didn't send.** The re-sort's shapes are:
  - `0B00 00` / `0B00 01`;
  - `0D 01 00` + a 36- or 46-byte block;
  - `0D 00 01` and `0D 00 00`;
  - Store `0012 37 24 92 66` (G8 only);
  - 0xE1 in PR 1's shape;
  - `2B BA BE`;
  - 0x29 with 1–10 records;
  - `2D BA BE 02 03 84 <count> 01 01`.
- **`0B00 00` is sent only inside the re-sort's bracket.** Every PV off the re-sort sends is followed by PV on to every TAP, whatever fails in between.
- **The re-sort runs behind Allow changes to the TAPs** (`master_table_writes`), and on the Pi only. MasterCore's known-networks switch (`set_known_networks`) is off on the ESP32.
- **A re-sort never starts by itself;** only `request_resort` starts one.
- **UI copy says "TigoMonitor"** and makes no CCA comparisons. Run `cd site && npm test` before the PR.
- **`/api/*` shape changes update `site/screenshots/fixtures.mjs`** in the same commit.
- **Every new host test is added to `linux/scripts/build.sh`.** Every new e2e script goes in a shard in `.github/workflows/pi.yml`.
- **Commits** use `git -c user.email=andrew@eiknet.com commit`, with no trailers. The PR targets `main`.

## Review Focus

1. **A TAP stops answering after PV off,** in the middle of the writes. Expected: PV on still goes to every TAP, the job fails naming the TAP, the file stays, and the put-back is offered. Pinned in Task 3 (`test_resort_fails_after_pv_off_still_sends_pv_on`) and Task 4 (`test_resort_tap_lost_mid_write`).
2. **TigoMonitor restarts between PV off and PV on.** Expected: the next start finds the file in phase `writing`, sends `0B00 01` to every TAP, and goes on waiting. Pinned in Task 4 (`test_resort_resume_after_cut_writes`).
3. **Automatic discovery wants the window closed during the wait,** because every optimizer reported today. Expected: the re-sort renews every TAP's window anyway. The owner's "let it close" refuses a start. Pinned in Task 4 (`test_resort_windows_renew_despite_auto_discovery`).
4. **Two TAPs list one id under different MACs,** or one MAC at two ids, before the start. Expected: refused, naming both. Pinned in Task 2 (`test_resort_list_conflicts`).
5. **A key or PAN leaks** into a dry run's text, a log line, `/api/master` or the e2e journal. Expected: none appears. Pinned in Task 3 (`test_resort_dry_run_has_no_key`), Task 4 (`no_key_in(lines)` in every resort test) and Task 7 (a grep of the log, the API answers and the stats).

---

### Task 1: Windows end `01 01` with more than one TAP

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h`:
  - `discovery_renewal_args` (~line 234);
  - the `SUB_DISCOVERY_START` case in `classify_request` (~line 1961);
  - `WindowKeeper` (~line 261);
  - `next_request` (~line 1233).
- Modify: `components/tigo_master/master_core.cpp`, in the tick, where `t.window.set_renewing(...)` is set (~line 1480).
- Modify: `linux/tools/mock_taps.py`, in the 0x2D handler: record the args.
- Modify: `linux/scripts/shared-network-e2e.sh`: add an assertion.
- Test: `tests/host/master_protocol_test.cpp`, `tests/host/master_core_test.cpp`

**Interfaces:**
- Produces:
  - `inline Bytes discovery_renewal_args(uint16_t node_count, bool known_networks = false)`: the last byte is `0x01` when known_networks is set.
  - `WindowKeeper::set_known_networks(bool on)` and `bool known_networks() const`: the renewal it asks for carries the flag. `WindowAction` gains `bool known_networks = false`, filled by `due()`.
  - The gate allows `… 01 00` and `… 01 01` as `Allow::yes`. Anything else in the last byte stays `bench_only`.
  - MasterCore sets each TAP's keeper flag every tick to `known_networks_ && taps_.size() > 1`.

- [ ] **Step 1: Write the failing protocol tests** in `master_protocol_test.cpp`:

```cpp
static void test_renewal_with_known_networks() {
  CHECK(mp::discovery_renewal_args(0x2C) == (mp::Bytes{0xBA, 0xBE, 0x02, 0x03, 0x84, 0x00, 0x2C, 0x01, 0x00}));
  CHECK(mp::discovery_renewal_args(0x2C, true) == (mp::Bytes{0xBA, 0xBE, 0x02, 0x03, 0x84, 0x00, 0x2C, 0x01, 0x01}));
  auto req = [](const mp::Bytes &args) { return mp::tunnel_request(0x2D, 7, args); };
  CHECK(mp::classify_request(mp::CMD_TUNNEL, req(mp::discovery_renewal_args(20, true))) == mp::Allow::yes);
  mp::Bytes two = mp::discovery_renewal_args(20, true);
  two[8] = 0x02;
  CHECK(mp::classify_request(mp::CMD_TUNNEL, req(two)) == mp::Allow::bench_only);
  CHECK(mp::classify_request(mp::CMD_TUNNEL, req(mp::discovery_renewal_args(0, true))) == mp::Allow::bench_only);
}
static void test_keeper_renews_with_the_flag() {
  mp::WindowKeeper k;
  k.set_known_networks(true);
  mp::DiscoveryWindow w;  // read: 0 s left, 20 listed
  w.remaining_s = 0;
  w.node_count = 20;
  k.on_window(w, 0);
  mp::WindowAction a = k.due(1);
  CHECK(a.kind == mp::WindowAction::renew && a.known_networks);
}
```

Register both in `main()`.

- [ ] **Step 2: Run and watch them fail.**
  - Run: `docker run --rm -v "$PWD":/src -w /src tigod-dev linux/scripts/build.sh 2>&1 | tail -20`
  - Expected: compile error: `discovery_renewal_args` takes 1 argument / no member `set_known_networks`.

- [ ] **Step 3: Implement** the default argument, the keeper's flag (copied into `WindowAction` by `due()`) and `next_request` passing `a.known_networks`. In the gate, take `args[8] <= 0x01`.

- [ ] **Step 4: Write the failing master tests** in `master_core_test.cpp`. First extend `MultiTap::One` with `std::vector<mp::Bytes> renewals;`, filled from every 0x2D request's args, if it doesn't record them already.
  - `test_two_taps_renew_with_known_networks`:
    - setup: two TAPs, `set_known_networks(true)`, both windows reading 0 s left with a count;
    - after `run_for`, every recorded renewal on both ends `0x01, 0x01`.
  - `test_one_tap_renews_without_the_flag`:
    - one TAP, `set_known_networks(true)`: renewals end `0x01, 0x00`;
    - two TAPs with `set_known_networks` never called (the ESP32): `0x01, 0x00`.

- [ ] **Step 5: Run and watch them fail** (same build command). Expected: FAIL on the last byte (`0x00`, want `0x01`).

- [ ] **Step 6: Implement.** In the tick, beside `set_renewing`: `t.window.set_known_networks(known_networks_ && several_());`.

- [ ] **Step 7: Make it pass, then the e2e.**
  - In `mock_taps.py`'s 0x2D branch, add `t.st.setdefault('renewal_flags', []).append(p[12:14].hex())`.
  - In `shared-network-e2e.sh`, after the 0xE1 checks: `expect "renewals carry the known-networks flag" "$(stat ... "set(sum((t.get('renewal_flags', []) for t in d['taps']), [])) == {'0101'}")" True`. Adapt the expression to the stats file's shape: a list of TAPs, or a dict.
  - Run: the build command, then `docker run --rm -v "$PWD":/src -w /src tigod-dev linux/scripts/shared-network-e2e.sh`
  - Expected: `master_core_test` passes; `shared-network e2e passed`.

- [ ] **Step 8: Commit**

```bash
git add components/tigo_monitor/tigo_master.h components/tigo_master/master_core.cpp tests/host/master_protocol_test.cpp tests/host/master_core_test.cpp linux/tools/mock_taps.py linux/scripts/shared-network-e2e.sh
git -c user.email=andrew@eiknet.com commit -m "feat(pi): windows carry the known-networks flag with more than one TAP"
```

The body says: the CCA's 119 renewals per TAP from 11-14 to 11-16 all ended `01 01`; one TAP and the ESP32 keep `01 00`; and the test counts.

---

### Task 2: The re-sort's plan: new networks, the list, the trim

**Files:**
- Create: `components/tigo_monitor/resort.h`
- Create: `tests/host/resort_test.cpp`
- Modify: `linux/scripts/build.sh` (add `resort_test` to the loop at line 7)

**Interfaces:**
- Consumes:
  - `RadioBlock`, `pan_for`, `key_from`, `CHANNEL_MIN`/`MAX` (tap_config.h);
  - `TableRecord`, `TableWalk::find`, `mac_hex` (table_edit.h).
- Produces (namespace `esphome::tigo_monitor::master`):
  - `struct ResortTap { uint16_t tap; RadioBlock now; bool saves_with_store; };`: one TAP as it runs before the re-sort.
  - `template <typename Draw> std::vector<RadioBlock> resort_networks(const std::vector<ResortTap> &taps, Draw draw)`. One new block per TAP, in the same order. Each block:
    - keeps its TAP's whole `now` block (channel, map, SLT, SLM, optimization, wide, extra), except PAN and key;
    - gets the PAN `pan_for({draw(), draw()}, in_use)`, where `in_use` starts as every TAP's current PAN and grows with each PAN picked;
    - shares one key, `key_from(draw)`, used for every TAP.
    - A PAN of 0 (none free) gives an empty vector.
  - `bool resort_list(const std::map<uint16_t, std::vector<TableRecord>> &tables, std::vector<TableRecord> &out, std::string &why)`. Every MAC on any table, once each, with tuid `0x8000 | id`, sorted by id. False with `why` when:
    - two TAPs list one id under different MACs: `"TAP 1201 lists 0010 as 04C05B…01 and TAP 1202 as 04C05B…02"`, full MACs in hex, which is allowed (MACs are not secrets);
    - one MAC is listed at two ids;
    - the list is empty.
  - `std::map<uint16_t, std::vector<Bytes>> resort_trim(const std::map<uint16_t, std::vector<TableRecord>> &tables)`. For each TAP, the MACs it lists that some other TAP lists as joined. A MAC joined on two TAPs is trimmed from neither, and the log will say so (Task 4).
  - `std::vector<TableRecord> resort_missing(const std::map<uint16_t, std::vector<TableRecord>> &tables, const std::vector<Bytes> &waited)`. The waited MACs joined on no TAP, with the id and tuid from the first table that lists them.

- [ ] **Step 1: Write the failing tests** in `tests/host/resort_test.cpp`. Use the same `CHECK` macro and `main()` shape as `table_edit_test.cpp`, and a counter-based `draw` (`[n = 0]() mutable { return uint8_t(0x11 + n++); }`).
  - `test_resort_networks_keep_all_but_pan_and_key`:
    - 3 TAPs on channels 15/18/20, with distinct made-up PANs and one made-up key;
    - each result keeps its channel, map, SLT, SLM and wide;
    - PANs are pairwise different and differ from all three old PANs;
    - all three keys are equal to each other and differ from the old key;
    - no key byte is 0x00 or 0xFF.
  - `test_resort_networks_no_free_pan`: a draw that always gives the same seed, with all 16 steps of `pan_for` in use, gives an empty vector.
  - `test_resort_list_merges_and_marks_listed`:
    - TAP A lists ids 0x10 and 0x11 joined, TAP B lists 0x12 joined, and 0x11 listed again (same MAC) not joined;
    - out has 3 records, sorted, every tuid with 0x8000.
  - `test_resort_list_conflicts`:
    - same id, different MACs: false, and `why` names both TAPs;
    - same MAC, two ids: false;
    - empty tables: false.
  - `test_resort_trim`:
    - A lists m1 joined and m2 not joined; B lists m1 not joined and m2 joined; C lists m1 and m2 not joined;
    - trim: A gets {m2}, B gets {m1}, C gets {m1, m2};
    - m3 joined on both A and B is trimmed from neither.
  - `test_resort_missing`: waited {m1, m2, m3}, with m1 joined on A and m2, m3 listed nowhere joined, gives {m2, m3} in that order.

- [ ] **Step 2: Add `resort_test` to build.sh and run it.**
  - Run: the build command.
  - Expected: compile error: `resort.h` not found.

- [ ] **Step 3: Implement `resort.h`** with the four functions. The header comment cites the spec and the CCA's 11-15 order (labels only, no binary names).

- [ ] **Step 4: Run.** Expected: `resort_test` passes, along with every other test.

- [ ] **Step 5: Commit**

```bash
git add components/tigo_monitor/resort.h tests/host/resort_test.cpp linux/scripts/build.sh
git -c user.email=andrew@eiknet.com commit -m "feat(pi): the re-sort's new networks, its list of every optimizer and its trim"
```

---

### Task 3: `ResortJob`, the CCA's order across every TAP

**Files:**
- Modify: `components/tigo_monitor/resort.h`
- Modify: `components/tigo_monitor/tigo_master.h`: add `bool resort = false;` to `struct Request` (~line 433), with a comment: "from a ResortJob (resort.h): its PV off is allowed inside its bracket".
- Test: `tests/host/resort_test.cpp`

**Interfaces:**
- Consumes:
  - Task 2's `ResortTap`, `resort_list`;
  - `TableWalk`, `tunnel_request`, `next_seq`, `radio_write_args`, `RadioBlock::decode`, `parse_tunnel_reply`;
  - `known_networks_args`, `KnownNetwork`, `known_networks_status`;
  - `discovery_renewal_args(count, true)`;
  - `CMD_SESSION`, `CMD_STORE`, `SUB_GATEWAY_CFG(_REPLY)`, `SUB_ERASE_FLASH`, `SUB_DISCOVERY_START`, `SET_TABLE_MAX_RECORDS`.
- Produces:

```cpp
class ResortJob {
 public:
  enum class Step { read_tables, save, pv_off, write, pv_on, store, save_read, check, known, erase, list, window, done, failed };
  // taps: every TAP the master runs, in polling order. target: the block each
  // is to get (resort_networks', or the file's old ones for a put-back), same order.
  ResortJob(std::vector<ResortTap> taps, std::vector<RadioBlock> target, bool dry_run, bool put_back);
  Request due(uint16_t tap, uint32_t now_ms, uint8_t &seq);  // {} if nothing is due on this TAP now
  void on_reply(uint16_t tap, const Request &req, bool ok, const Frame &reply, uint32_t now_ms);
  void abort(const std::string &why);  // the gate refused a frame: PV on still goes out, then failed
  bool wants_save() const;             // at Step::save: the caller writes the file, then saved()
  void saved(bool ok);
  bool finished() const;   // done or failed
  bool ok() const;
  bool dry_run() const;
  bool put_back() const;
  bool pv_off_sent() const;  // between the first PV off and the last PV on: the caller's file phase is "writing"
  Step step() const;
  const char *step_text() const;  // "reading the tables", "saving the old networks", "PV off", "writing the networks", ...
  const std::string &message() const;  // never a key or PAN
  const std::vector<TableRecord> &records() const;  // the list, after read_tables
  const std::vector<std::string> &dry_text() const; // a dry run's frames, by label only
  uint16_t failed_tap() const;  // 0, or the TAP a step failed on
};
```

- **The steps, in the spec's order.** Each step runs on every TAP in `taps` order before the next step starts. `due()` returns a request only for the TAP whose turn it is in the current step, so the master keeps polling the others in between. The bodies:
  1. **`read_tables`:** a `TableWalk` per TAP, then `resort_list`. It fails with that `why`.
  2. **`save`:** skipped for a put-back. A dry run stops after `read_tables` and fills `dry_text`.
  3. **`pv_off`:** `0B00 00`, with `resort = true`, to each TAP. The reply `0B01` is not required.
  4. **`write`:** `0D 01 00` + `target[i]`, with `provision = true`, `table_edit = true` and `WRITE_WAIT_MS` = 3000. The echo must equal the target with status 0 or 1, as ProvisionJob checks it.
  5. **`pv_on`:** `0B00 01` to each TAP. This step runs whenever `pv_off_sent()` is true, including after a failure in `write` (see `abort`/fail).
  6. **`store`:** only for `saves_with_store` TAPs. Store `0012 37 24 92 66`, with `provision = true` and `table_edit = true`; the reply must be `0x0013`.
  7. **`save_read`:** `0D 00 01` to each TAP. The TAP has no `00 01` read? Then it's done (ProvisionJob's rule).
  8. **`check`:** `0D 00 00` to each TAP. The decoded block must equal `target[i]`.
  9. **`known`:** 0xE1 to each TAP: `known_networks_args` of every other TAP's target (channel, PAN, key), in `taps` order. Status must be 0. More than 16 others: refused at construction.
  10. **`erase`:** `2B BA BE` to each TAP; the ack is 0.
  11. **`list`:** `records()` in chunks of `SET_TABLE_MAX_RECORDS` (10), each 0x29 with `table_edit = true`; each ack is 0.
  12. **`window`:** `2D` with `discovery_renewal_args(records().size(), true)` to each TAP.
  13. **`done`**.
- **When a step fails after `pv_off_sent()`,** the job goes to `pv_on` for every TAP before `failed`. Its message names the TAP and the step ("TAP 1202 didn't echo its network write"). `failed_tap()` returns it.

- [ ] **Step 1: Write the failing tests.** Add a small fake bus to `resort_test.cpp`: a `FakeTaps` struct, map tap id → { radio block, table records, `saves_with_store`, `silent_from` step }. It answers each `Request` the way `MultiTap` does in master_core_test (copy that file's 0x0D, 0x26/0x27, 0x29, 0x2B, 0x2D, 0xE1, 0x0B00, 0x0012 replies). Add a `drive(job, fake)` loop: round-robin over the TAP ids, `due()` then `on_reply()`, recording `(tap, label)` for each frame. Labels are `"pvoff"`, `"write"`, `"pvon"`, `"store"`, `"saveread"`, `"check"`, `"e1"`, `"erase"`, `"list"` and `"window"`. The tests:
  - `test_resort_order_three_taps`:
    - the frame list filtered to labels equals `read…` ×3 tables, then `pvoff`×3, `write`×3, `pvon`×3, `store` (G8 only), `saveread`×3, `check`×3, `e1`×3, `erase`×3, `list` (40 records = 4 per TAP ×3), and `window`×3;
    - each TAP's final table has 40 records, all with 0x8000;
    - each window's args end `01 01`, with count 40.
  - `test_resort_e1_names_the_others_new_networks`:
    - the 0xE1 to TAP A carries B's and C's target PAN and the shared target key, in `taps` order (the test compares bytes; it never prints them);
    - count 2.
  - `test_resort_save_comes_before_the_first_network_frame`:
    - `wants_save()` is true after `read_tables`, and `due()` returns `{}` on every TAP until `saved(true)`;
    - `saved(false)`: failed, no `pvoff` sent, message "couldn't save the old networks; nothing was changed".
  - `test_resort_fails_after_pv_off_still_sends_pv_on`:
    - TAP B is silent from `write`;
    - labels after the failure contain `pvon` for A, B and C;
    - `failed`, `failed_tap() == B`, and message names "TAP 1202".
  - `test_resort_abort_after_pv_off_still_sends_pv_on`: `abort("refused")` during `write` gives `pvon` ×3, then failed.
  - `test_resort_dry_run_has_no_key`:
    - the dry run reads the tables and stops: no `pvoff`, `write` or `erase`;
    - `dry_text` lists `"PV off to 3 TAPs"`, `"0D 01 00 + block (channel 15, a new PAN, the new shared key) to TAP 1201"` …, `"0012 (the Store) to TAP 1202"`, `"0xE1 to each TAP, naming the 2 others"`, `"2B erase, then 40 listed (not joined) on each TAP"` and `"2D window ending 01 01 on each TAP"`;
    - no hex of any target PAN or key byte run (4 or more hex chars from the key) appears in any `dry_text` line or `message()`.
  - `test_resort_put_back_skips_the_save`: `put_back = true` never sets `wants_save()`, and the frame order is otherwise the same.
  - `test_resort_list_conflict_stops_before_anything`: the `resort_list` refusal fails at `read_tables` with no `pvoff`.

- [ ] **Step 2: Run.** Expected: compile errors (no `ResortJob`).

- [ ] **Step 3: Implement `ResortJob`** in `resort.h`, plus `Request::resort` in `tigo_master.h`. Reuse ProvisionJob's echo and status rules: status 0 or 1, echo equal to what was sent. Write the form the TAP read back in (`wide`/`extra` from `now`).

- [ ] **Step 4: Run.** Expected: every `resort_test` case passes; the full build passes.

- [ ] **Step 5: Commit**

```bash
git add components/tigo_monitor/resort.h components/tigo_monitor/tigo_master.h tests/host/resort_test.cpp
git -c user.email=andrew@eiknet.com commit -m "feat(pi): ResortJob, the CCA's re-sort order across every TAP"
```

---

### Task 4: MasterCore runs the re-sort, the wait, the trim and the put-back

**Files:**
- Modify: `components/tigo_master/master_core.h`, `components/tigo_master/master_core.cpp`
- Test: `tests/host/master_core_test.cpp`

**Interfaces:**
- Consumes: Task 3's `ResortJob`; Task 2's `resort_networks`, `resort_trim`, `resort_missing`; `TableRemove(std::vector<Bytes>, bool)`; `TableWalk`.
- Produces (in `master_core.h`):

```cpp
// The re-sort's state, as the daemon keeps it on disk (the old networks
// hold keys: the file is the only place outside memory and the TAPs).
struct ResortFile {
  std::string phase;  // "writing" (PV off may be unanswered by a PV on), "waiting"
  int64_t started_unix = 0;
  std::vector<esphome::tigo_monitor::master::Bytes> waited;  // every MAC listed by the re-sort
  // Each TAP's network before the re-sort, by TAP id; empty after a put-back's writes.
  std::vector<std::pair<uint16_t, esphome::tigo_monitor::master::RadioBlock>> old_networks;
};
struct ResortStatus {
  std::string state = "idle";  // idle, running, waiting, putting_back
  bool dry_run = false, finished = false, ok = false;
  std::string step, message;
  std::vector<std::string> dry_frames;
  int64_t started_unix = 0;
  std::vector<std::pair<uint16_t, int>> joined;  // per TAP, in polling order
  std::vector<TableWaiting> missing;             // waited for, joined nowhere
  bool can_put_back = false;                     // old networks on file
};
// in MasterStatus:  ResortStatus resort;
// MasterCore:
using ResortSaved = std::function<bool(const ResortFile *file)>;  // null: delete
void set_resort_saved(ResortSaved saved, const ResortFile *resume = nullptr);  // before open()
template <typename Draw> void set_resort_draw(Draw d);  // the randomness; std::function<uint8_t()>
bool request_resort(bool dry_run, std::string &why);
bool request_resort_put_back(bool dry_run, std::string &why);
bool request_resort_stop(std::string &why);
static constexpr uint32_t RESORT_READ_MS = 30000;  // each TAP's table, while waiting
```

- **Behaviour (the tests pin each):**
  - **`request_resort` refuses** (under `mu_`) with:
    - `table_refusal_`'s reasons (standing by, a change running, writes off, PV held off);
    - fewer than two TAPs: "a re-sort needs at least two TAPs";
    - any TAP not `running`: "TAP 1203 isn't answering; a re-sort needs every TAP";
    - any TAP without a read radio, or `key != "same"`: "TAP 1203's radio settings haven't been read yet" / "TAP 1203's key isn't the one the others share; use the shared key first";
    - the owner's window switch off: "the discovery window is held closed; no optimizer could join";
    - a re-sort already running or waiting: "a re-sort is already running".
    - A dry run needs neither writes nor the window switch.
  - **Starting.** It builds `ResortTap` from each TAP's `radio` and `saves_with_store(st.version)`, and the targets with `resort_networks(taps, draw_)`. An empty result is refused: "no free network ID left".
  - **While the job runs:**
    - the tick routes `resort_job_->due(t.id, …)` ahead of the TAP's table sources and housekeeping, and after its PV switch and window keeper;
    - the replies go to `resort_job_->on_reply(t.id, …)`;
    - every other table job, provision, period, herd and forget is refused: "a re-sort is running";
    - `may_send`'s `pv_off` argument is `(t.pv && t.pv->off()) || req.resort`;
    - a gate refusal of a resort frame calls `resort_job_->abort(...)`.
  - **The file:**
    - at `wants_save()`, it calls `resort_saved_` with phase `writing`, `waited` = the job's records' MACs, and `old_networks` = each TAP's current radio;
    - at the job's ok end it calls it again with phase `waiting`;
    - a failed job after `pv_off_sent()` keeps the file in phase `waiting` (so `can_put_back`) and logs the message;
    - a failed job before it deletes the file (null).
  - **Waiting** (file present, phase `waiting`):
    - every TAP's window keeper renews whatever automatic discovery wants. The owner's switch still wins: `set_renewing(discovery_renewing_ && (window_wanted_ || join_watch.wants_window() || resort_waiting_))`;
    - each TAP's table is read with a `TableWalk` every `RESORT_READ_MS` when no job runs on it;
    - when a TAP's window reading shows 0 s left and `resort_trim` names MACs for it, a `TableRemove(macs, false)` runs on that TAP before its renewal: hold `set_renewing(false)` on that TAP until the removal finishes. Log: "re-sort: taking N optimizers off TAP 1202 that joined another TAP". A MAC joined on two TAPs logs "re-sort: 04C05B…  is joined on TAP 1201 and TAP 1202; left on both" once;
    - `status().resort.joined` and `.missing` come from the last reads;
    - when `resort_missing` is empty and no trim is owed: the file is deleted, state `idle` with `finished`/`ok`, and the log says "re-sort: done: 20 on TAP 1201, 0 on TAP 1202, 20 on TAP 1203".
  - **`request_resort_stop`:** while waiting, deletes the file and leaves the missing listed: "re-sort: stopped waiting; 2 optimizers stay listed on every TAP". Refused when idle.
  - **`request_resort_put_back`:**
    - refused unless the file has `old_networks` and every TAP named in it is `running` ("TAP 1202 isn't answering; the old networks need every TAP");
    - runs `ResortJob(taps, old blocks in taps order, dry_run, true)` with the current tables as the list;
    - on ok it saves the file with `old_networks` cleared, phase `waiting`.
  - **Resume** (`set_resort_saved` with `resume`):
    - phase `writing`: at the first tick after `open()`, `0B00 01` goes to every TAP before anything else (`req.resort`, logged "re-sort: TigoMonitor restarted during the network writes; PV on to every TAP"), then the phase becomes `waiting` (saved);
    - phase `waiting`: the wait goes on.
  - **After the job's `check` step,** the TAPs' radios are already updated by `take_config_reply_` (its `0D 00 00` reads), so `known_networks_due_()` fires as for a provision. Further 0xE1s after the job's own are expected and harmless.

- [ ] **Step 1: Extend `MultiTap`.**
  - Record `0B00` payloads per TAP (`std::vector<uint8_t> sessions`).
  - Answer 0x26 pages from `records`, 0x29 writes into `records`, and 0x2B by clearing `records`, if it doesn't already (TableTap at line 458 has the page logic; reuse it).
  - Add `void join(uint16_t tap, uint16_t id)`, which clears 0x8000 on that TAP's record for id.
  - Add a `std::set<uint16_t> silent_after_pv_off` that makes a TAP stop answering after its first `0B00 00`.

- [ ] **Step 2: Write the failing tests** in `master_core_test.cpp`. Each starts three TAPs with `give_radio` (shared key 0x30…), 20/20/0 records joined, `set_known_networks(true)`, `set_table_writes(true)` and a `set_resort_saved` lambda that records each `ResortFile` (or null). Each ends with `CHECK(no_key_in(lines))`.
  - `test_resort_refusals`: each refusal above, by its words, plus a dry run allowed with writes off.
  - `test_resort_runs_and_waits`:
    - the file is saved before any `0B00 00` (compare the record's index with the first session byte's time, using a shared counter in both lambdas);
    - every TAP gets PAN ≠ old and the same new key;
    - all 40 are listed on all three TAPs;
    - `status().resort.state == "waiting"` and `missing.size() == 40`.
  - `test_resort_trims_at_window_close_and_finishes`:
    - after the job, `bus.join(0x1201, ids 0x10..0x23)` and `bus.join(0x1203, ids 0x24..0x37)`;
    - set each window to read 0 s left;
    - `run_for` past `RESORT_READ_MS`;
    - TAP 1201's records are only 0x10..0x23, TAP 1203's only 0x24..0x37, TAP 1202's none;
    - the last saved file is null (deleted), state `idle`, `ok`, and the "done:" log line names the counts.
  - `test_resort_windows_renew_despite_auto_discovery`: `set_window_wanted(false)` during the wait; windows reading 0 s left are still renewed (the renewals grow) and end `01 01`.
  - `test_resort_stop_waiting`: with 2 never joined, `request_resort_stop` deletes the file; the 2 stay listed on all TAPs; the log has "stay listed on every TAP".
  - `test_resort_tap_lost_mid_write`:
    - TAP 1202 is in `silent_after_pv_off`;
    - all three TAPs got `0B00 01` after their `0B00 00`;
    - the status is `finished && !ok` with the message naming "TAP 1202";
    - the file is kept, phase `waiting`, and `can_put_back`.
  - `test_resort_put_back`:
    - after a finished re-sort, `request_resort_put_back(false, why)` writes each TAP's old PAN and key back;
    - the file is saved with `old_networks` empty;
    - `can_put_back == false`.
  - `test_resort_resume_after_cut_writes`:
    - a new `MasterCore` with `set_resort_saved(cb, &file)` and phase `writing`;
    - the first frames after open include `0B00 01` to every TAP before any 0x2D or 0x29;
    - the saved phase becomes `waiting`.
  - `test_resort_blocks_other_changes`: during the wait, `request_table_add` and `request_provision` are refused with "a re-sort is running". Stop and put-back are not refused.

- [ ] **Step 3: Run.** Expected: compile errors, then FAILs.

- [ ] **Step 4: Implement** in MasterCore, per the behaviour above. Keep `resort_job_` and the waiting state on the tick thread, and `status_.resort` plus the request flags under `mu_`, as the table job does.

- [ ] **Step 5: Run the whole suite, then under ThreadSanitizer.**
  - Run: the build command, then the TSAN build of master_core_test as PR 1 did: `g++ -std=c++17 -O1 -g -fsanitize=thread -I components/tigo_monitor tests/host/master_core_test.cpp components/tigo_master/master_core.cpp -lpthread -o build/linux/mct_tsan && build/linux/mct_tsan`.
  - Expected: all pass, and no data race reported.

- [ ] **Step 6: Commit**

```bash
git add components/tigo_master/master_core.h components/tigo_master/master_core.cpp tests/host/master_core_test.cpp
git -c user.email=andrew@eiknet.com commit -m "feat(pi): the master runs a re-sort, waits for every optimizer, trims and can put the old networks back"
```

---

### Task 5: The daemon: the file, the routes, `/api/master`

**Files:**
- Create: `linux/src/resort_store.h`, `linux/src/resort_store.cpp` (and add `resort_store.cpp` to the daemon's sources: `linux/CMakeLists.txt` or the list `build.sh` compiles; follow where `tap_store.cpp` is listed)
- Modify: `linux/src/bus_master.h`: pass-throughs for `set_resort_saved`, `request_resort*`
- Modify: `linux/src/main.cpp`: load the file at bus-master creation, the draw (`std::random_device`), the callbacks
- Modify: `linux/src/http_server.h`, `linux/src/http_server.cpp`: routes and `/api/master`'s `resort`
- Modify: `site/screenshots/fixtures.mjs`

**Interfaces:**
- Consumes: Task 4's `ResortFile`, `ResortStatus` and the three requests.
- Produces:
  - **`ResortStore(const std::string &state_dir)`**, with these methods:
    - `bool save(const ResortFile &)`: `resort.json`, 0600, `write_file_atomic`;
    - `bool clear()`: unlink, true if gone;
    - `std::optional<ResortFile> load()`.
    - Each block is stored as the hex of `RadioBlock::encode()`, under `"tap"` and `"block"`. The MACs are hex.
    - Nothing in it is logged; `load()`'s failures log only "resort.json unreadable; ignored".
  - **`POST /api/master/resort`** `{dry_run: bool}` → 200 `{status: "ok"}`, or 409 `{error: why}`.
  - **`POST /api/master/resort/stop`** → 200, or 409.
  - **`POST /api/master/resort/put-back`** `{dry_run: bool}` → 200, or 409.
  - **`GET /api/master`** gains `"resort"`: `{state, dry_run, finished, ok, step, message, dry_frames: [..], started_unix, joined: [{tap, count}], missing: [{mac, id}], can_put_back}`.
  - All three POSTs sit behind the same auth as `/api/master/taps/provision`.

- [ ] **Step 1: Write the failing checks.** No host test harness covers `linux/src` stores, so the checks are the e2e in Task 7 and a fixture test:
  - add `resort` to the `/api/master` fixture in `site/screenshots/fixtures.mjs` (state `idle`, empty lists);
  - add to `site/test/fixtures-privacy.test.mjs` (or its pattern): no 32-hex-character run appears in any fixture.
  - Run: `cd site && npm test 2>&1 | tail -5`. Expected: PASS for the existing tests. The privacy check passes now, and stays as a guard.

- [ ] **Step 2: Implement** `ResortStore`, the wiring and the routes. In `main.cpp`:
  - give the MasterCore `set_resort_saved([&store](const ResortFile *f) { return f ? store.save(*f) : store.clear(); }, loaded ? &*loaded : nullptr)` and `set_resort_draw([&rd]() { return uint8_t(rd()); })`;
  - after a failed save the job stops (Task 3's `saved(false)`).

- [ ] **Step 3: Build.**
  - Run: the build command.
  - Expected: the daemon builds; all host tests pass.

- [ ] **Step 4: Commit**

```bash
git add linux/src/resort_store.h linux/src/resort_store.cpp linux/src/bus_master.h linux/src/main.cpp linux/src/http_server.h linux/src/http_server.cpp site/screenshots/fixtures.mjs site/test/fixtures-privacy.test.mjs
git -c user.email=andrew@eiknet.com commit -m "feat(pi): re-sort routes, its state on /api/master, and the old networks kept in resort.json"
```

---

### Task 6: The page: the Re-sort row, the wait, Add a TAP's last step, Replace a TAP

**Files:**
- Modify: `components/tigo_server/web/app.html`
- Test: `site/test/bus-master.test.mjs`, `site/test/setup-guides.test.mjs`

**Interfaces:**
- Consumes: Task 5's routes and `/api/master.resort`.
- Produces:
  - **A "Re-sort the optimizers" row** in `#pi-danger`, once per system, below the per-TAP rows (`piDangerRefresh`). It has Preview (`dry_run: true`) and a danger button "Re-sort". The confirm dialog, via `uiAsk` with tone `danger` and ok "Re-sort", has the body:

    > Every TAP gets a new network, and every optimizer looks for a TAP again and joins the one it hears. Production drops for a few minutes while they rejoin: on the first run it fell to zero for about 3 minutes and was back in about 8. Run it at low sun. At night the optimizers join at sunrise.

    The row's dry run shows `resort.dry_frames`, one per line.
  - **A waiting panel** on the Bus page while `resort.state` is `running`, `waiting` or `putting_back`:
    - "Re-sort: 31 of 40 joined: Garage 20, TAP 1202 8, Mid Roof 3", using the TAP names the page already resolves;
    - the missing, by optimizer name or label;
    - "Stop waiting" (POST `/resort/stop`, with a confirm saying the missing stay listed on every TAP);
    - "Put the old networks back" while `can_put_back`. Its confirm says every optimizer looks again and production dips again.
  - **The same control as Add a TAP's last step:** `SETUP_STEPS.addtap` becomes `['connect', 'allow', 'find', 'radio', 'name', 'resort', 'done']`, with `SETUP_STEP_NAMES.resort = 'Re-sort the optimizers'`. The step's guide is the row's copy plus the waiting panel. Next is enabled once a re-sort has started (any state but `idle`) or the owner chooses "Skip for now". Skipping says the new TAP gets no optimizers until a re-sort.
  - **A real Replace a TAP flow:**
    - `SETUP_STEPS.replacetap = ['connect', 'allow', 'find', 'radio', 'name', 'pick', 'resort', 'done']`;
    - the `connect` copy for replacetap: "Unplug the old TAP, then wire the new one in its place";
    - `pick` reuses `retiretap`'s list and POST (forget the old TAP);
    - its card in `renderSetup` becomes a live `task('replacetap', 'Replace a TAP', 'Swap a TAP for a new one, then let every optimizer choose its TAP again.')` and replaces the "Not yet" card.

- [ ] **Step 1: Write the failing site tests.**
  - In `bus-master.test.mjs`:
    - with `resort.state = 'waiting'`, joined `[{tap:'1201',count:20},{tap:'1203',count:11}]` and 9 missing, the page shows "31 of 40 joined", "Stop waiting" and "Put the old networks back";
    - with `can_put_back: false`, no put-back button;
    - clicking Re-sort posts `{dry_run:false}` to `/api/master/resort` after the confirm;
    - Preview posts `{dry_run:true}`.
  - In `setup-guides.test.mjs`:
    - Add a TAP has a `resort` step between `name` and `done`;
    - Replace a TAP is a live card whose flow runs `connect … pick, resort, done`;
    - no "Not yet" text for Replace a TAP;
    - no "CCA" in the new copy (`no-pi-in-copy`'s rule applies too).
  - Run: `cd site && npm test 2>&1 | tail -20`. Expected: the new tests FAIL.

- [ ] **Step 2: Implement** in app.html. Use the polling pattern already in the danger handler (`apiFetch('/api/master')`, every 500 ms, up to a minute for a dry run). The waiting panel refreshes with the Bus page's own poll.

- [ ] **Step 3: Run.** Expected: `npm test` all pass.

- [ ] **Step 4: Screenshots.** Run `cd site && npm run screenshots`, open the PNGs that show the Bus page and the setup page, and commit any changed `docs/images/*.png`.

- [ ] **Step 5: Commit**

```bash
git add components/tigo_server/web/app.html site/test/bus-master.test.mjs site/test/setup-guides.test.mjs docs/images
git -c user.email=andrew@eiknet.com commit -m "feat(web): re-sort the optimizers from the Bus page, at the end of Add a TAP, and in Replace a TAP"
```

---

### Task 7: The mock and an end-to-end run

**Files:**
- Modify: `linux/tools/mock_taps.py`
- Create: `linux/scripts/resort-e2e.sh`
- Modify: `.github/workflows/pi.yml`: add `resort` to the shard that runs `shared-network`

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces, as new mock flags:
  - **`--window S`:** a renewal opens S seconds (default 600).
  - **`--home N:LO-HI`** (repeatable): optimizer ids LO..HI (hex) join only on TAP number N. A provisional entry elsewhere never joins. Ids with no home keep today's behaviour: they join wherever they are written.
  - **`--never-join ID`** (repeatable).
- The stats also gain `pv` (each `0B00` byte, in order, with a sequence number shared across TAPs).

- [ ] **Step 1: Write `resort-e2e.sh`,** modelled on `shared-network-e2e.sh` (same `req`/`master`/`stat`/`wait_for` helpers):
  - **Setup:** three mock TAPs, `--tap 1201,01,20 --tap 1202,02,20 --tap 1203,03,0`, each `--radio` with one shared made-up key. `--home 1:<ids of A,B> --home 3:<ids of C>` puts the 20 that were on 1202 onto TAP 3. Also `--never-join` one id, `--window 6` and `--join-after 2`.
  - **The checks:**
    1. A dry run: 200, and `resort.dry_frames` is non-empty; the mock's stats show no `cfg_writes`.
    2. A refusal with `master_table_writes` false (a second daemon start, or the settings route): 409 with "table writes are off".
    3. A re-sort: 200. Within 30 s, `resort.state == 'waiting'`.
    4. The mock's `pv` sequence: every `00` comes before every write, and is followed by a `01` on every TAP.
    5. Each TAP's `cfg_writes` has a new PAN, and `key_same_as` is null (a new key) and the same for all three. Check by the stats' PAN strings differing from the starting ones; the key itself is never printed.
    6. `resort.json` exists with mode 600 (`stat -c %a`).
    7. Within 60 s: TAP 1's table holds only its home ids, TAP 3's only C's, and TAP 2's none. The missing list is exactly the never-join id.
    8. Restart the daemon. The re-sort is still `waiting`, `can_put_back` true, with the same missing id.
    9. Stop waiting: 200; `resort.json` is gone; the never-join id is listed on all three TAPs.
    10. Run a second re-sort to the end, then a put-back: each TAP's `cfg_writes` gets its last PAN back.
    11. **No key or PAN leaks:** the daemon's log and every saved `/api/master` answer contain none of the made-up key's hex, and no starting or new PAN hex taken from the stats. (Only the test reads those values; it prints only ok/FAIL.)
  - Make it executable. Add it to the pi.yml shard list next to `shared-network`.

- [ ] **Step 2: Run and watch it fail** before the mock flags exist.
  - Run: `docker run --rm -v "$PWD":/src -w /src tigod-dev sh -c 'linux/scripts/build.sh >/dev/null && linux/scripts/resort-e2e.sh'`
  - Expected: FAIL (unknown mock flag `--home`).

- [ ] **Step 3: Implement the mock flags** in `mock_taps.py`, and update its docstring.

- [ ] **Step 4: Run** the e2e, then `shared-network-e2e.sh`, `provision-e2e.sh`, `mesh-e2e.sh` and `master-e2e.sh`. Expected: each prints its "passed" line.

- [ ] **Step 5: Commit**

```bash
git add linux/tools/mock_taps.py linux/scripts/resort-e2e.sh .github/workflows/pi.yml
git -c user.email=andrew@eiknet.com commit -m "test(pi): a re-sort end to end against three mock TAPs"
```

---

### Task 8: CHANGELOG, the spec's status, the PR

**Files:**
- Modify: `CHANGELOG.md` (under `## [Unreleased]`, Added)
- Modify: `docs/superpowers/specs/2026-11-23-tap-resort-design.md`: the status line becomes "built on feature/tap-resort, PR #N"

- [ ] **Step 1: The CHANGELOG entry,** in the house shape (shipping-tigo-changes), with a bold lede for the owner's symptom:

> **A new TAP can now get optimizers: re-sort them, and each joins the TAP it hears best (Pi, experimental).** Settings → Bus → dangerous options, the last step of Add a TAP, and the new Replace a TAP all offer it. Every TAP gets a new network, every optimizer joins the TAP it hears, and each TAP is then trimmed to its own. Production drops for a few minutes, so run it at low sun. Until every optimizer is back, the old networks can be put back. With more than one TAP, discovery windows now carry the known-networks flag.

- [ ] **Step 2: The full suite.**
  - Run: `docker run --rm -v "$PWD":/src -w /src tigod-dev linux/scripts/build.sh`, then every `linux/scripts/*e2e.sh`, then `cd site && npm test`.
  - Expected: all pass. Report any that fail by name, with output.

- [ ] **Step 3: Commit, push the branch and open the PR against `main`.** The PR body has:
  - what it does;
  - the CCA evidence (labels only);
  - the owner's decisions;
  - the test counts;
  - the roof test from the spec;
  - the bench step (two G8s: the writes, saves, file and put-back);
  - "Not tested: a real optimizer joining after a re-sort (no real optimizers on the bench)".
  - It ends with "🤖 Generated with [Claude Code](https://claude.com/claude-code)".
  - Check `gh pr view N --json baseRefName` reads `main`.

```bash
git add CHANGELOG.md docs/superpowers/specs/2026-11-23-tap-resort-design.md
git -c user.email=andrew@eiknet.com commit -m "docs: changelog for the re-sort"
git push -u origin feature/tap-resort
```
