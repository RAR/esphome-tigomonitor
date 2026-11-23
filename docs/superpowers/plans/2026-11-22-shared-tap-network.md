# One Network for Every TAP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every TAP on a Pi-run bus shares one network key, a TAP with the
wrong key can be re-keyed from dangerous options, and each TAP is told the
other TAPs' networks with 0xE1 in the CCA's shape.

**Architecture:** The wire shape and gate for 0xE1 go in `tigo_master.h`,
the shared-key choice and re-key in `tap_config.h`'s provision job, and the
bookkeeping in `MasterCore`. MasterCore keeps each TAP's last-read radio
block in memory, says per TAP whether its key matches, fills a provision's
key from the others, and sends 0xE1 on events. The Pi daemon turns it on,
the API gains two words per TAP, and the page gains a Network key row and
Add a TAP copy.

**Tech Stack:** C++17 headers shared by the ESP32 and the Pi, the Pi
daemon (cpp-httplib, nlohmann::json), Python mocks and POSIX sh e2e
scripts, `app.html`, and node:test + Playwright site tests.

**Spec:** `docs/superpowers/specs/2026-11-22-shared-tap-network-design.md`

## Global Constraints

- **No key outside the daemon's memory and the TAP:** no log line, API
  field, page, fixture, commit, test literal from a real TAP, or chat
  output. Classification words only: `same`, `different`, `unread`,
  `alone`.
- **No real PAN, key or MAC in fixtures or tests.** Use made-up values
  such as `0x7E21`, keys `0x30+i`, and MACs `04C05B0000A1B2xx`.
- **0xE1 request args:** `count 00 00 00` then `count × { ch 00 PAN_hi
  PAN_lo key[16] }`, with count 1..16 and channel 11..26. Nothing else
  passes the gate.
- **0xE1 is gated as `Allow::table_write`**: it needs Allow changes to the
  TAPs (`master_table_writes`), and the bench switch doesn't allow it on
  its own.
- **0xE1 is sent only when `MasterCore::set_known_networks(true)`**, which
  only the Pi daemon calls.
- **E2 status words:** 0 `accepted`, 1 `refused: an invalid channel`, 2
  `refused: an invalid network ID`, 3 `refused: no room for another
  network`, other `refused (status N)`, and no reply `no answer`.
- **Commits** use `git -c user.email=andrew@eiknet.com` and no trailers.
  The type/scope vocabulary is in `.claude/skills/shipping-tigo-changes`.
- **UI copy** says "TigoMonitor", makes no CCA comparisons, and follows
  `snake_case` JSON and `kebab-case` IDs.
- **Any `/api/master` shape change** updates
  `site/screenshots/fixtures.mjs` in the same commit.
- **New host tests** are added to `linux/scripts/build.sh`'s list. The
  existing test files are already in it.
- **Base:** this branch is cut from main, which doesn't have #180 (the
  channel change). Rebase onto main once #180 merges, before Task 2. If it
  hasn't merged, Task 2's `keep_network` branch has no `new_channel` line
  to keep. A channel change is a provision, so Task 4's hook covers it
  after the rebase with no change.

## Review Focus

1. **A TAP in the 46-byte radio form** (a G8.59) among the others. Its key
   is read from the same offset and compared like a 36-byte one. Task 3
   tests a mixed pair.
2. **A table job running when 0xE1 falls due.** It waits until the job
   ends, and its reply is never routed to the job. Task 4.
3. **Allow changes to the TAPs off.** No 0xE1 goes out, the TAP's
   known-networks word says why, and turning the switch on sends it.
   Task 4.
4. **A lost TAP that was read before.** It is still named in the others'
   0xE1. One never read is left out, and is not sent one. Task 4.
5. **Key bytes in any log line or status string** from the provision and
   0xE1 paths. They must be absent. Tasks 3 and 4 scan every captured log
   line for the made-up key's hex.

---

