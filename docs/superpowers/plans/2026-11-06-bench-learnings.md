# What's left from the 2026-11-06 bench capture — plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement a code task from this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Bench and roof sessions are run with the owner, not by an agent.

**Goal:** turn every finding from the backup-CCA bench capture and the owner's H1.0007 analysis into one of four things: a code change, a docs fix, a bench or roof session, or a recorded "no action".

**Architecture:** the code tasks are small and touch only what the master already sends. Nothing here adds a frame the CCA never sent. The two features that would (Phase 1's Add-optimizer UI and Phase 2's table write) keep their own design doc and get their own implementation plan when started.

**Tech Stack:** C++17 (`tigo_master.h`, `master_core.cpp`), host tests in `tests/host/`, Linux daemon (`linux/src/`), the shared `app.html`.

**Spec:** `docs/superpowers/specs/2026-11-05-optimizer-commissioning-design.md`. Findings are summarised in memory under *cca-enrollment-static-findings*.

## Global Constraints

- No new frame shapes. The send gate (`classify_request`) stays as it is in 2720da4.
- Never log, commit or display: the 0x0D reply blob, the PAN value, MACs, serials, barcodes, the RS485 key.
- Any `/api/*` JSON shape change updates `site/screenshots/fixtures.mjs` in the same commit.
- Bench before roof. The backup CCA never touches the roof bus.
- Every bench or roof session that changes who holds a bus, or stops stack 68, needs the owner's go-ahead on the day.

## Review Focus

1. A TAP with an empty table: `0x2F` reports 0 in table, so the renewal guard (no count of 0) never renews. Fine today; it blocks Phase 0 step 2 on a wiped TAP. Task 3 records it.
2. A 0x2F reply shorter than 9 bytes (the 4.x 9-byte request form vs the 8-byte one): the parser must still reject it, not read past the end.
3. The PAN watch on a TAP that's been re-addressed: the first 0x0D read after a drop-back must set the baseline, not raise a change.
4. Wording that says how many optimizers have joined must not be read as "how many are reporting": joined is a persistent flag, so a dead optimizer still counts.

---

## Where each finding goes

