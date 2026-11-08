# What a CCA does to a TAP

Engineering notes on how a Tigo CCA manages its TAPs (gateways) over RS485, from bring-up to removing an optimizer. The bus master in `components/tigo_master` and its send gate (`classify_request` in `components/tigo_monitor/tigo_master.h`) are built on this.

**Sources:**
- Listen-only bench recordings of a backup CCA (firmware 3.6.0-ct) on 2026-11-06, driving an H1.0006 TAP, a G5.26 TAP (reflashed to G8.59 during the session) and one TS4 optimizer (Mnode H9.0022).
- That CCA's local web UI. Its diagnostic pages name every opcode and decode the frames.
- The owner's static analysis of CCA 4.0.3 (`meshdcd`, `lmudcd`) and of the Mgate H1.0007 image.
- The roof's 24-hour capture under the 4.0.7 CCA, and the Zero's CCA-era cold-start capture.

The recordings and UI pastes hold MACs, PANs and network keys. They live in `~/tigo-protocol-analysis/captures/`, outside any git tree, and nothing here quotes them. The planning record is `docs/superpowers/specs/2026-11-05-optimizer-commissioning-design.md` and `docs/superpowers/plans/2026-11-06-bench-learnings.md`.

## Two namespaces

**Top-level commands** go to a TAP's RS485 id (1201, 1202, …; the factory id is 1235, broadcast is 0000). The reply is the command + 1, with 0x8000 set on the address. The TAP's NAK is 0x0006.

| Cmd | What it is |
|---|---|
| 0x000A | VERSION (text reply) |
| 0x000C | SET_CHANNEL, 2 bytes. Seen as `00 00` (disable, or start of a channel scan) and `20 00` (unknown). Acked with 0x000D |
| 0x000E / 0x0E02 | the bring-up read; H-series TAPs take 0x000E and refuse 0x0E02 |
| 0x0010 | RESET, key only (broadcast) |
| 0x0012 | STORE, key only: commits the TAP's current settings (RS485 id, channel) |
| 0x0014 | SET_485_ID broadcast: key + factory id. Moves **every** TAP to 1235 |
| 0x0038 / 0x003A | GET_MAC with / without reply backoff |
| 0x003C | SET_ID_BY_MAC: key + MAC + new id |
| 0x0070 / 0x0071 | start a channel scan: `<type> 08 07 FF F8 00` (type 01 Active, 00 Energy; mask = 802.15.4 channels 11–26) |
| 0x0072 / 0x0073 | poll for scan results (40-byte reply, zero until done) |
| 0x0146 | the TAP's answer to a text query it couldn't deliver (`00 02`); seen from an H1.0006 when the node was off or reconnecting |
| 0x0148 / 0x0149 | the poll, carrying the queue cursor; the reply holds queued node messages |
| 0x0520 / 0x0522 / 0x0526 | TAP firmware: start, 136-byte data block, list partitions |
| 0x0B00 | a one-byte housekeeping write |
| 0x0B0F / 0x0B10 | the tunnel: `00 00 00 <sub> <seq> <args>` |

**Tunnel subs and queue entries share one numbering.** That's the firmware's own naming table (44 names): the same type byte names a tunnel sub sent to the TAP and an entry queued in a poll reply.

| Sub | Name | Notes |
|---|---|---|
| 06 / 07 | EncapString Req / Resp | `^00…` text to a node. The answer (`!Smrt …`) comes back as a type-07 queue entry, not a tunnel reply |
| 09 | Report NetDiag | once per (re)join: own NWID, parent NWID, MAC, RSSI |
| 0D / 0E | GatewayCfg | `00 00`/`00 01` read; **`01 00` + 36 bytes writes the TAP's whole network config** |
| 13 / 14 | PeriodicCfg Write → Report Combined | per-node record |
| 17 / 18 | PeriodicCfg Req / Resp | `tuid + 1 byte` (`0F`) |
| 1D / 1E | ResetNode | |
| 22 / 23 | SamplingRate | |
| 26 / 27 | GetTable | `startTUID`; reply `firstTUID, count, count × (MAC, TUID)` |
| 29 / 2A | SetTable | `count + count × (MAC, TUID)`, ≤10; ack 0 ok, 1–5 rejections |
| 2B / 2C | EraseFlash | `BA BE`: erases the TAP's whole node table |
| 2D | DiscState Write | `BA BE 02 <limit s, BE> <count> 01 00`: opens the window |
| 2E / 2F | DiscState | read: flag, seconds left, count echo, joined, in table |
| 30 | Report NetDebug | off by default |
| 31 | Report Power | every 2 s, delivered two at a time |
| 40–47 | StarDisc Found / **SetPgw** / Own / Ignore / Kick / Status, Firmware Request / Delivery | **0x41 SetPgw is the per-minute "housekeeping" frame**: sets a node's preferred gateway |
| E1–E6 | SetKnownNetworks, Get / EraseStackTrace | |