### Task 1: 0xE1 on the wire: encoder, gate, reply status

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h` (new helpers near
  `tunnel_request`; `classify_request`'s `case 0xE1`)
- Test: `tests/host/master_protocol_test.cpp`

**Interfaces:**
- Produces:
  - `struct KnownNetwork { uint8_t channel; uint16_t pan; std::array<uint8_t, 16> key; };`
  - `static const uint8_t SUB_KNOWN_NETWORKS = 0xE1, SUB_KNOWN_NETWORKS_REPLY = 0xE2;`
  - `static const size_t KNOWN_NETWORKS_MAX = 16;`
  - `Bytes known_networks_args(const std::vector<KnownNetwork> &others);`
    1..16 entries; the caller refuses more.
  - `bool is_known_networks_args(const Bytes &args);`
  - `int known_networks_status(const Frame &reply);`: the status byte, or
    -1 for anything that isn't a `0B10 … E2` with data.
  - `std::string known_networks_status_text(int status);` gives the words in
    Global Constraints.

- [ ] **Step 1: Write the failing tests** in `master_protocol_test.cpp`:
  - `test_known_networks_args_one_and_two`: for `{15, 0x7E21, key 0x30..0x3F}`,
    the args are `01 00 00 00 0F 00 7E 21 30 31 … 3F` (24 bytes). Adding
    `{11, 0x5A5A, key 0x40..0x4F}` gives `02 00 00 00`, then the first
    record, then `0B 00 5A 5A 40 … 4F` (44 bytes).
  - `test_known_networks_gate`:
    - `classify_request(CMD_TUNNEL, tunnel_request(0xE1, 7, args))` is
      `Allow::table_write` for both of the above.
    - It is `Allow::never` for count 0, count 17, a nonzero byte 1..3, a
      length off by one, a record whose byte 1 isn't 0, and channel 10 or
      27.
    - `may_send(..., /*bench=*/true, ...)` with `table_writes=false` is
      false.
  - `test_known_networks_status`: replies `0B10 00 0E 00 E2 07 00 0F 7E 21`
    → 0 → `accepted`; status 3 → `refused: no room for another network`; a
    `0B10` with sub `0x2F` → -1 → `no answer`.
  - Keep the never-anywhere loop as it is: its E1 args `{}`, `00 02` and
    `00 02 80 00` are all invalid, so it must still pass.
- [ ] **Step 2: Run them to see them fail.**
  `g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor tests/host/master_protocol_test.cpp -o /tmp/mpt && /tmp/mpt`
  Expected: compile errors for `known_networks_args` and the rest.
- [ ] **Step 3: Implement** the helpers in `tigo_master.h`, with
  `#include <array>`. In `classify_request`, take `case 0xE1` out of the
  never block: it returns `is_known_networks_args(args) ? Allow::table_write
  : Allow::never`.
- [ ] **Step 4: Run them to see them pass,** and run
  `tests/host/bus_watch_test.cpp` the same way. Its E1 masking test must
  still pass.
  Expected: `all passed` for both.
- [ ] **Step 5: Commit.**
  `feat(master): the CCA's 0xE1 SetKnownNetworks shape, allowed behind Allow changes to the TAPs`

### Task 2: The shared key and the re-key in the provision job

**Files:**
- Modify: `components/tigo_monitor/tap_config.h` (`RadioBlock`, the
  `ProvisionJob` `read_before` branch, a new `shared_key`)
- Test: `tests/host/tap_config_test.cpp`

**Interfaces:**
- Produces:
  - `enum class KeyMatch { alone, unread, same, different };`
  - `KeyMatch shared_key(const std::vector<const RadioBlock *> &others, std::array<uint8_t, 16> &key);`
    - `others` holds one entry per other TAP that counts, with `nullptr`
      for one not read yet.
    - Empty: `alone`.
    - Any two read keys differ: `different`.
    - Any `nullptr` and no disagreement: `unread`.
    - Otherwise `same`, and `key` is set.
  - `RadioBlock::shared_key` (bool, not sent). With `keep_network`, the
    TAP's own key isn't copied from its read: the block's `key`, which
    MasterCore filled in, is written instead.

