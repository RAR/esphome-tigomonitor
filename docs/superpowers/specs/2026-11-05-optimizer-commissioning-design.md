# Adding optimizers without a CCA — design sketch

2026-11-05 · branch `feature/cca-replacement` · **status: Phase 0 observations 1 and 3 done, 2 open; the STORE is built (2720da4) and passed on the bench.** The plan is ordered so the observations come first; nothing beyond Phase 0 gets built until they're done. What's left is planned in `docs/superpowers/plans/2026-11-06-bench-learnings.md`.

## Why

On Tigo's system you register an optimizer by scanning its barcode on Tigo's website. The cloud sends the updated system config to the CCA. The CCA then rewrites the TAP's node table to match, and configures the optimizer once it joins. Our CCA is dead and the Pi is the bus master, so today we can't add an optimizer that the old CCA never registered.

What we want:
- add an optimizer from our own web UI;
- have it join and report;
- never touch Tigo's cloud.

## How Tigo does it, step by step

| Step | Tigo | Ours today |
|---|---|---|
| 1. Register | Scan the barcode on Tigo's site | The Topology editor holds inverters, strings, panels, Replace and the "known" tag |
| 2. Send the config down | Cloud → CCA (sysconfig) | Not needed: the config lives on the Pi |
| 3. Update the TAP | The CCA builds an "ideal table" from sysconfig, compares it with the gateway's table, then rewrites the differences (`SET_LIST_ENTRIES` 0x0182) or wipes and rewrites (`CLEAR_ADT` 0x011E). It assigns each node an ID: preferred, else random, kept in a permanent table | Never sent; unknown territory |
| 4. Join | The optimizer joins the TAP, but only while the discovery window is open (measured 2026-11-05 and 2026-11-06, below). The TAP then clears the entry's 0x8000 bit | We already see joins (new-optimizer handling) |
| 5. Configure | The updaters read `Version`, `Info`, `Mppt`, `Smrt`, `Tests`. The Info updater also corrects settings (`Mppt_..1.…`, `Mppt_%X.1`, `Discharge_%02X`) | We send the reads; none of the corrective writes |

Sources: the owner's RE write-ups of CCA 4.0.3-nd, summarised in memory under *cca-enrollment-static-findings*. Treat the table rewrite and the ID choice as firmware-confirmed. The write-ups' "joins aren't gated, only the discover bit is" came from log strings; the roof measurement below contradicts it for a registered node that has never joined.

Rejecting unknown optimizers ("not in SysConfig and Discovery not Promiscuous") is a CCA setting, not a TAP rule. The Pi has no such filter.

## Phase 0 — observe (no new frames)

Each observation below decides how much of what follows is needed.

1. **The roof swap of B11–B14, recorded.** These four (802A–802D) are already in the TAP's table: the CCA wrote them before they were installed. Start a full raw bus capture on the Zero, enable discovery, then swap them in. If they join and report, then an optimizer the CCA already registered needs only the discovery window. Things to record:
   - whether each address drops the 0x8000 bit;
   - whether the Info and Mppt replies match the other 40;
   - which queries run, in what order.

   **Result (2026-11-05, one of the four, powered on a bench supply at night, no raw capture):**
   - Powered with the window closed, the TAP never passed it on.
   - With the window open, its first reading arrived 26 s later at 002D.
   - The next Frame 27 listed it at 002D, and the 802D entry was gone. So 0x8000 on a table entry means "registered, not yet joined", and the TAP clears it itself on the join.
   - After the window closed again, it kept reporting.

   So for an optimizer already in the TAP's table, commissioning is: open the window, power it, wait for the join, close the window. No frames beyond the ones we already send. Still worth capturing raw for the other three: the join frames, and the Info and Mppt comparison with the other 40.
2. **Bench: an unregistered spare optimizer, no CCA.** A spare TAP on a PAN and channel that aren't the roof's (the roof uses channel 24), on its own RS485 adapter, with the Pi as master and discovery enabled. Does the spare join and report even though nothing added it to the TAP's table? **If it does, the feature is Phase 1 only.**
3. **Bench: the backup CCA doing a real add, captured.** The owner registers the spare on Tigo's site for the bench CCA, and the Pi records every byte. This gives us:
   - the exact `SET_LIST_ENTRIES` payloads (the 11-byte entry);
   - the order of the steps;
   - how the ID reaches the optimizer;
   - whether a STORE follows;
   - which corrective writes, if any, a factory-fresh optimizer gets.

   Afterwards, power-cycle the bench TAP and read its table back.