## The gateway config block

The 37-byte GatewayCfg reply, laid out per the CCA UI's own decode. It was confirmed by a controlled channel change (23 → 20 moved byte 1 alone, plus the per-write bytes 9–11).

| Byte | Field |
|---|---|
| 0 | status |
| 1 | channel (802.15.4, 11–26) |
| 2–3 | PAN |
| 4–7, 12–13 | slot map (12-2-2-1/28/3 on the bench) |
| 8 | 00 |
| 9–11 | a per-operation value: new on every write, and shared by every TAP one operation writes |
| 14 | SLT (90): the signal level a node must beat to be admitted |
| 15–30 | **the 16-byte network key** |
| 31–34 | Optimizn (00000200) |
| 35 | SLm (60) |
| 36 | 00 |

- **A factory TAP** has PAN 0x1234 and channel 0x14.
- **The roof TAP is on channel 18.**
- **The PAN is chosen by the CCA, not derived from its serial.** The bench CCA wrote a new PAN and a new key on every placement, rediscover and delete. Read it from the TAP; never compute it.
- **Never log or commit this block:** it carries the key.

## Bring-up

1. SET_485_ID broadcast (every TAP → 1235).
2. GET_MAC with backoff, which gives one answer per TAP.
3. SET_ID_BY_MAC to each TAP's id. A known TAP keeps its old id; a new one gets the next free id.
4. **The hop:** each TAP moves to the next free id beyond the assigned ones (one TAP: 1201 → 1202; two: 1201 → 1204, 1202 → 1203), is read there, then a RESET broadcast.
5. Each TAP is read at its home id. **A TAP that came back at 1235 instead fails the check. The CCA restarts the bring-up and this time sends it a key-only STORE before the hop.** A TAP that kept its id never gets a STORE.
6. VERSION, then 0x000E (H-series) or 0x0E02, `0x0B00`, GatewayCfg reads, then polling.

Our master does steps 1–6 for one TAP. Its STORE runs only on that drop-back path, and it worked on hardware twice with a never-used G5.26 (2720da4). A stored id can be stored again: SET_ID_BY_MAC to 1235 plus a key-only STORE returns a TAP to "factory" for its RS485 id.

**A CCA reflashes TAPs.** The 3.6 CCA updated the never-used G5.26 to G8.59 on its first bring-up, with its own updates blocked: 292 × 0x0522 blocks, about 20 s. Its UI can also install a chosen image or erase one, and erasing them all bricks the TAP.

## Node tables, joining, moving and removing

- **Each TAP's table** is up to 1024 slots of MAC + TUID, in flash.
  - Bit 15 of the TUID means provisional: in the table, not yet joined.
  - Joins persist through TAP power loss. The open window doesn't.
- **A known node rejoins on its own,** with the window closed:
  - about 40 s after a TAP RESET;
  - about 2 min after the optimizer itself was powered off for hours.