- [ ] **Step 1: Write the failing tests:**
  - `test_shared_key_choice`:
    - no others → `alone`;
    - two blocks with key `0x30+i` → `same` and the key is returned;
    - one block with `0x30+i` and one with `0x40+i` → `different`;
    - one read and one `nullptr` → `unread`;
    - a 46-byte (`wide`) block and a 36-byte block with the same key →
      `same`.
  - `test_provision_rekey_keeps_channel_pan_profile`:
    - `FakeTap` ram: channel 11, pan `0x7E21`, key `0x50+i`, map for 20.
    - Request: `keep_network`, `keep_tap_profile`, `shared_key`, key
      `0x30+i`.
    - Afterwards: `job.ok()`, `tap.ram.channel == 11`, `pan == 0x7E21`,
      `key[0] == 0x30`, `map[0] == 20`.
    - `job.message()` contains neither `3031` nor `5051`.
- [ ] **Step 2: Run them to see them fail.**
  `g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor tests/host/tap_config_test.cpp -o /tmp/tct && /tmp/tct`
  Expected: compile errors (`shared_key`, `KeyMatch`).
- [ ] **Step 3: Implement.** In `read_before`'s `keep_network` branch, do
  `if (!block_.shared_key) block_.key = now.key;`. The channel and PAN
  lines stay as PR #180 left them.
- [ ] **Step 4: Run to see them pass,** along with the rest of
  `tap_config_test`.
  Expected: `all passed`.
- [ ] **Step 5: Commit.**
  `feat(setup): a TAP can take the key the other TAPs share, keeping its channel and network ID`

### Task 3: MasterCore remembers each TAP's radio settings and fills the shared key

**Files:**
- Modify:
  - `components/tigo_master/master_core.h`: `Tap::radio`,
    `TapStatus::key`, `set_known_networks`, `known_networks_`.
  - `components/tigo_master/master_core.cpp`: the config-read condition at
    its three uses, storing replies, key words, `request_provision`.
- Test: `tests/host/master_core_test.cpp` (extend `MultiTap::One` with a
  radio block)

**Interfaces:**
- Consumes: `RadioBlock::decode`, `shared_key`, `KeyMatch` (Task 2).
- Produces:
  - `void MasterCore::set_known_networks(bool on);`
  - `Tap::radio` (`std::optional<RadioBlock>`), set from every reply to a
    `0D 00 00` read, whoever sent it.
  - `TapStatus::key` (`std::string`): `same`, `different`, `unread` or
    `alone`, recomputed whenever any TAP's `radio` changes.
  - `request_provision` with `block.shared_key`:
    - `alone`: the block's own (random) key stands.
    - `unread`: refused with `TAP %04X's radio settings haven't been read
      yet; try again in a minute`, naming the first one unread.
    - `different`: refused with `your other TAPs don't share a key`.
    - `same`: `block.key` is set before the job starts.
    - The others counted are every TAP but the target, minus lost TAPs
      never read.

- [ ] **Step 1: Write the failing tests** (MultiTap answers `0D 00 00` with
  its `One::cfg`, takes `0D 01 00` writes into it, `0D 00 01` saves, and
  `0012` stores):
  - `test_radio_reads_give_key_words`:
    - Three TAPs with `set_known_networks(true)`. 1201 and 1202 have key
      `0x30+i`; 1203 has `0x50+i` and is a 46-byte TAP.
    - After the opening reads: 1201 and 1202 are `same`, 1203 is
      `different`.
    - With `set_known_networks(false)` and `set_read_tap_config(false)`, no
      `0D` read is sent and every word is `unread`.
  - `test_provision_takes_the_shared_key`:
    - `request_provision(mac_1203, block{keep_network, keep_tap_profile,
      shared_key, key 0x99…}, false, false, why)` returns true.
    - Afterwards 1203's fake `cfg.key[0] == 0x30` and its channel and PAN
      are unchanged.
    - 1203's word becomes `same`.
  - `test_provision_shared_key_refusals`:
    - 1201 and 1202 with different keys → false, and `why == "your other
      TAPs don't share a key"`.
    - 1202 never answering its read, while it isn't lost → `why` names
      1202.
    - A single TAP with `shared_key` → true, and the written key is the
      block's own.
  - In every one of these tests, collect the log lines and `CHECK` that
    none contains `3031` or `5051`.