The backup CCA stays on the bench bus only. It re-PANs any TAP it can reach.

### Bench capture, 2026-11-06: the CCA saves the TAP's address with STORE

The setup: the backup CCA (firmware 3.6.0-ct, on its own new system in Tigo EI) with the spare H1.0006 TAP, recorded read-only from the bench RS485 pair. Payloads were compared against ours as booleans only; the capture stays out of the repo.

The CCA's bring-up writes have the same shapes as ours:
- broadcast SET_485_ID: key + factory address;
- SET_ID_BY_MAC: key + MAC + target;
- broadcast RESET: key only.

It sends one frame we don't. **Right after verifying the TAP at 1201, it sends STORE (0x0012) to 1201 with the 4-byte key as the whole payload; the TAP acks with 0x0013.** The full sequence, all within 50 ms:

1. read at 1201;
2. STORE;
3. SET_ID_BY_MAC to 1202;
4. read at 1202;
5. broadcast RESET.

1.4 s later the TAP answered at **1201**. So STORE commits the RS485 address to the TAP's flash, and the hop then RESET is the CCA checking that the commit took. Our bring-up does the hop and the RESET without the STORE, which is why the bench TAP fell back to the factory address. The fallback in 32ce89b re-addresses it, but only until the next power cycle.

**Decision (owner, 2026-11-06): add the STORE to our bring-up. Built in 2720da4, and it passed on the bench the same evening (below).** As built:
- **Exact captured shape only:** dest = the target address, cmd 0x0012, payload = the key and nothing else.
- **Placement:** only on the drop-back path: when the TAP is back at the factory address after the hop's RESET, the master re-addresses it and then sends the STORE. A TAP that keeps its address (the roof's G8.65) never gets one. This matches what the CCA does with two TAPs (below): it sends STORE only to a TAP that failed the check after the RESET.
- **Guards:** a startup-only exception alongside RESET, SET_485_ID and SET_ID_BY_MAC. Every other STORE, including the structured channel/PAN/slot form from the 4.0.3 analysis, stays never-send.
- **Logging:** never log the payload.
- **Test:** on the bench, the TAP answers at its configured address after a RESET *and* after a power cycle, with no re-addressing.

After that burst the 3.6 CCA went back to re-addressing in a loop and never moved on to polling. After a full restart (10:30), the first hop-and-RESET check passed with no STORE, because the TAP still held 1201 from the earlier STORE. Then the CCA went straight into the usual bring-up and polling. The loop was probably the failing check; the gateway was not missing from the cloud config.

### Bench capture, 2026-11-06: a CCA adds an unknown optimizer