- **Adding:** SetTable writes the node with bit 15 set; the window is opened; the node joins and the TAP clears the bit. First join on the bench: about 10 min.
- **Moving between TAPs:** EraseFlash on the target, SetTable with bit 15 and the same TUID, open the target's window. The node had moved after about 2½–3 min.
- **Removing:** there is no single-entry delete, even for the CCA. When a window runs out with an entry still provisional, the CCA erases that TAP's table and rewrites what should stay; here, nothing.
- **Disabling a TAP** (`SET_CHANNEL 00 00` + STORE) leaves its table alone. So a node can be listed as joined on two TAPs at once, and only the channel decides which one it talks to.
- **Whitelisting a node onto every TAP** ("undiscovered nodes are checked onto all gateways") re-keys every TAP, erases every table and writes the node provisional to each. The node joins one TAP, and the others are cleared when their windows end.
- **Rediscover Nodes** re-keys one TAP and erases its table.
- **Delete Gateway**, with the TAP still wired, makes the CCA forget it and set it up again as new: re-detect, re-key, erase. It stays in service.

**Discovery policy:** the CCA opens 900 s windows only while an expected node is missing. With every node found, it sits in "Idle Final" with discovery off and checks that it stays off. That matches the roof, where the 4.0.7 CCA's count field was the system's optimizer total, and where a closed window still let all 40 rejoin.

## Reports and timing