- [ ] **Step 2: Run to see them fail.** Use the g++ line from
  `linux/scripts/build.sh` for `master_core_test`.
  Expected: compile errors (`set_known_networks`, `TapStatus::key`).
- [ ] **Step 3: Implement.**
  - Add one private helper, `bool reads_config_() const { return
    read_tap_config_ || known_networks_; }`, and use it in place of
    `read_tap_config_` at its three uses in the housekeeping-plan
    condition, `plan.config_read` and the `bring_up` gate.
  - After `transact`, when `ok` and the request is a `0D 00 00` tunnel
    read, decode the reply's block into `t.radio` and recompute the words
    under `mu_`.
  - Logging stays outside the lock.
- [ ] **Step 4: Run** the full `master_core_test`.
  Expected: `all passed`.
- [ ] **Step 5: Commit.**
  `feat(master): know which TAPs share a key, and give a new one theirs`

### Task 4: MasterCore sends 0xE1 on events

**Files:**
- Modify: `components/tigo_master/master_core.h` and `.cpp` (`Tap::known_due`,
  `TapStatus::known_networks`, the send step in the per-TAP tick, the
  event hooks)
- Test: `tests/host/master_core_test.cpp` (MultiTap answers `E1` with
  `00 0E 00 E2 seq <One::e1_status> <ch> <pan>`, and records each request's
  args)

**Interfaces:**
- Consumes: Task 1's helpers, and `Tap::radio` from Task 3.
- Produces:
  - `TapStatus::known_networks` (`std::string`): `""` before anything was
    tried, `accepted (N other TAPs)`, the status words otherwise,
    `waiting: Allow changes to the TAPs is off`, or `not sent: more than 16
    other TAPs`.
  - `Tap::known_due` is set for every TAP:
    - in `open()`;
    - when a provision job finishes ok and not as a dry run;
    - in `set_taps`, `herd_done_` and a successful `request_forget_tap`.
  - It is set for one TAP when `tap_answered_` brings it back from lost.

- [ ] **Step 1: Write the failing tests:**
  - `test_known_networks_at_start`:
    - Three TAPs, known networks on, table writes on.
    - After the reads, each TAP got exactly one E1. Its count is 2, and its
      records are the other two in `taps_` order (by channel).
    - Each status reads `accepted (2 other TAPs)`.
    - None is sent again in the next 120 s.
  - `test_known_networks_waits_for_writes`:
    - With table writes off, no E1 is sent and each word reads `waiting:
      Allow changes to the TAPs is off`.
    - After `set_table_writes(true)`, all three are sent.
  - `test_known_networks_after_provision`: after a provision on 1203
    completes, every TAP gets one more E1.
  - `test_known_networks_waits_for_a_table_job`: a table add on 1201 that
    is still running means no E1 goes out until it finishes, and the add's
    reply handling sees none of the E2s.
  - `test_known_networks_lost_and_unread`:
    - 1202 was read, then went lost: it is still in 1201's and 1203's
      records, and it isn't sent one while lost.
    - When it comes back, it alone is sent one.
    - A TAP lost before its first read is in no record.
  - `test_known_networks_refusal_is_said_once`:
    - 1203's mock answers status 3: its word reads `refused: no room for
      another network`, and it isn't retried in the next 120 s.
  - In each of these, scan the log lines for the made-up keys' hex: none.
- [ ] **Step 2: Run to see them fail.**
  Expected: compile errors or failed CHECKs.
- [ ] **Step 3: Implement** the send step in the per-TAP tick, before
  `next_request`. It runs when all of these hold:
  - `known_networks_ && t.known_due && !t.lost && t.radio`;
  - `!table_job_ && !table_job_pending_`;
  - every other TAP is read, or lost.

  Then:
  - Others: each other TAP with `radio`, in `taps_` order, mapped to
    `KnownNetwork{uint8_t(radio->channel), radio->pan, radio->key}`.
  - No others → clear `known_due`.
  - More than 16 → set that word and clear `known_due`.
  - Table writes off → set the waiting word and keep `known_due`.
  - Otherwise send `Request{CMD_TUNNEL, tunnel_request(SUB_KNOWN_NETWORKS,
    seq_ = next_seq(seq_), args), 2000}` with `table_edit = true`, through
    the existing gate and `transact`.
  - On the reply, set the word from `known_networks_status`, clear
    `known_due` whatever the answer, and log
    `bus master: TAP %04X told the other TAPs' networks (%zu): %s`.