Steps the owner took: they added one of the spare optimizers to the bench system in Tigo EI, then powered it on a bench supply. The cloud pushed the config at 10:43:21 (seen on the CCA's HTTPS through a MITM). Fourteen seconds later the CCA restarted its bus daemon and ran the frames below.

Tunnel arguments are shown after the seq byte; `<MAC>` is the optimizer's 8-byte long address.

| # | CCA sends | TAP answers | Reading |
|---|---|---|---|
| 1 | 0x26 table read | empty | |
| 2 | sub 0x2B `BA BE` | `00` | erases the table (a real flash erase, per the H1.0007 analysis) |
| 3 | 0x26 | empty | |
| 4 | sub 0x22 `00 00` | `0B` | |
| 5 | sub 0x22 `01 00 02` | `02` | something for node 2 |
| 6 | **sub 0x29 `01 <MAC> 80 02`** | `00` | **the table write**: count, MAC, node ID with the 0x8000 "registered, not joined" bit |
| 7 | sub 0x22 `00 01` | `02` | |
| 8 | 0x26 | `00 01 <MAC> 80 02` | the entry is in |
| 9 | **sub 0x2D `BA BE 02 03 84 00 01 01`** | window open | 0x384 = 900 s; `00 01` is the number of optimizers the CCA expects (44 on the roof). The TAP stores and echoes it but never compares it: the window closes on time only |
| — | 0x2E every 20 s, 0x41 every minute | | waiting |
| 10 | (join at 10:53:52, 10 min later) | entry now `<MAC> 00 02`; joined count 1 | the TAP clears 0x8000 itself |
| 11 | `^00Version`, `^00Info`, `^00Smrt`, `^00Cal`, `^00Mppt` | | reads, starting within 1 s of the join |
| 12 | `^00Smrt_........S....0000` | | a settings write; **not the roof's `^00Smrt_........S0000...0000`** |
| 13 | sub 0x17 `00 02 0F` | ack | resent after a TAP restart |
| 14 | sub 0x13 `00 02` + 22 bytes | ack | looks like a config block |

The CCA chose node ID 2, not a random one. Its cloud view afterwards: 44.9 V in, 0 A, RSSI 205.

**TAP power cycle (10:57:59, a quick unplug).** The TAP came back at 1201 with the entry `<MAC> 00 02` intact, and its first poll reply carried the optimizer's data. The CCA did no re-addressing; it re-read the window and the table and resent sub 0x17. Both the STORE'd address and the table entry survive a power cycle. A longer TAP power-off is still to test. A long *optimizer* power-off was tested that evening (below): it rejoined with the window closed.

**What this means for Phase 2.** Steps 1–6 are now known shapes:
- the table write is sub 0x29, not `SET_LIST_ENTRIES` 0x0182, on this firmware;
- the window renewal carries the expected count.

Open:
- the meaning of 0x22's extra byte, of 0x17 and of 0x13;
- `Cal`;
- which of the post-join writes a 4.0.7-configured optimizer actually needs. The roof-swap capture with the Pi as master answers that.

### Bench, 2026-11-06 evening: our STORE on a new TAP, then a CCA with two TAPs

Times in this section are UTC (the recording machine's clock); subtract 5 h for Eastern.

**Our bring-up on a never-used TAP (20:19–20:29).** The master ran on a PC through an FTDI USB-RS485 cable, with no Pi involved. On the bus was a TAP no CCA had ever touched (Gateway G5.26, 2017), alone and on bench power.
- **First run:** found at 1235, re-addressed, hopped. After the RESET it was back at 1235. The master re-addressed it again and the TAP acked the STORE. After a power cycle it answered at 1201.
- **The stored address can be changed back.** A one-off bench script (SET_ID_BY_MAC to 1235, then the key-only STORE there) put the TAP back at 1235, and it held through a RESET and a power cycle. A second run of our bring-up went exactly like the first, and the TAP again kept 1201 through a power cycle. So STORE can be re-run, and a TAP that has stored 1235 behaves like one that never stored anything.
- **Until a CCA configures it, this TAP refuses the tunnel, 0E02 and polls (reply 0x0006).** Our master reads the refused polls as a lost TAP and re-probes every few seconds; the plan has the fix.

**The backup CCA with two TAPs (20:35–20:55).** The H1.0006 (stored at 1201, holding the bench optimizer) and the G5.26 (stored back at 1235) shared the bus with the CCA. The owner set the bench system in Tigo EI to two TAPs. The CCA had pulled that config by 20:39:09, and its first frame came at 20:39:15.

1. **The SET_485_ID broadcast moves every TAP to 1235,** including one that has stored 1201. GET_MAC with backoff then gets one answer per TAP.
2. **It assigns ids by MAC:** the TAP it already knew got 1201, and the new one got 1202, even though the new one answered first.
3. **The hop goes to the next free ids:** 1202 → 1203 and 1201 → 1204, then a RESET broadcast and a read at each home id. With one TAP, "one up" and "next free" are the same thing, which is why our single-TAP hop matches the CCA.
4. **STORE only after a failed check.** In the first round the G5.26 came back at 1235 (its stored id), not 1202. The CCA started over and this time sent STORE to 1202 before the hop. The H1.0006 kept 1201 and never got a STORE. This is the gate on STORE that the morning capture couldn't explain, and it's what our drop-back path already does.
5. **The CCA reflashed the new TAP.** After a version read (G5.26), it sent 0x0520, then 292 × 0x0522 data blocks of 136 bytes (about 37 KB) with 0x0526 reads between, then a RESET to 1202. That took 20:39:47 to about 20:40:05. Afterwards the TAP reported `Mgate Version G8.59` (2020). The CCA itself was blocked from updating, but it still updated the TAP. **Any CCA that reaches a bus can silently reflash its TAPs,** which is one more reason to keep it off the roof bus.
6. **Each TAP gets its own opcodes:** 0x000E (empty) to the H-series, version and 0x0D reads to both.
7. **Tables are per TAP.** The H1.0006 kept its one joined entry. The CCA erased the new TAP's table (sub 0x2B, 20:40:57) and wrote nothing to it. It opened the window only on 1201 (sub 0x2D, 900 s). That window read closed 23 s later (20:40:58), for a reason we don't know.
8. **New reply: 0x0146, payload `00 02`.** The H1.0006 sends it instead of the tunnel reply when the CCA sends a text query (sub 06) to an optimizer it can't reach. The bench optimizer was off at the time, and the replies stopped once it was powered. Meaning "node unreachable": very likely, not proven.
9. **An optimizer power-off keeps its join.** The bench optimizer had been off for hours. Powered at about 20:52, it was reporting through 1201 by 20:53:48, with the window closed. The second TAP never heard from it. The CCA sent it no text queries or writes after it came back.

Capture: `~/tigo-protocol-analysis/captures/cca-two-taps-20261106-2035.bin` (git-excluded, never committed; it holds MACs and the key).

### Bench, 2026-11-06 night: the CCA's local UI, driven by hand

Times in this section are UTC (the recording machine's clock); subtract 5 h for Eastern.

The owner worked through the backup CCA's local web UI while we recorded the bus listen-only (21:09–23:20). The UI's diagnostic pages name the opcodes and decode the frames, so this section uses its names. Capture: `~/tigo-protocol-analysis/captures/cca-ui-session-20261106-2109.bin`. The owner's pastes of the UI pages are in `cca-ui-mesh-history-20261106-2110.txt` next to it. Both are git-excluded; they hold MACs, PANs and network keys.

**The CCA's names for the tunnel subs:** 0D/0E GatewayCfg, 22/23 SamplingRate, 26/27 GetTable, 2B/2C **EraseFlash**, 2D DiscState Write, 2E/2F DiscState, 06 EncapString, 17 PeriodicCfg. Optimizer reports: 31 Power (every 2 s, delivered in pairs), 09 NetDiag (once per join), NetDebug (off). Text replies (`!Smrt …`) come back inside the poll replies as queue type 07, not as tunnel replies.

**GatewayCfg (0x0D) is also a write.** With args `01 00` plus the 36-byte block, it sets a TAP's whole network config. From the UI's own decode of the block:

| Bytes | Field |
|---|---|
| 0 | status (00) |
| 1 | channel (802.15.4, 11–26; the bench TAPs used 15, 20 and 23) |
| 2–3 | PAN |
| 4–7, 12–13 | slot map (12-2-2-1/28/3) |
| 9–11 | change on every write; identical on both TAPs when one operation re-keyed both (22:13), so a per-operation value the CCA picks, not a checksum |
| 14 | SLT (90), the signal-level threshold for admitting a node |
| 15–30 | **16-byte network key** (matches the UI's "Security Key" column byte for byte) |
| 31–34 | Optimizn 00000200 |
| 35 | SLm (60) |

A fresh TAP's factory block has PAN 0x1234 and channel 0x14. **The PAN is not a fixed function of the CCA serial:** the CCA chose new ones on several rewrites tonight. Only one nibble of it changed between rewrites, so the earlier serial rule describes only the PAN it set first.

**What each UI action sent:**

| Action | On the bus |
|---|---|
| Disable gateway (1201) | `SET_CHANNEL 0x000C 00 00` + key-only STORE to 1201. Then the CCA moved the optimizer: EraseFlash on 1202, sub 0x29 writing it as `8002` (same TUID), window on 1202. Readings returned via 1202 ~3 min later |
| Re-enable | one `SET_CHANNEL 20 00`, no STORE. 1201 had kept the optimizer in its table as joined, so it was listed joined on both TAPs |
| Change channel (1202, 23 → 20) | a GatewayCfg write (channel byte + bytes 9–10), no STORE, RESET; EraseFlash on 1202 and a window on 1201. The optimizer went back to 1201 |
| Node Placement: A1 on both TAPs | GatewayCfg writes to both TAPs (new PAN and key each), RESET, EraseFlash + sub 0x29 `8002` on both, windows on both. A1 joined 1201 in ~45 s. **When 1202's window ran out, the CCA erased 1202's table and wrote nothing back** |
| Rediscover Nodes (1202) | a re-key of 1202 (new PAN and key), two RESETs, EraseFlash |
| Delete Gateway (1202, still on the bus) | the TAP is rediscovered as new: re-detect, re-key (both TAPs), EraseFlash. It stays in service |
| Channel Scan (1202, G8.59+ only) | RESET, SamplingRate `01 01 0c`/`01 00 0c`, `SET_CHANNEL 00 00`, **60 s with no bus traffic at all**. Then **0x0070** `<type> 08 07 FF F8 00` starts a sweep (type 01 Active, 00 Energy; mask = channels 11–26), polled with **0x0072** → **0x0073** (40 bytes). One active sweep, then three energy sweeps, ~62 s each. Energy result = 16 signed dBm bytes (the UI shows their rounded mean, +256). Active result = n × (PAN, channel, RSSI) |
| Firmware page | 0x0526 lists the TAP's firmware partitions. The page can also install a chosen image or erase one. Not used |
| Discharge Enabled | rides in `Smrt_` field 14: Default and Off write `0000`, On leaves it `.` |

**What this settles:**
- **Removal (B1):** a CCA removes an optimizer from a TAP by erasing the whole table and rewriting what should stay. There is no single-entry delete, even for the CCA.
- **Moving between TAPs:** erase, rewrite as provisional, open the window on the target. That's our Phase 2 sequence, done by the CCA itself.
- **When the CCA opens windows:** only while an expected node is missing. Once every node is discovered, it sits in "Idle Final" with discovery off.
- **STORE commits a TAP's current settings** (RS485 id, channel), not only the address.
- **Our `Smrt_` form is the CCA's Default.** The 3.6 and 4.0.7 shapes differ by settings (field 14 Discharge, field 10 unmapped), not by firmware.
- **Timing:** power reports carry a slot counter (top 2 bits = Hslot 0–3, low 14 = 5 ms ticks; a 2 s bin = 400 ticks, an Hslot = 60 s). The CCA polls each TAP ~44 times a second; >98% of polls are empty; TAP turnaround averages 10–13 ms with spikes to ~140 ms.

Subs 0x2B and 0x29, and 0x22 with data after the node ID, stay on the never-send list until the owner decides otherwise.

## Phase 1 — our "scan it on the website"

- **An "Add optimizer" action in the Topology editor.** Enter the barcode by typing it or by scanning it with the phone camera, using the browser's barcode-reader API where it exists and typing where it doesn't. Place it at the same time: inverter, string, panel.
- **The entry starts as pending.** This builds on the existing `known` flag: listed, left out of counts, no alerts, and cleared by the first reading.
- **The printed barcode is not the long address.** Tigo labels use a short form, `N-XXXXXC`: a digit, a dash, five hex digits and a trailing letter. Our UI keys optimizers by the 16-character long address from Frame 27.
  - The existing CCA matcher (`match_barcode()`: last 6 chars, 1-char tolerance) already bridges the two forms.
  - That suggests `N` plus the five hex digits are the tail of the MAC, and the letter is a check character.
  - So the Add form has to resolve a scanned or typed short code against the long addresses it hears, and keep the short code as the label.
  - Confirm the mapping, and how the check letter is computed, on the bench TAP, comparing and printing booleans only.
- **Enabling discovery is part of the flow.** The UI offers to enable it while anything is pending and to disable it when nothing is.

## Phase 2 — adding an optimizer the TAP has never seen

Only for an optimizer that isn't in the TAP's table. One already listed (with the 0x8000 bit, like the roof's spares) needs Phase 1 alone. The frame shapes come from the 2026-11-06 bench capture; the TAP's behaviour comes from the owner's analysis of the Mgate H1.0007 image, one build newer than the bench H1.0006.

**How the TAP stores the table.**
- The table is 1024 slots of `MAC(8) + TUID(2) + FFFF`, in non-volatile storage, filled first-fit.
- Two RAM bitmaps sit beside it: "in table" and "joined". A read sets 0x8000 on any TUID that is in the table but not joined.
- Sub 0x29 *adds* records; it never replaces by ID. The TAP acks only after the record is committed to storage.

The 0x29 ack byte:

| Byte | Meaning |
|---|---|
| 0 | done |
| 1 | TUID outside 2..1023 |
| 2 | table full |
| 3 | commit failed |
| 4 | TUID already in use |
| 5 | MAC already listed under another TUID |

The steps, one optimizer at a time:

1. **Read the whole table** with 0x26, which we already send. Stop if the MAC is already in it: that's Phase 1's case.
2. **Choose a TUID** that isn't in the table, in 2..1023. Keep it in our own permanent table in the state dir.
3. **Write one record**, `0x29 01 <MAC> <0x8000 | TUID>`.
   - **Always with bit 15 set.** That reserves the ID without marking the optimizer joined. A write with bit 15 clear marks it joined immediately, which the CCA never does.
   - Any ack other than 0 stops the process. Code 5 is reported as an identity conflict.
4. **Read back** with 0x26. The new record must be present with 0x8000 set, and every other record unchanged.
5. **Open the window** with the 0x2D renewal we already send, and wait for the join (the 0x8000 bit clears) with a timeout. Then close the window.
6. **Run the usual reads.** We send none of the CCA's post-join writes: Smrt_, 0x13, 0x17. The roof's 40 have reported for weeks without them.

UI: commissioning progress on the pending tile or row. A dry-run mode prints each frame it would send, without sending.

Still open:
- the TUID the CCA chose (2) doesn't match 4.0.3's preferred-then-random allocator;

**Answered: a TAP power cycle keeps every join.** Each slot's spare word holds a persistent "joined" bit: FFFF when written, FFFE once joined. At boot the TAP rebuilds both bitmaps from the table. The bench read joined = 1 about a second after power-on, with no window. A reboot loses only the open window.

**One consequence for step 3:** any successful 0x29 write stores the spare word as FFFF, so rewriting an already-joined record silently turns it back into a provisional one at the next boot. Step 1's "stop if the MAC is already listed" is what prevents that, and it must never be skipped.

## Guardrails

- **A new setting, `master_commissioning`.** Off by default, and it requires `master_optimizer_writes`, which requires `master_queries`.
- **Allowlist by exact captured shape**, not by opcode. Only a single-record 0x29 with bit 15 set is ever allowed. `CLEAR_ADT` and sub 0x2B, the table erase (unrecoverable, and the TAP's flash sectors really are wiped), stay banned outright.
- **STORE (0x0012):** only the key-only form, only on the bring-up's drop-back path (2720da4). Every other form stays banned. Never log STORE or the key.
- **Bench before roof**, every time, including the TAP power cycle. The roof's discovery window and rapid shutdown behaviour must not change.
- **One optimizer at a time.** Stop on any unexpected reply, including one the master doesn't match ("nobody cared").
- **No real barcodes, MACs or IDs** in fixtures, tests or this repo.

## Open questions (all answered by the Phase 0 captures)

1. Does an unregistered optimizer join and report with no table write? This sets the scope. **Still open** (plan session B3).
2. ~~The 11-byte table entry~~ **Answered:** sub 0x29's record is count + MAC + TUID, with bit 15 meaning provisional.
3. ~~How the ID reaches the optimizer~~ **Answered:** through the table record; the optimizer takes it when it joins.
4. ~~Whether entries survive a TAP restart~~ **Answered:** the table and the joined flags are in flash and survive. STORE is about the RS485 address, not the table.
5. Which corrective writes a factory-fresh optimizer needs, if any. **So far none:** 002D reported accurately without any, and the roof's 40 have run for weeks without 0x13 or 0x17. The roof swap's Info/Mppt comparison is the last check.
6. ~~How the printed short code maps to the long address~~ **Answered 2026-11-09** by taptap's protocol notes ("Barcodes"): it is the whole MAC with `04C05B` implied, the dash standing for zeroes and a CRC-4 check letter; it round-trips on all 44 roof MACs. See `2026-11-09-optimizer-table-edits-design.md`.
   - Partly answered on the bench H1.0006 TAP (2026-11-05): its five hex digits are the last five hex digits of the MAC, and the MAC starts `04C05B`.
   - Still unknown: the leading digit, which is not simply the MAC nibble before those five, and how the trailing letter is computed. Matching on the five hex digits is enough to find a device.

## Out of scope

- Tigo cloud integration of any kind.
- Removing an optimizer from the TAP's table. The TAP has no single-entry delete; removal would mean an erase (sub 0x2B) and a rewrite, which we ban.
- Changing a TAP's channel or PAN.
- Firmware updates (`Remup`), and TAP firmware updates (0x0520/0x0522/0x0526, seen on the bench 2026-11-06). Never sent.
