# Adding and removing optimizers in the TAP's table — design

2026-11-09 · branch `feature/optimizer-table` · **status: draft, for the owner's review.** It builds on Phase 2 of `2026-11-05-optimizer-commissioning-design.md` and adds removal. The owner chose on 2026-11-09 to build both.

## Why

With the CCA gone, nothing can put an optimizer into the TAP's table or take one out. Adding matters for a replacement optimizer the TAP has never listed. Removing matters for a dead or swapped one: it stays in the table, and in the window's counts, for good.

## What a CCA does (from the bench captures and the owner's analysis)

- **Add:** a table read (0x26), then sub 0x29 `01 <MAC> <0x8000 | TUID>`, ack `00`, a read back, then the window (0x2D). The TAP clears 0x8000 when the node joins. First join on the bench: about 10 min.
- **Remove:** there is no single-entry delete. The CCA erases the whole table (sub 0x2B `BA BE`, ack `00`) and writes back what should stay. Each 0x29 holds up to 10 records.
- **A survivor is written back as it was:** bit 15 clear for a joined one, which keeps it joined (2026-11-09 capture, below). Only a new record has bit 15 set. The owner's analysis had a successful write store the spare word as FFFF ("not joined"); whether a survivor written with bit 15 clear stays joined through a TAP power cycle is still to check on the bench.
- **0x29 acks:** 0 done, 1 TUID outside 2..1023, 2 table full, 3 commit failed, 4 TUID in use, 5 MAC listed under another TUID.

## Bench capture, 2026-11-09: a CCA adds a second optimizer

The backup CCA, two TAPs (1201 holding one joined optimizer at 0002, 1202 empty), listen-only recording (`cca-add-delete-20261109-1906.bin`, git-excluded). The owner added a second spare in Tigo EI at about 19:13.

| Time (EST) | TAP | The CCA sent |
|---|---|---|
| 19:14:41 | both | GatewayCfg reads `00 00`, `00 01`; the empty 0x22 pair |
| 19:14:43 | both | a table read, then **EraseFlash** (`BA BE`, ack `00` in under a second) |
| 19:14:44 | 1202 | 0x29 with **one record: the new one at `8003`** |
| 19:14:44 | 1201 | 0x29 with **two records: `0002` (bit 15 clear) and the new one at `8003`** |
| 19:14:45 | both | a read back, then the window: `BA BE 02 03 84 <count> 01 01`, count 1 on 1202 and 2 on 1201 |
| 19:18:17 | 1201 | the new optimizer joined (its record read `0003`); the reads, then `Smrt_........S0000...0000` |

What it settles:
- **A CCA adds by rewriting the whole table,** not by appending: erase, then every record that stays plus the new one. The 11-06 capture hid this because that table was empty.
- **A survivor goes back with bit 15 clear,** listed as joined. It doesn't have to rejoin.
- **The erase doesn't drop a joined optimizer.** 0002 reported at 19:14:50, six seconds after the rewrite. Its only gap, 78 s from 19:13:32, began a minute before the erase, while the CCA was restarting on the new config.
- **The new optimizer goes into every TAP's table,** and joins one of them.
- The window's last byte was `01` here, against `00` on the roof and on 11-06.

**The delete, 19:24 (the owner removed the optimizer at `0003` in Tigo EI):**

| Time (EST) | TAP | The CCA sent |
|---|---|---|
| 19:24:51 | both | GatewayCfg reads; the empty 0x22 pair |
| 19:24:53 | both | a table read: 1201 held `0002` and `0003`, both joined; 1202 held `8003` |
| 19:24:54 | 1201 | **EraseFlash**, then 0x29 with **one record: `0002`, bit 15 clear**, then a read back |
| 19:24:54 | 1202 | **EraseFlash**, and nothing written back |
| 19:24:56 | 1201 | the reads to `0002`, a PeriodicCfg write, the `Smrt_` write; **no window** |

- **A delete is an erase of every TAP that lists the optimizer, then a write of what stays, as it was.** Nothing has to rejoin, and no window is opened.
- `0002` reported from 19:24:57, three seconds after the rewrite. It had been silent since 19:15:58, and `0003` reported only from 19:18:22 to 19:23:42, because the bench has one supply, moved from one optimizer to the other (owner, 2026-11-09). So neither erase dropped a powered, joined optimizer: in the add, `0002` was powered and reported six seconds after the rewrite.

**Our add appends instead** (one record, no erase), as the owner's analysis says a 0x29 does. That keeps the erase off the roof for an add, but no capture shows an append to a table that already holds records. The first bench run checks it, on 1201 with its two records.

## What we build

