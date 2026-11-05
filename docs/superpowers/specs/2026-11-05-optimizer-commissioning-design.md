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
| 4. Join | The optimizer joins the TAP. Joins don't appear to be gated by discovery. The discovery window lets the master learn about the new node | We already see joins (new-optimizer handling) |
| 5. Configure | The updaters read `Version`, `Info`, `Mppt`, `Smrt`, `Tests`. The Info updater also corrects settings (`Mppt_..1.…`, `Mppt_%X.1`, `Discharge_%02X`) | We send the reads; none of the corrective writes |

Sources: the owner's RE write-ups of CCA 4.0.3-nd (`meshdcd`, `lmudcd`), summarised in memory under *cca-enrollment-static-findings*. Treat the table rewrite and the ID choice as firmware-confirmed. Treat "joins aren't gated" as resting on gateway log text plus what we've seen on the roof.

Rejecting unknown optimizers ("not in SysConfig and Discovery not Promiscuous") is a CCA setting, not a TAP rule. The Pi has no such filter.

## Phase 0 — observe (no new frames)

Each observation below decides how much of what follows is needed.

1. **The roof swap of B11–B14, recorded.** These four (802A–802D) are already in the TAP's table: the CCA wrote them before they were installed. Start a full raw bus capture on the Zero, enable discovery, then swap them in. If they join and report, then an optimizer the CCA already registered needs only the discovery window. Things to record:
   - whether each address drops the 0x8000 bit;
   - whether the Info and Mppt replies match the other 40;
   - which queries run, in what order.
2. **Bench: an unregistered spare optimizer, no CCA.** A spare TAP on a PAN and channel that aren't the roof's (the roof uses channel 24), on its own RS485 adapter, with the Pi as master and discovery enabled. Does the spare join and report even though nothing added it to the TAP's table? **If it does, the feature is Phase 1 only.**
3. **Bench: the backup CCA doing a real add, captured.** The owner registers the spare on Tigo's site for the bench CCA, and the Pi records every byte. This gives us:
   - the exact `SET_LIST_ENTRIES` payloads (the 11-byte entry);
   - the order of the steps;
   - how the ID reaches the optimizer;
   - whether a STORE follows;
   - which corrective writes, if any, a factory-fresh optimizer gets.

   Afterwards, power-cycle the bench TAP and read its table back.

The backup CCA stays on the bench bus only. It re-PANs any TAP it can reach.

## Phase 1 — our "scan it on the website"

- **An "Add optimizer" action in the Topology editor.** Enter the barcode by typing it or by scanning it with the phone camera, using the browser's barcode-reader API where it exists and typing where it doesn't. Place it at the same time: inverter, string, panel.
- **The entry starts as pending.** This builds on the existing `known` flag: listed, left out of counts, no alerts, and cleared by the first reading.
- **Open: does the barcode map directly to the long address the TAP reports?** The fixtures suggest it does. Confirm with one real optimizer before relying on it.
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
6. Whether the barcode maps to the long address.

## Out of scope

- Tigo cloud integration of any kind.
- Removing an optimizer from the TAP's table.
- Changing a TAP's channel or PAN.
- Firmware updates (`Remup`).
