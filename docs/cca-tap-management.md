# What a CCA does to a TAP

Engineering notes on how a Tigo CCA manages its TAPs (gateways) over RS485, from bring-up to removing an optimizer. The bus master in `components/tigo_master` and its send gate (`classify_request` in `components/tigo_monitor/tigo_master.h`) are built on this.

**Sources:**
- Listen-only bench recordings of a backup CCA (firmware 3.6.0-ct) on 2026-11-06, driving an H1.0006 TAP, a G5.26 TAP (reflashed to G8.59 during the session) and one TS4 optimizer (Mnode H9.0022).
- That CCA's local web UI. Its diagnostic pages name every opcode and decode the frames.
- The owner's static analysis of CCA 4.0.3 and of a TAP's firmware (H1.0007).
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
| 09 | Report NetDiag | once per (re)join: own NWID, address, parent NWID, MAC, parent's signal |
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

The 37-byte GatewayCfg reply: the 36-byte block the CCA writes, then a 00. Laid out per the CCA UI's own decode and the owner's analysis of the CCA's firmware (2026-11-09); a controlled channel change (23 → 20) moved byte 1 and the map's bytes 9–11.

| Byte | Field |
|---|---|
| 0–1 | channel (802.15.4, 11–26), big-endian; byte 0 is always 00 |
| 2–3 | PAN |
| 4–13 | the map: shown as `12-2-2-1/28/3`, its bytes 5–7 (block 9–11) a hidden value not in that text |
| 14 | SLT (90): the signal level a node must beat to be admitted |
| 15–30 | **the 16-byte network key** |
| 31–34 | Optimizn (00000200) |
| 35 | SLm (60) |
| 36 | 00 |

