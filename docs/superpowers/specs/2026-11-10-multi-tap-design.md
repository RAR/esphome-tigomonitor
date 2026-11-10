# More than one TAP

2026-11-10. The bus master drives one TAP today. This is what it takes to
drive several, as a CCA does, using only frames our master already sends.
Adopting a TAP's radio settings (the GatewayCfg write) is out of scope: a
TAP comes to us already configured, by a CCA on the bench.

## What the CCA does (the 11-06 captures)

Decoded with the recorder's 10-byte record headers stripped
(`cca-two-taps-20261106-2035.bin`, `cca-ui-session-20261106-2109.bin`).

**Bring-up with two TAPs**, one known (T2, at 1201 before) and one new (T1):

1. Five SET_485_ID broadcasts, ~0.75 s apart: every TAP to 1235.
2. One GET_MAC (with backoff) at 1235. Both TAPs answer it, 70 ms apart.
3. SET_ID_BY_MAC for each, at 1235: T1 → 1202 (new: the next free id), T2 →
   1201 (known: its old id).
4. Five more GET_MACs at 1235 (the scan): nothing answers.
5. GET_MAC_NOW at each new id, in assignment order.
6. The hop, per TAP in that order: SET_ID_BY_MAC to the next id past every
   assigned one (1202 → 1203, 1201 → 1204), and GET_MAC_NOW there.
7. RESET broadcast, ~5 s of quiet, GET_MAC_NOW at each home id.
8. T1, new, had no stored id and didn't answer at 1202. **The whole herd ran
   again** from step 1, and this time T1 got a key-only STORE straight after
   its read in step 5. Then every TAP answered at home.
9. VERSION to each; `000E` only to the H-series TAP.

**Steady state with two TAPs:**
- Polls strictly alternate, one per TAP (309,302 and 309,169 in the UI
  session; a run of two only around other requests).
- Each TAP gets its own discovery reads and renewals (`2E`/`2D`), `0B00`,
  `22`, table walks (`26`) and `0x41`s, including one for the TAP's own
  TUID `0001`.
- Optimizer queries (`06`) go to the TAP the optimizer reports through.

**The owner's analysis of the CCA's firmware** (2026-11-09): detection runs at
start and on a reset (a gateway missing, a config change, an explicit reset),
never on a timer; a known gateway not detected counts a miss, and after 10 it
is removed and its optimizers' gateway assignment dropped; optimizer ids are
unique across the whole system; addresses go up from 1201 skipping 1234 and
1235; a saved MAC → address map gives a known TAP its old address.

## Design

**One `TapLink` per TAP** inside `MasterCore`, holding what today is
per-master but is really per-TAP: its id and MAC, the discovery window
keeper, the poll cursor, housekeeping, queries, the PV switch's frames, the
join watch, the timeout and refusal counts, its version, and its status
counters. Each tick serves the next TAP in turn, so with one TAP every tick
is that TAP's, exactly as now.

**Status:** `MasterStatus.taps` lists each TAP (id, MAC, version, state,
window, polls, timeouts, losses, last reply). The existing top-level fields
stay: counts are summed over the TAPs, state (window, version, PV
confirmation) is the first TAP's. With one TAP nothing in `/api/master`
changes.

**Which TAPs:** the saved map (`taps.json`: MAC → id), or, with none saved,
the one at `gateway_id`. A TAP found answering at its id is polled.

**Adopting (setting `master_adopt_taps`, off by default):**
- Off: as now. A TAP heard at the factory address that isn't one of ours is
  reported (`extra_tap`) and left alone.
- On: such a TAP starts the CCA's herd (above). Every TAP, the working ones
  too, is pulled to 1235 and given its saved id back; a new MAC gets the
  next free one from `gateway_id` up, skipping 1234/1235. The map is saved.
- The owner can also start a herd (System setup), to find a TAP left at some
  other address.

The herd changes the bring-up for one TAP too (five broadcasts, the scan
before the read, a drop-back reruns the herd with a STORE), so with one TAP
it is used only once adopting is on. Off, the single-TAP bring-up stays the
one tested on hardware (2720da4).

**With more than one TAP, re-addressing any of them is a herd**, adopting
or not. The one-TAP bring-up starts with a SET_485_ID broadcast, which
moves every TAP to the factory address; its GET_MAC there then hears them
all and could give the first to answer this TAP's id, and a TAP with no
stored id would stay at the factory address after its RESET. Not adopting,
the herd moves only known TAPs and leaves a MAC it doesn't know at the
factory address, reported as now (`extra_tap`).

**The herd gets a bench capture before it goes near the roof.**

**A TAP that stops answering** (10 requests in a row, as now): with one TAP,
as now (startup mode looks for it again, at its id then the factory
address). With several, the others keep being polled; the lost one is asked
at its id once every `LOST_PROBE_MS` (30 s), and after `LOST_MISSES` (10)
unanswered it is dropped from the polled set (state `removed`) and asked
every 5 min. Its map entry stays, so it gets its id back if it returns.
The CCA removes the entry; keeping it is the one deliberate difference.
In startup mode each of its tries also asks at the factory address once
(GET_MAC_NOW, a bring-up read): a TAP there, after a power cut that lost
its id, starts a herd. A table change aimed at a TAP that isn't answering
is refused rather than left waiting.

**Table changes name a TAP.** An add, removal or replace runs on one TAP:
the one the optimizer reports through, or the owner's choice for an add.
An add's "lowest free id" also skips every id listed on the other TAPs
(ids are system-wide). Each TAP has its own join watch.

**The ESP32** component keeps one TAP: nothing there changes but the
interface it already uses.

## Not in this change

- The GatewayCfg write (never-send).
- SetPgw steering between TAPs. **With more than one TAP no 0x41 is sent
  at all.** Our 0x41 content is copied from a CCA (learned, or built in) and
  names the TAP that CCA chose; the owner's analysis (2026-11-10) found it is
  the CCA's steering message, and the CCA tells each TAP the others' network
  settings, so through another TAP a copied one could move an optimizer onto
  the wrong TAP. A node that gets none keeps its TAP. With one TAP the 0x41s
  go out as before.
- Moving an optimizer between TAPs in System setup.
- The five-pass herd on the roof before a bench capture.