**One job at a time, in `tigo_master.h` (`TableEditor`), with no I/O, so host tests cover every step.** The master core runs it between polls, as it runs Housekeeping.

**Add (MAC, optional TUID):**
1. Walk the whole table. If the MAC is listed, stop: nothing to write (Phase 1's case; the window does the rest).
2. Pick the TUID: the one asked for if free, else the lowest free one from 2 (the CCA chose 2 on an empty table).
3. Send `0x29 01 <MAC> <0x8000|TUID>`. Any ack but `00` stops the job and reports the code.
4. Walk again. The new record must be there with 0x8000, and every other record unchanged. Otherwise stop and report.
5. Hold the window open (WindowKeeper's renewal with the table's new count) until the record's 0x8000 clears, up to a timeout (30 min, then left to `discovery_auto`).

**Remove (MAC):**
1. Walk the whole table; the MAC must be listed. The survivors are every other record, with their TUIDs.
2. **Save the survivors to the state dir before anything is sent.** If the daemon stops between the erase and the last write, the job resumes from that file at the next start. It's deleted only after a good read back.
3. Send `0x2B BA BE`; ack `00` (the TAP erases, then acks: 16–48 ms on the bench). Walk: the table must read empty. Then wait 0.5 s, the CCA's holdoff (0.7–2.3 s seen from erase to rewrite). Every optimizer on the TAP is unjoined from the erase to the write back (the owner's analysis, `table-erase-rewrite.md`), so that is the whole gap.
4. Write the survivors back, 10 per 0x29, each with its TUID as read (bit 15 clear for a joined one, as the CCA wrote them), in id order. Any ack but `00` stops the job and leaves the file, so a retry resumes.
5. Walk: the table must equal the survivors. Then watch that every survivor that was reporting still reports; one that doesn't within a few minutes gets the window.

**The send gate** gets one new class, `table_write`, allowed only while a job runs and the setting is on, and only in exact shapes:
- sub 0x29: count 1–10, length `1 + 10 × count`, every TUID with bit 15 set and in 2..1023;
- sub 0x2B: exactly `BA BE`.

Everything else stays `never`: CLEAR_ADT, SET_LIST_ENTRIES, a 0x29 with bit 15 clear, 0x2B with other args.

**Settings and guards:**
- `master_table_writes`, off by default. Needs `bus_role: master`.
- **Dry run:** a job can be run with "show the frames" instead of sending them. The page lists each frame it would send, in hex.
- No job while the master is probing, discovering or in fault, or while PV off is held.
- A job stops on any reply that doesn't match, and on a TAP lost mid-job (except for the resumed removal, which waits for it).
- Never log a MAC at info level beyond what the Devices page already shows (`identifier-rule-scope`); fixtures use the 04C05B0000 prefix.

**UI (Devices page, TAP section):** the table as the walks read it (address, MAC, joined or provisional, its panel if placed), **Remove** on each row with a confirm that says every optimizer will rejoin, **Add optimizer** (MAC, optional address), and the running job's step and result. API: `GET /api/table`, `POST /api/table/add`, `POST /api/table/remove`, each with `dry_run`.

## Bench first

Each step on the bench TAP before the roof, with `--capture` on:
1. Add the spare optimizer to an empty table; time the join.
2. With two records (the bench optimizer and a made-up 04C05B0000… MAC that never joins), remove the made-up one. **Watch whether the real one, joined and reporting, keeps reporting after the erase or drops until it rejoins,** and how long the rejoin takes with the window open.
3. Stop the daemon between the erase and the rewrite; check that the restart finishes it.
4. Power-cycle the TAP after the removal; the survivor must stay joined.

## Open (for the owner or the analyst)

- ~~The full MAC~~ **Answered (taptap's `docs/protocol.md`, "Barcodes"):** the label is the whole MAC. `04C05B` is implied; the digit before the dash is the next nibble; the dash stands for zeroes; the hex digits are the rest; the last letter is a CRC-4 (polynomial 0x3, initial 0x2, over all 8 bytes, written with `GHJKLMNPRSTVWXYZ`). All 44 MACs in the roof's table encode to a label and decode back, and the check letter caught every single-digit typo tried (4,620 of 4,620). So Add takes the label and refuses one whose letter doesn't match.
- ~~What a CCA writes back for the survivors~~ **Answered by the 2026-11-09 capture:** each one as it was, bit 15 clear for a joined one.
- ~~Does erasing the table drop nodes that are joined right now~~ **Answered by the same capture:** no; the joined optimizer reported six seconds after the rewrite. Whether a rewritten survivor stays joined through a TAP power cycle is still to check.
- ~~How long the erase takes~~ **Under a second** on both bench TAPs.