- [ ] **Step 4: Run** the full `master_core_test`.
  Expected: `all passed`.
- [ ] **Step 5: Commit.**
  `feat(master): tell each TAP the others' networks (0xE1) at start and after a change (Pi)`

### Task 5: The daemon, the API and a several-TAP e2e

**Files:**
- Modify:
  - `linux/src/main.cpp`: `set_known_networks(true)`, `block.shared_key`
    in the provision lambda.
  - `linux/src/http_server.cpp`: the provision route's `"shared_key"`, and
    `/api/master` taps' `key` and `known_networks`.
  - `site/screenshots/fixtures.mjs`: `/api/master` taps get `key: 'alone',
    known_networks: ''`.
  - `linux/tools/mock_taps.py`.
- Create: `linux/scripts/shared-network-e2e.sh`.

**Interfaces:**
- Consumes: Tasks 3 and 4.
- Produces:
  - **Provision route:** `POST /api/master/taps/provision` takes `"shared_key":
    true` (default false). Add a TAP's request (no `keep_network`) always
    sends it true from the page (Task 6). The re-key sends `{"mac",
    "keep_network": true, "shared_key": true, "dry_run"}`.
  - **`/api/master` taps[]:** each TAP gains `"key"` and `"known_networks"`
    strings.
  - **mock_taps.py:**
    - `--tap ID,MAC[,PANELS[,new]]` gains `--radio N,CH,PANHEX,KEYBYTE`. It
      sets TAP N's block: channel, PAN, and a 16-byte key of `KEYBYTE`
      repeated.
    - The `0D` read answers with that block, and `0D 01 00` writes and
      `0D 00 01` saves work as `mock_tap.py`'s do.
    - `E1` answers status 0 with its own channel and PAN.
    - Per TAP, stats record `e1: [{count, channels}]` and `cfg_writes:
      [{channel, pan, key_same_as: N|null}]`, where `key_same_as` names
      which TAP's mock key the written key equals. No key bytes go into
      stats.

- [ ] **Step 1: Write the e2e.** `shared-network-e2e.sh`, built like
  `multitap-e2e.sh`, runs three mock TAPs with `--radio 1,15,7E21,30`,
  `--radio 2,18,5A5A,30` and `--radio 3,11,1A2B,77`. Settings: master,
  table writes on, web password `pw`. It expects:
  - `/api/master` keys: `same`, `same`, `different`.
  - Each mock got one E1 with count 2, and the channels it was told are
    the other two's.
  - Re-key 1203: a dry run shows `key hidden` and writes nothing. The real
    one leaves `cfg_writes[-1]` as `{channel: 11, pan: '1A2B', key_same_as:
    1}`. Then every key word reads `same`, and each mock got a second E1.
  - Add a TAP's shared key: a TAP brought in by a herd (a fourth `--tap
    1235,..,0,new`) takes key_same_as 1.
  - `grep -c` on the daemon log for `30303030` and `77777777`: 0.
- [ ] **Step 2: Run it to see it fail** (dev container, after build.sh).
  `docker run --rm -v "$PWD":/src -w /src tigod-dev sh -c 'linux/scripts/build.sh >/dev/null && linux/scripts/shared-network-e2e.sh'`
  Expected: FAIL lines (no `key` field, no E1).
- [ ] **Step 3: Implement** the daemon wiring, the route, the JSON fields,
  the fixture and the mock.
- [ ] **Step 4: Run it to see it pass,** along with provision-e2e and
  multitap-e2e in the same container.
  Expected: `shared network e2e passed`, `provision e2e passed`,
  `multitap e2e passed`.
- [ ] **Step 5: Commit.**
  `feat(pi): every TAP on one key, and the others' networks sent — the daemon and API`

### Task 6: The page: the Network key row and Add a TAP's key line