| Finding | Goes to |
|---|---|
| STORE commits the RS485 id; 0x000E for H-series | **Done** in 2720da4 (on both Pis; untested on hardware) |
| 0x2F order: flag, remaining, count echo, joined, in table | Task 2 (our parser already reads the right field; name the other two and show them) |
| The 0x2D count is ignored; the window closes on time | No action. We keep copying the CCA's count. Task 1 records it |
| A TAP power loss keeps joins, loses the window | Task 1 (docs); Session B2 confirms our re-open |
| 0x8000 = in the table, not joined | Task 4 (the Node Table tag wording) |
| PAN = bytes 2–3 of the 0x0D blob, set by whichever CCA last ran the TAP | Task 5 (owner's call) |
| The 0x0D (GatewayCfg) blob layout, from the CCA UI's own decode | Byte 1 is the channel (bench TAPs 15 and 23, one per TAP), 2–3 the PAN, 4–7 and 11–12 the slot map, 13 SLT, **14–29 a 16-byte network key**, then Optimizn and SLm. Byte 4 was never the channel. Task 5 reads only bytes 2–3; the key must never be logged |
| 0x29 table write, ack codes, first-fit, never rewrite a listed MAC | Phase 2 of the design doc; not approved |
| 0x2B is a real erase | Stays banned. No action |
| 0x13, 0x17, 3.6's `Smrt_` shape | No action: 002D and the roof's 40 report correctly without them |
| 0x22 with data, `Cal` | No action (`Cal` is a read we could add later if a reply turns out useful) |
| 0x0149 layout | No action: confirms the parser we have |
| No single-entry delete | Session B1 captures what the CCA does instead |
| STORE only after a failed check (two-TAP capture) | No action: our drop-back path already does this |
| The hop goes to the next free id, not "one up" | No action while we support one TAP (one up = next free). Revisit with multi-TAP support |
| A CCA reflashes a TAP over RS485 (0x0520/0x0522/0x0526) | No action: never sent. Another reason to keep CCAs off the roof bus |
| 0x0146 `00 02` = text query to an unreachable node | Task 7 |
| An optimizer power-off keeps its join | No action: confirms we need nothing for a rejoin |
| The window read closed 23 s after the CCA opened it | Watch for it; no theory yet |
| Short code → long address check letter | Session B4 |
| An optimizer not in the table: does it join? | Session B3; decides whether Phase 2 is needed at all |
| CCA allocates TUID 2, not preferred-then-random | No action; Phase 2 picks its own free TUID |

---

### Task 1: Bring the commissioning design doc up to date — done 2026-11-06 with the evening's write-up

**Files:**
- Modify: `docs/superpowers/specs/2026-11-05-optimizer-commissioning-design.md`

Docs only; no tests.

- [ ] **Step 1:** Status line: Phase 0 observations 1 and 3 are done; 2 is open.
- [ ] **Step 2:** STORE section and the Guardrails STORE line: built in 2720da4, key-only, sent **only on the drop-back path** after the re-address (so the roof G8.65 never gets it), not "after the verify, before the hop". Every other STORE form stays banned.
- [ ] **Step 3:** Open questions: mark 2 (the record is count + MAC + TUID), 3 (the TUID rides in the table record), 4 (entries and joins survive a power cycle) and 5 (none needed, so far) as answered, with one line each. Leave 1 and 6 open.
- [ ] **Step 4:** Add under Phase 2: the 0x2D count is stored and echoed, never compared; the window closes on time only.
- [ ] **Step 5:** Out of scope, "Removing an optimizer": add that the TAP has no single-entry delete; removal would be erase + rewrite, which we ban.
- [ ] **Step 6:** Commit: `docs: the commissioning doc catches up with 2720da4 and the H1.0007 analysis`

### Task 2: Name the 0x2F counts and show them

The parser already takes the third count (`data[7..8]`, in table) for the renewal, which the analysis confirms. The other two are discarded.

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h` (`DiscoveryWindow`, `parse_discovery_window`, the comment above it)
- Modify: `components/tigo_master/master_core.cpp`, `components/tigo_master/tigo_master_component.cpp`, `linux/src/http_server.cpp`, `linux/src/mqtt_publisher.cpp` (wherever `remaining_s` is published)
- Modify: `components/tigo_server/web/app.html` (Discovery window card), `site/screenshots/fixtures.mjs`
- Test: `tests/host/master_protocol_test.cpp`, `site/test/bus-master.test.mjs`

**Interfaces:**
- Produces: `DiscoveryWindow { bool running; uint16_t remaining_s; uint16_t requested; uint16_t joined; uint16_t node_count; }`. `node_count` keeps its name and meaning (in table) so `discovery_renewal_args` and its callers don't change. The JSON gains `joined` and `in_table` beside the existing window fields.

- [ ] **Step 1: Write the failing test** `test_discovery_window_counts`:
  - roof reply `01 03 84 00 2C 00 28 00 2C` → `requested == 44`, `joined == 40`, `node_count == 44`;
  - bench reply `01 03 84 00 01 00 00 00 01` → `1, 0, 1`;
  - an 8-byte reply → `parse_discovery_window` returns false;
  - `discovery_renewal_args(44)` is byte-identical to before.
- [ ] **Step 2:** `./build.sh` → FAIL: no member `requested`/`joined`.
- [ ] **Step 3:** Add the fields; rewrite the comment to the confirmed order (flag, remaining, count echo, joined, in table).
- [ ] **Step 4:** `./build.sh` → all host suites pass.
- [ ] **Step 5:** Publish `joined` and `in_table`. Card copy: "40 of the 44 in the TAP's table have joined". Add both fields to the fixture. A JS test asserts the sentence renders, and that a missing field renders nothing rather than `undefined`.
- [ ] **Step 6:** `node --check` on app.html's script, `cd site && npm test`, then `npm run screenshots`; open the Bus master PNG.
- [ ] **Step 7:** Commit: `feat(master): show how many of the TAP's table have joined`

### Task 3: Record the empty-table renewal gap

**Files:**
- Modify: `components/tigo_monitor/tigo_master.h` (the guard comment near `discovery_renewal_args`)
- Test: `tests/host/master_protocol_test.cpp`

- [ ] **Step 1:** Test `test_no_renewal_for_an_empty_table`: a 0x2F reading with 0 s left and `node_count == 0` → nothing due. It should pass already; it pins the behaviour.
- [ ] **Step 2:** Comment: the CCA never sent 0, and a TAP with an empty table therefore can't have its window opened by us. Phase 2's first table write is what makes the count non-zero.
- [ ] **Step 3:** Commit with Task 2 or on its own: `test(master): pin that an empty table gets no renewal`

### Task 4: Node Table tag wording for 0x8000 entries

**Files:**
- Modify: `components/tigo_server/web/app.html` (the "not set up" tag and its tooltip)
- Test: `site/test/` (the existing tag test)

- [ ] **Step 1:** Change the tooltip to the proven meaning: "In the TAP's table but hasn't joined yet. Open the discovery window and power it to join it." Keep the tag label unless the test shows it's misleading.
- [ ] **Step 2:** `node --check`, `cd site && npm test`.
- [ ] **Step 3:** Commit: `style(ui): say what a not-set-up optimizer is waiting for`

### Task 5 (owner's call): Watch the TAP's PAN

A CCA writes the TAP's PAN from its own serial ([[backup-cca-repans-tap]]). If a CCA ever reaches the roof bus, every optimizer drops off. A watch would say so directly.

- Read the 0x0D pair at every startup, not only after a re-address. It's a read the CCA sends at every boot.
- Keep bytes 2–3 in the state dir. On a change, warn: "the TAP's network ID changed since the last start: a CCA may have re-PANned it". **Expose a boolean only; never the value or the rest of the blob.**
- Tests: same blob twice → no warning; bytes 2–3 changed → warning; the first read sets the baseline (Review Focus 3); the log line contains no blob bytes.

Don't build it until the owner says yes: it widens a bring-up read to every start, and it stores a value derived from the dead CCA's serial (locally, never displayed).

---

### Task 6: Don't loop on a TAP that refuses polls

When a TAP answers polls with 0x0006 (refused), say so once and back off; don't treat it as a lost TAP and re-probe every few seconds. Test: a mock TAP that refuses 0148 gets one warning and no re-probe loop within 60 s. Commit: `fix(master): a TAP that refuses polls is not a lost TAP`

---

### Task 7: Treat 0x0146 as "that optimizer isn't reachable"

When a text query (tunnel sub 06) gets 0x0146 back from the TAP instead of 0x0B10, end that transaction at once: no retry, no wait for a timeout. Count it per node, and don't let it look like a lost TAP or a second master. Test: a mock TAP that answers sub 06 to node 2 with `0146 00 02` → one "node 2 unreachable" result, no retries, polling unaffected. Commit: `fix(master): a query to an unreachable optimizer ends at the TAP's answer`

### Task 8: Ban the GatewayCfg write, and rename sub 0x2B

The CCA's own UI (Mesh → History, 2026-11-06) names tunnel sub 0x0D `GatewayCfg Req`. With args `00 00` or `00 01` it's a read. With `01 00` and a 36-byte blob it's a **write of the TAP's whole network config** (PAN, channel and what is probably key material). That's how a CCA re-PANs a TAP. Our gate returns `bench_only` for it today; make it `never`. The same UI names sub 0x2B `EraseFlash`, not "Stop Discovery". It's already `never`; rename `SUB_DISCOVERY_STOP` and fix its comment.
- Test: `classify_request` of sub 0x0D with `01 00` + 36 bytes → `never`, in every mode including bench; `00 00`/`00 01` unchanged.
- Commit: `fix(master): never send a TAP's network config write`

---

## Bench sessions (with the owner)

In order. B1 and B5 need the backup CCA on the bench bus; B2–B4 need it unplugged and the Pi as the bench master.

- **B1. Removal, captured.** Two spare optimizers in the bench Tigo EI system, both joined. Remove one in the cloud and record the bus. Expected from the analysis: 0x2B erase, then a 0x29 rewrite of the survivor. Then power-cycle the TAP. If the rewrite set the survivor's spare word back to FFFF, it comes back provisional and needs the window to rejoin. That's the main thing to learn.
- **B2. A long TAP power-off, Pi as master.** Several minutes off. Expect: the table and joins intact, the window closed, and our master renewing it on the first 0-second reading if discovery is enabled. Also confirm on hardware: the bring-up sends 0x000E (not 0x0E02) to the H1.0006, and no STORE because the TAP keeps 1201.
- **B3. An optimizer not in the table, Pi as master.** The bench TAP keeps its one record, so the renewal count is 1. Window open, power an optimizer the TAP has never listed. **If it joins and reports, Phase 2 isn't needed and the feature is Phase 1 only.**
- **B4. Short code check letter.** For each spare, compare the label's short code to the long address the TAP reports; print booleans only. Settles Open question 6.

With the never-used TAP as well (B0 first, then B5):

- **B0 result (2026-11-06 20:19–20:21 EST): passed.** Our master ran on this PC through the FTDI USB-RS485 cable, with the never-used TAP alone on the bus (Gateway G5.26, 2017). Our bring-up found it at 1235, re-addressed it and hopped. After the RESET it was back at 1235, so the master re-addressed it again and the TAP acked the STORE. After a bench power cycle it answered at 1201, with no second fall-back. Found along the way:
  - This TAP refuses the tunnel, 0E02 and polls with 0x0006: it has no table or slot map until a CCA configures it.
  - Our master treats the refused polls as "stopped answering" and loops re-probing every ~4 s. That's reads only, but noisy. Fix: Task 6.
  - 0E02 is refused by G5.26 as well as H1.0006; only the roof's G8.65 is known to take it. No change until there's more evidence.
  - **Repeatable, 20:23–20:29:** a scratchpad script moved the TAP back to 1235 and STOREd it there (SET_ID_BY_MAC to 1235, then the key-only STORE). It held through a RESET and a power cycle. A second B0 run then went exactly like the first, and the TAP kept 1201 through another power cycle. So a stored id can be re-stored, and a TAP storing 1235 behaves like a never-stored one.
  - With both bench TAPs on the bus, the stored one went silent and came back with its window closed, probably restarted by the CCA powering its port. Unconfirmed.
- **B0. Our STORE path on hardware, Pi as master, with the never-used TAP.** It has never run with any CCA, so it should come back at the factory id after a RESET, as the H1.0006 did before the backup CCA stored it. Power it alone on the bench bus with our master. Expected: our bring-up re-addresses it, hops, sees it fall back, sends the key-only STORE at 1201 and logs "stored the id". A power cycle must then bring it back at 1201. Run it before the backup CCA ever sees this TAP, since the CCA stores it on its first bring-up. If it turns out to keep its id without a STORE, or the chance is missed, the fallback is a bench-only one-off script outside the master (its send gate doesn't allow this pair): SET_ID_BY_MAC to 1235, then the key-only STORE there. That needs the owner's OK, and it's unproven that a TAP storing 1235 behaves like one that never stored.
- **B5 result (2026-11-06 20:35–20:55): done, captured.** The CCA moved both TAPs to 1235 with its broadcast, gave the known TAP 1201 and the new one 1202, and hopped each to the next free id (1203, 1204). It sent STORE only to the TAP that failed the check after the RESET. It **reflashed the new TAP from G5.26 to G8.59** over RS485 (about 37 KB in 292 blocks). It erased the new TAP's table, left the optimizer only in the H1.0006's table, and opened the window only there. A text query to the switched-off optimizer got `0x0146 00 02`. Once powered, the optimizer rejoined with the window closed and reported through 1201 within about 2 minutes. Details are in the design doc.
- **B5. Two TAPs, with the backup CCA.** Learn how a CCA addresses two: expected 1201 and 1202, a STORE for each, and how the hop changes when the id one up is taken. That would also explain why the hop exists. Then see which TAP each optimizer joins, and what each TAP's table gets. Our master supports one TAP: it reports a second one found in the scan and sends it nothing, so B5 is CCA-only.

Storing 1202 would not help: the TAP would come back at 1202, where our master never looks. Storing 1235 is what reproduces a fall-back.

## Roof sessions (with the owner)

- **R1. Swap in the remaining replacement optimizers with a raw capture.** The four are 802A–802D; 002D has already joined. Record on the Pi5 (stopping stack 68 needs a go-ahead). Open the window, swap, wait for each join, close. Compare their `Info` and `Mppt` replies with the other 40's, including 002D's. If they match, no corrective write is ever needed.

## After these

- **Phase 1** (Add-optimizer UI): write its implementation plan once B3 and B4 are in. B3 decides whether Phase 1 is the whole feature.
- **Phase 2** (single-record 0x29): only if B3 shows an unlisted optimizer can't join, and only with the owner's explicit approval. Bench first, with a dry run.
- **Housekeeping, any time:** the `device_info:` fix from the PR #100 review; move the capture tools into `linux/scripts/` scrubbed of real values; the CHANGELOG and release pass before merging the branch.