- **A factory TAP** has PAN 0x1234 and channel 0x14.
- **The roof TAP is on channel 18.**
- **Channel, PAN and key are per TAP**, and each TAP is its own network. Two bench TAPs under one CCA sat on channels 15 and 23 (later 20), with different PANs and keys. The PAN comes from a search for a free one, seeded by the TAP's MAC and the CCA's serial; the key is 16 random bytes (owner's analysis). The bench CCA wrote a new PAN and key on every placement, rediscover and delete. Read them from the TAP; never compute them.
- **The map is sized per TAP from the number of optimizers it carries** (owner's analysis). The roof TAP with 40 reads `14-2-2-1/28/1`; the bench TAPs with one read `12-2-2-1/28/1` (2026-11-09) and `12-2-2-1/28/3` with bytes 9–11 nonzero (2026-11-06). Two TAPs written in one operation can share bytes 9–11 when their maps match.
- **The write** is `0B0F` sub `0D`, args `01 00` + the 36-byte block; the reads are `00 00` and `00 01`, both sent to each TAP. Every write in the 11-06 captures (six) came right after a herd and RESET, between `0B00 00` and `0B00 01` sent to every TAP, and only to the TAPs being configured: adopting a new TAP left the other's block untouched.
- **Never log or commit this block:** it carries the key.

## Bring-up

1. SET_485_ID broadcast (every TAP → 1235).
2. GET_MAC with backoff, which gives one answer per TAP.
3. SET_ID_BY_MAC to each TAP's id. A known TAP keeps its old id; a new one gets the next free id.
4. **The hop:** each TAP moves to the next free id beyond the assigned ones (one TAP: 1201 → 1202; two: 1201 → 1204, 1202 → 1203), is read there, then a RESET broadcast.
5. Each TAP is read at its home id. **A TAP that came back at 1235 instead fails the check. The CCA restarts the bring-up and this time sends it a key-only STORE before the hop.** A TAP that kept its id never gets a STORE.
6. VERSION, then 0x000E (H-series) or 0x0E02, `0x0B00`, GatewayCfg reads, then polling.

Our master does steps 1–6 for one TAP. Its STORE runs only on that drop-back path, and it worked on hardware twice with a never-used G5.26 (2720da4). A stored id can be stored again: SET_ID_BY_MAC to 1235 plus a key-only STORE returns a TAP to "factory" for its RS485 id.

With more than one TAP, the CCA herds them all at once, and our master's `Herd` (`tigo_master.h`) copies it frame for frame, used only with `master_adopt_taps` on. The 11-06 recording of the bench CCA adopting a second TAP (`cca-two-taps-20261106-2035.bin`) shows: five SET_485_ID broadcasts ~0.75 s apart; one GET_MAC at 1235 that both TAPs answer, 70 ms apart; SET_ID_BY_MAC for each at 1235, the known TAP to its old id and the new one to the next free; the five-GET_MAC scan; GET_MAC_NOW at each new id; the hop, each TAP to the next id past every placed one (1202 → 1203, 1201 → 1204); RESET, ~5 s of quiet, GET_MAC_NOW at each home id. The new TAP had no stored id and wasn't back, so the CCA ran the whole herd again and sent that TAP a key-only STORE straight after its read. Then polling alternated strictly between the two, each getting its own window reads, `0B00`, `22`, walks and `0x41`s. The design is `docs/superpowers/specs/2026-11-10-multi-tap-design.md`.

**A CCA reflashes TAPs.** The 3.6 CCA updated the never-used G5.26 to G8.59 on its first bring-up, with its own updates blocked: 292 × 0x0522 blocks, about 20 s. Its UI can also install a chosen image or erase one, and erasing them all bricks the TAP.

**How the CCA finds and numbers TAPs** (the owner's analysis of the 4.0.3 CCA's firmware, 2026-11-09, checked against the 11-06 bring-up capture):
- **It never looks for a TAP at its old address or scans a range.** The SET_485_ID broadcast pulls every TAP on the bus to 1235, whatever address it had, duplicates included, and GET_MAC then separates them by MAC. A used TAP from another system is found the same way as a new one.
- **The herd runs five passes.** A TAP already moved is pulled back to 1235 and moved again, and the CCA logs it as a successful retry. Our bring-up runs one pass.
- **Addresses go up from 1201, skipping 1234 and 1235,** which a TAP treats as broadcast and management addresses. The CCA keeps a saved MAC → address map, so a TAP it has seen before gets its old address back, and a new MAC gets the next free one. That matches the 11-06 bench, where the known TAP kept 1201 and the new one got 1202.
- **A TAP takes 0000, 1234, 4321 and FFFF as broadcast addresses** (the owner's analysis of an H1.0007 TAP's firmware).
- **A used TAP brings its gateway, not its optimizers.** Once the CCA writes the system's channel, PAN and key to it (the GatewayCfg write), the optimizers it served are on a foreign network until each is added again.
- **Baud rate:** detection sets the CCA's port to 38400 without telling the TAPs. A TAP left at 115200 by another system won't hear the herd. We've only ever seen TAPs at 38400.
- **Herding is bus-wide:** every TAP, including working ones, is pulled to 1235 and reassigned, and data collection stops meanwhile.
- **Firmware:** the 4.0.3 analysis found the version read and stored, with no gateway firmware update in the bus daemon it covered. The 3.6 CCA on the 11-06 bench did reflash a TAP (above), so either 4.0.3 dropped it or it lives elsewhere. Treat any CCA as able to reflash a TAP.

Our master now drives every TAP in its saved list (`taps.json`), and with `master_adopt_taps` on, a TAP at 1235 that isn't one of them starts the five-pass herd with the address rules above; the owner can start one too, to find a TAP left at some other address. A TAP it adopts must already be configured by a CCA: the GatewayCfg write stays on the never-send list, so the Pi never gives a TAP its channel, PAN or key.

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
- **NetDiag (type 09), once per join, 20, 23 or 24 data bytes** (the owner's analysis of the CCA's firmware, corrected 2026-11-09). In the CCA's own reading order:
  - 0–1, the node's **NWID**: the mesh's own id for it, separate from its address, and liable to change when it rejoins.
  - 2–3, its address (TUID), the same as the entry header and the TAP's table. The CCA drops a report whose address isn't the expected node's.
  - 4–5, the **parent's NWID**. `0001` is the gateway.
  - 6–7, a status word; 8–15, the node's MAC; 16, the parent's signal; 17, alignment; 18–19, the hyper-slot.
  - 20–21 and 22, the firmware and application versions, in the 23- and 24-byte forms only. The parent is at 4–5 in all three lengths.
- **A parent is a NWID, never an address.** The CCA keeps a NWID → node map, filled from the joins themselves, and looks a parent up in it each time it refreshes its routes. So a parent heard after its child still resolves, and a parent that rejoined under a new NWID leaves its children unresolved until they rejoin. A parent not in the map is drawn as no parent. A parent equal to the node's own NWID is rejected as a self-link. There is no second table, and no command or other report behind it. The CCA's own tree (`netrouting.*.csv`) is this per node, with the gateway written as `@<its MAC>`.
  - Until 2026-11-09 we read 2–3 as the node's NWID. They are its address, so the check "own NWID = bus address", which held on every roof join, was testing the address against itself. Bytes 0–1 went unread, and a parent was matched against addresses, so most relays looked unidentified (9 of 21 joins on 11-08, 17 of 28 on 11-09). The two that did match an address were coincidences: A1 "through itself" and B4 "through C5".
  - In the analyst's 21-join capture, every non-gateway parent equals another join's bytes 0–1, and none equals any address. NWIDs ran `0018`–`0097`, against addresses `0002`–`0029`.
  - Byte 16, the parent's signal, tracked the RSSI of the node's next power reports on the roof (correlation 0.93, within 4.9 on average). Bytes 20–22 are not the parent, as first guessed here.
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

The read-back `!Smrt …` has **one layout for every firmware** (the analysis side, 2026-11-08: the same answer format in the K6, K8 and H9 optimizer firmware, and the CCA's own field table): nine words, the flag, a byte, three words, a byte. Counting `!Smrt` as token 0:

| token | field | unit, raw → real |
|---|---|---|
| 1 | SwVout, the software output-voltage setpoint | V, ×0.05 |
| 2 | OutResis, output resistance | Ω, ×0.01953125 |
| 3 | HwVout, the hardware output-voltage limit | V, ×0.05 |
| 4, 5 | DnPwmTim, UpPwmTim, PWM ramp times | s, ×0.001 |
| 6 | PwmThrsh | raw |
| 7 | OvRecTim, over-voltage recovery | s, ×0.001 |
| 8, 9 | UvinLvLo, UvinLvHi, input under-voltage levels | V, ×0.05 |
| 10 | the flag: `S` stored, `N` not | literal |
| 11 | AcLoss (PwmAcLos), rapid shutdown on AC loss | raw |
| 12 | UclLimit, the upper current limit | A, ×0.0025 |
| 13 | UclThrsh | V, ×0.05 |
| 14 | OvMinPwm | raw |
| 15 | BaseOverride, the CCA UI's "Discharge Enabled" | a choice |

- **A K8's answer is cut at 58 characters** (every one in the roof captures: no `\r`, the CRC straight after), two digits into UclLimit. So the roof's `… S 00 01` ends in a fragment of `0190`, not a setting; UclLimit, UclThrsh, OvMinPwm and BaseOverride never arrive from a K8. Its `!Info` is cut the same way. The CCA expects it: it logs the reply as truncated but still useful, and retries BaseOverride, then gives up on it.
- **An H9's answer is a placeholder** (the owner's analysis of two H9 firmware builds, 3.5.0 and 4.0.3, 2026-11-09; the roof's H9.0018 falls between them). The answer is built from literal zeros for every setpoint, and its flag is a fixed `N`; the only live value is one byte in the AcLoss position, which read `00` on the roof and whose source isn't pinned. A `Smrt_…` write is real: it's parsed field by field (dots skip) and applied, and its reply is the same zero answer with the flag a fixed `S`. Nothing is kept to read back, so a read always gives `N` and zeros. A CCA therefore reads `N`, writes, sees `S` in the reply, reads `N` again, and rewrites all day: it can never confirm a setting on an H9. The H9's live MPPT state is in its `!Mppt` answer instead.
- **The CCA compares four fields** and re-sends `Smrt_` on a mismatch: HwVout, the flag (anything but `S`), AcLoss and BaseOverride.
- The Devices page names tokens 1–9 and 11, shows 10 as Stored/Not stored, and compares an optimizer's whole fields only with others of its firmware family. An answer in the H9's placeholder form shows as "Not reported" whatever its flag, and is never compared. On the roof, the 35 K8s (K8.0120, K8.0114) all agree; the 5 H9.0018s (D6–D10, the same five that keep producing through a mesh PV off) answer zeros. Two of the H9s went `N` → `S` after the Zero's allowlisted `Smrt_`: that `S` was the write's own reply, not a stored setting.
- **The MPPT parameters are the settings that do read back, on every firmware** (the owner's analysis, 2026-11-09). An answer to `^00Mppt` reads `!Mppt 1 0 S M[100 0018 - 3/3] 0 12 40 08 12` on the roof. The bracket is live MPPT state and varies by node and over time; the byte after it is status. The last four bytes are the four parameters a CCA's settings page edits as "Additional MPPT command settings", 5–8:
  - 5, minimum points for a valid PV curve, raw: `12` = 18.
  - 6, minimum volt difference for the PV slope, raw ÷ 640 V: `40` = 0.100 V.
  - 7, minimum PIV activation current, raw: `08` = 8. The CCA's page gives a range of 0.3–1.55 A; the scale isn't known.
  - 8, the hardware under-Vin margin below VMP, raw × 0.5 V: `12` = 9.0 V.

  A CCA writes them with `Mppt_..1.` and four 2-hex fields (`.` leaves one unchanged), then checks the four bytes in the next `!Mppt`. We never send that write. Every optimizer on the roof, K8 and H9 alike, reports `12 40 08 12`. The name-to-byte order is taken from the order of the CCA's settings page. The Devices page shows the four in each optimizer's settings panel, compared with what most optimizers report, H9s included.
- **A CCA never drifts an optimizer's settings** (the owner's analysis, 2026-11-09). HwVout, the four MPPT parameters, StartPWM, Discharge enabled and BaseOverride come from its configuration. When a node reports something else, it writes the configured value back as a correction, never a new target worked out from production, voltage, temperature or history. With "Smrt Auto-Fix Missing Settings" on, it writes the same targets to a node that reports them missing. Across 13 days and three captures, every node's answers were byte-identical: the K8 setpoints, the H9 zeros, and MPPT `12 40 08 12`. So the Pi treats any change to a settings answer (a whole field, not the stored flag), the MPPT parameters or the firmware as news. It lists the change on the Devices page, counts it for Home Assistant for 24 hours, and logs a warning.
  - **AcLoss is the one target a CCA can move by itself.** When it's set to "system determined", the CCA tallies the nodes' current AcLoss states, enforces the majority (a tie goes ON), and shows "Voting in progress" meanwhile. The vote state lives in `/tmp`, so a reboot can run it again, and a changed fleet can change the result. The older CCA generation also elected the Smrt voltage and resistance targets from the nodes' votes unless the user fixed a voltage; the live generation doesn't. Our master sends the CCA's write as captured, with `0000` in the AcLoss slot. That enforces AcLoss off, which matches every node on the roof, and never runs the vote.
- Open: which character of the `^00Smrt_` write maps to which field.

## What the Pi must never send

The send gate lists these as never (some are still being moved there; see plan Task 8):
- **Tunnel subs:** EraseFlash, SetTable (until Phase 2 is approved), the GatewayCfg write, ResetNode, PeriodicCfg Write, SetKnownNetworks, EraseStackTrace, 0x12, 0x1B.
- **Top-level commands:** SET_CHANNEL, the channel scan (0x0070/0x0072), every TAP firmware command (0x0520–0x0527), CLEAR_ADT, SET_LIST_ENTRIES, and STORE in any form but the key-only bring-up one.

**Any one of these, on the roof, can silence every optimizer, re-key the network, or brick the TAP.**

## Still open

- What `SET_CHANNEL 20 00` means.
- The status byte's bit order.
- Which character of the `^00Smrt_` write maps to which field.
- Whether to turn NetDiag on with PeriodicCfg Write so the mesh map refreshes, and at what rate; left until the Pi takes on more writes.
- Whether a TAP takes a node that isn't in its table (plan session B3). This decides whether we ever need SetTable.