**Files:**
- Modify: `components/tigo_server/web/app.html` (the dangerous options card
  in `piDangerRefresh` and its click handler; the `addtap` radio step's
  `setup-found` key line; Add a TAP's provision body gets `shared_key:
  true`)
- Test: `site/test/bus-master.test.mjs`, `site/test/setup-guides.test.mjs`

**Interfaces:**
- Consumes: `/api/master` taps[].key and `.known_networks` (Task 5).
- Produces:
  - `data-danger-key` rows with `data-danger="key-preview"` and
    `data-danger="key-write"`.
  - The words:
    - `same`: "The same key as your other TAPs"
    - `different`: "A different key from your other TAPs: optimizers
      can't move to it"
    - `unread`: "Not read yet"
    - `alone`: "The only TAP"
  - The row's buttons are disabled for `same` and `alone`.

- [ ] **Step 1: Write the failing site tests:**
  - **Bus-master tests**, with `/api/master` taps 1201 `same`, 1203
    `different`, and dangerous options on:
    - 1203's row shows the "different" words and its Write is enabled;
      1201's buttons are disabled.
    - Preview posts `{mac, keep_network: true, shared_key: true, dry_run:
      true}`.
    - Write opens a red dialog whose text matches `/drop off it/` when the
      TAP has `discovery.node_count > 0`, and `/no optimizers/` when it
      has 0, then posts `dry_run: false`.
  - **Setup-guides test:** in Add a TAP's radio step with two TAPs, the
    key line reads "The key your other TAPs share" / "never shown", and
    the provision body includes `shared_key: true`. With one TAP it reads
    "A new network key" as now.
- [ ] **Step 2: Run them to see them fail.**
  `cd site && node --test --test-name-pattern="key" test/bus-master.test.mjs test/setup-guides.test.mjs`
  Expected: failures.
- [ ] **Step 3: Implement** the row beside Channel, the click branch
  (`what === 'key'`, polling `kind === 'provision'` like channel), and the
  Add a TAP line.
- [ ] **Step 4: Run** `cd site && npm test`.
  Expected: every test passes, 312 or more.
- [ ] **Step 5: Commit.**
  `feat(web): a Network key row in dangerous options, and Add a TAP says it shares the others' key`

### Task 7: The capture check (outside the repo), CHANGELOG, and the full run

**Files:**
- Create (scratchpad, not committed): a one-off `e1check.cpp` that reads
  `(channel, pan, key, captured args)` lines from a 0600 temp file. The
  file is written by a Python extractor over the owner's capture.
  `e1check.cpp` prints only `N of M identical`.
- Modify: `CHANGELOG.md` (`## [Unreleased]`: Added, Fixed)

- [ ] **Step 1: Run the capture check.**
  - Extract each captured E1's network and args, and the network the
    other TAP's `0D` read showed at that time, to `$SCRATCH/e1.txt`
    (chmod 600).
  - Compile `e1check.cpp` against `components/tigo_monitor/tigo_master.h`.
  - Expected: `70 of 70 identical`, or whatever the count is: all of them.
  - Delete `e1.txt` after.
- [ ] **Step 2: Write the CHANGELOG entries.**
  - **Fixed:** "A TAP added by TigoMonitor got a network key of its own,
    so no optimizer could ever join it…" Say what to do: re-key it in
    dangerous options.
  - **Added:** the Network key row.
- [ ] **Step 3: The full run.** In the container, run `build.sh` and every
  `linux/scripts/*-e2e.sh`. Then `cd site && npm test`.
  Expected: all pass, apart from the two `mock_tap.py --panels 0`
  tracebacks that main also prints.
- [ ] **Step 4: Commit.**
  `docs(changelog): one network for every TAP`
- [ ] **Step 5: Bench (hardware, so the owner's go-ahead first).**
  - Put both bench TAPs on the FTDI cable at different addresses. Both are
    at 1235 after the 11-22 reset, so bring them up one at a time.
  - Run a scratch TigoMonitor in Docker with table writes on. Give each
    TAP its settings with Add a TAP; the second must take the first's key.
  - Expected: both TAPs read `accepted (1 other TAP)` in the Bus log, and
    both Network key rows read `same`.
  - Then put each back to factory with `tap_factory_reset.py --at`.
