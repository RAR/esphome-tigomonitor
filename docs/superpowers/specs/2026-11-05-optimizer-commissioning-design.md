# Adding optimizers without a CCA — design sketch

2026-11-05 · branch `feature/cca-replacement` · **status: sketch, not started.** The plan is ordered so the observations come first; nothing beyond Phase 0 gets built until they're done.

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
| 4. Join | The optimizer joins the TAP, but only while the discovery window is open (measured 2026-11-05, below). The TAP then clears the entry's 0x8000 bit | We already see joins (new-optimizer handling) |
| 5. Configure | The updaters read `Version`, `Info`, `Mppt`, `Smrt`, `Tests`. The Info updater also corrects settings (`Mppt_..1.…`, `Mppt_%X.1`, `Discharge_%02X`) | We send the reads; none of the corrective writes |

Sources: the owner's RE write-ups of CCA 4.0.3-nd (`meshdcd`, `lmudcd`), summarised in memory under *cca-enrollment-static-findings*. Treat the table rewrite and the ID choice as firmware-confirmed. The write-ups' "joins aren't gated, only the discover bit is" came from log strings; the roof measurement below contradicts it for a registered node that has never joined.

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

**Decision (owner, 2026-11-06): add the STORE to our bring-up; documented here, not built yet.** When it's built:
- **Exact captured shape only:** dest = the target address, cmd 0x0012, payload = the key and nothing else.
- **Placement:** sent once per bring-up, after the verify at the target and before the hop.
- **Guards:** a startup-only exception alongside RESET, SET_485_ID and SET_ID_BY_MAC. Every other STORE, including the structured channel/PAN/slot form from the 4.0.3 analysis, stays never-send.
- **Logging:** never log the payload.
- **Test:** on the bench, the TAP answers at its configured address after a RESET *and* after a power cycle, with no re-addressing.

After that burst the 3.6 CCA went back to re-addressing in a loop and never moved on to polling. That loop is still unexplained; a full CCA restart is being captured.

## Phase 1 — our "scan it on the website"

- **An "Add optimizer" action in the Topology editor.** Enter the barcode by typing it or by scanning it with the phone camera, using the browser's barcode-reader API where it exists and typing where it doesn't. Place it at the same time: inverter, string, panel.
- **The entry starts as pending.** This builds on the existing `known` flag: listed, left out of counts, no alerts, and cleared by the first reading.
- **The printed barcode is not the long address.** Tigo labels use a short form, `N-XXXXXC`: a digit, a dash, five hex digits and a trailing letter. Our UI keys optimizers by the 16-character long address from Frame 27.
  - The existing CCA matcher (`match_barcode()`: last 6 chars, 1-char tolerance) already bridges the two forms.
  - That suggests `N` plus the five hex digits are the tail of the MAC, and the letter is a check character.
  - So the Add form has to resolve a scanned or typed short code against the long addresses it hears, and keep the short code as the label.
  - Confirm the mapping, and how the check letter is computed, on the bench TAP, comparing and printing booleans only.
- **Enabling discovery is part of the flow.** The UI offers to enable it while anything is pending and to disable it when nothing is.

## Phase 2 — a commissioning step in the bus master (only if Phase 0 step 2 fails)

A per-optimizer state machine in `components/tigo_master/master_core`, for one barcode at a time:

1. **Choose an ID.** A preferred ID if it's free, otherwise a random one, checked against the TAP's table (the 0x26 paging). Keep it in our own permanent table in the state dir.
2. **Write one table entry**, with bytes copied exactly from the Phase 0 step 3 capture. Add only: **never `CLEAR_ADT`**, and never rewrite the whole table.
3. **Read back** the TAP's node table. The new entry must be present and every other entry unchanged; otherwise stop and report.
4. **Wait for the join** with discovery enabled, with a timeout.
5. **Run the usual reads.** Compare its settings with the fleet's. Send a corrective write only if the settings differ *and* the capture showed the CCA sending that write.
6. **Done** at its first reading. On the bench, power-cycle the TAP once to show the entry survives. That answers whether STORE matters.

UI: commissioning progress on the pending tile or row. A dry-run mode prints each frame it would send, without sending.

## Guardrails

- **A new setting, `master_commissioning`.** Off by default, and it requires `master_optimizer_writes`, which requires `master_queries`.
- **Allowlist by exact captured shape**, not by opcode. `CLEAR_ADT` stays banned outright.
- **STORE (0x0012) stays banned** unless the power-cycle test proves entries are lost without it. Even then it needs the owner's explicit decision: its payload carries a key. Never log STORE or that key.
- **Bench before roof**, every time, including the TAP power cycle. The roof's discovery window and rapid shutdown behaviour must not change.
- **One optimizer at a time.** Stop on any unexpected reply, including one the master doesn't match ("nobody cared").
- **No real barcodes, MACs or IDs** in fixtures, tests or this repo.

## Open questions (all answered by the Phase 0 captures)

1. Does an unregistered optimizer join and report with no table write? This sets the scope.
2. The 11-byte table entry: its fields, and whether the ID is inside it.
3. How the ID reaches the optimizer: through the table entry, or a separate frame.
4. Whether entries survive a TAP restart without STORE.
5. Which corrective writes a factory-fresh optimizer needs, if any.
6. How the printed short code (`N-XXXXXC`) maps to the long address.
   - Partly answered on the bench H1.0006 TAP (2026-11-05): its five hex digits are the last five hex digits of the MAC, and the MAC starts `04C05B`.
   - Still unknown: the leading digit, which is not simply the MAC nibble before those five, and how the trailing letter is computed. Matching on the five hex digits is enough to find a device.

## Out of scope

- Tigo cloud integration of any kind.
- Removing an optimizer from the TAP's table.
- Changing a TAP's channel or PAN.
- Firmware updates (`Remup`).