- **Power reports carry a slot counter:** top 2 bits = Hslot 0–3, low 14 bits = 5 ms ticks. One report per 2 s bin (400 ticks) on the bench, per 3 s (600 ticks) on the roof; an Hslot is 60 s; the counter wraps every 4 min. `tigo_protocol.h` keeps it as text (`slot_counter`).
- **Reports carry no sequence number.** No byte steps by one from report to report in the bench captures; the CCA UI's "serial" column is the CCA's own count. Loss shows in the slot counter instead: a gap of twice the cadence means one report missed, and 0 a duplicate. `link_stats.h` counts it per optimizer, taking the smallest gap of the day as the cadence. On the bench that was 400 ticks (2 s); on the roof at sunrise, 2026-11-08, it was 600 ticks (3 s) on a 200-tick grid, and about one report in six reached the bus behind a newer one: 600, 1,800 or 3,000 ticks behind. Counting only up to 1,600 ticks behind as late read 20% loss (median of 40) where the slots seen showed 0.9%; anything behind by under half the wrap and heard within 60 s now counts as late, which reads 1.1%.
- **NetDiag (type 09), once per join, 20, 23 or 24 data bytes** (the owner's analysis of the CCA's meshdcd, 2026-11-08): words at 0 and 6 unknown, 2 the node's own NWID (meshdcd drops the report if it isn't the expected node), **4 the parent's NWID**, 8–15 the node's MAC; 20–21 and 22 only in the longer forms. `0001` is the gateway: the bench CCA's mesh tree drew its one optimizer on the TAP, and that optimizer's joins all name `0001`. The CCA's own tree (`netrouting.*.csv`) is this parent per node, with the gateway written as `@<its MAC>`.
  - Roof, 21 joins at sunrise 2026-11-08: own NWID = the bus address every time; 11 name `0001`, 1 names optimizer `0018`, and 9 name NWIDs `0039`–`0091`, which aren't in the TAP's 44-entry table (highest `002D`). One optimizer joined twice through two of those. How meshdcd maps such an NWID to a node (its Traceroute resolves "parent NWID … which is node …") is open; the Mesh tab draws them as a relay not identified yet.
  - Byte 16 is the node's signal at the join: it tracked the RSSI of its next power reports (correlation 0.93, within 4.9 on average). Bytes 20–22 are not the parent, as first guessed here.
  - **Nothing asks for a NetDiag** (owner's analysis, 2026-11-08). Like Power (31) and NetDebug (30), type 09 is an unsolicited report with no request opcode paired with it; it reaches the bus inside the TAP's input queue (0x0149), as Power does. How often a node sends one is set per node by PeriodicCfg Write (13), which carries a period and rate for Power, NetDiag and NetDebug, in that order; a zero slot turns that report off. The CCA leaves NetDiag and NetDebug at zero, so a node sends one NetDiag when it joins and no more: 1 NetDiag to 256 power reports in a CCA dump. Joins still happen in daylight: on the roof at 12:26 and 12:40, 2026-11-08, two optimizers rejoined, one (B5, losing 18.9% of its reports) through optimizer D9 and one through an unidentified NWID. PeriodicCfg Req (17, payload `0F`) reads the table back in 18.
  - So a map that refreshes on demand needs a write: either PeriodicCfg Write with a nonzero NetDiag slot, which changes what every node sends, or a way not yet known. The Pi never sends PeriodicCfg Write (below), so for now it keeps each optimizer's last join across restarts and waits for the next. Reading the table with 17 would show the roof's actual rates; our master has never sent it, so it gets a bench capture first. NetDebug (30) also carries the parent's RSSI, at offset 0x30.
- **The CCA polls each TAP about 44 times a second,** about 22.8 ms apart, alternating between TAPs. Over 98% of replies are empty.
  - TAP turnaround averages 10–13 ms, worst case about 142 ms.
  - Our master polls at a similar rate, one request per tick with a 10 ms sleep. Its reply timeout is 50 ms, so the rare slow reply is retried, and the late one is discarded by `transact`'s request match.
- **Status byte** (e.g. 0x68): the UI decodes it as Over Vout + AC Loss + VMPE Change. Bit order not yet known.

## Channel scan

On a G8.59 or later TAP (the UI says the H1.0006 can't):
1. A RESET, then SamplingRate `01 01 0c` / `01 00 0c`, then `SET_CHANNEL 00 00`.
2. **60 s with no bus traffic at all.**
3. One Active sweep and three Energy sweeps, each started with 0x0070 and polled once a second with 0x0072 until 0x0073 holds data (about 62 s per sweep).
   - **Energy result:** 16 signed dBm bytes, channels 11–26. The UI shows their mean.
   - **Active result:** n × (PAN, channel, RSSI).

Data collection from every TAP stops for the whole scan, about 5½ min. The active sweep didn't find our own TAPs or the roof's, so TAPs that aren't discovering may not answer beacon requests.

## Optimizer settings: `Smrt_`

`^00Smrt_……` writes settings, where `.` means "leave unchanged". The CCA builds it from its settings page.
- **The last field:** Default and Off write `0000`, On leaves it `.`. The UI calls this Discharge Enabled; the CCA's code calls the field BaseOverride. On this TS4 no separate `Discharge_` command was sent, so the CCA appears to fall back to this field when a node doesn't support Discharge.
- **The field after `S`** is AcLoss, the rapid-shutdown-on-AC-loss setting: `0000` in the 4.0.7 and night forms, `.` in the morning form.
- **Our allowlisted `^00Smrt_........S0000...0000` is the CCA's Default.**

The read-back `!Smrt …` has 16 tokens on the bench: token 3 HwVout, 10 the store-done flag (`S`), 11 AcLoss, 15 BaseOverride. The CCA compares them with what it wants and re-sends on a mismatch. The roof answers two ways (sunrise, 2026-11-08, 22 optimizers): 13 tokens with values (17, all identical: `0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01`), or 16 tokens of zeros (5). Token 10 is `S` or `N` in both; the other positions don't line up, so the Pi names only the flag and shows the rest as heard. Two of the zero-form five went `N` → `S` after the Zero's allowlisted `Smrt_`.

## What the Pi must never send

The send gate lists these as never (some are still being moved there; see plan Task 8):
- **Tunnel subs:** EraseFlash, SetTable (until Phase 2 is approved), the GatewayCfg write, ResetNode, PeriodicCfg Write, SetKnownNetworks, EraseStackTrace, 0x12, 0x1B.
- **Top-level commands:** SET_CHANNEL, the channel scan (0x0070/0x0072), every TAP firmware command (0x0520–0x0527), CLEAR_ADT, SET_LIST_ENTRIES, and STORE in any form but the key-only bring-up one.

**Any one of these, on the roof, can silence every optimizer, re-key the network, or brick the TAP.**

## Still open

- What `SET_CHANNEL 20 00` means.
- The status byte's bit order.
- The `Smrt_` fields the parser never reads.
- Whether to turn NetDiag on with PeriodicCfg Write so the mesh map refreshes, and at what rate; left until the Pi takes on more writes.
- Whether a TAP takes a node that isn't in its table (plan session B3). This decides whether we ever need SetTable.
