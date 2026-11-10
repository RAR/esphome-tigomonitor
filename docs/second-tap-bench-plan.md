# A second TAP: what we'd need to learn on the bench first

The roof's String B loses 2–19% of its reports (2026-11-08) on signals of
54–87, where C and D mostly lose under 1% on 100–240. A TAP near String B
would likely help. Our master can't run one yet, and some of what a CCA does
to bring one up is on our never-send list. This note is the bench plan that
decides what's possible. Nothing in it goes near the roof bus.

## Where we stand

- **The master drives one TAP.** A second TAP that answers at the factory
  address is reported (`Fault::extra_tap`, the Bus page and `/api/health`)
  and left alone, so its optimizers wouldn't be monitored.
- **A CCA writes each TAP's network settings itself.** It sets the PAN from
  its own unit ID and sends a GatewayCfg write. That write is on the Pi's
  never-send list. A new TAP that never gets the roof's PAN and key is on a
  different network, and the roof's optimizers can't join it.
- **Moving an optimizer between TAPs uses frames we never send:** EraseFlash
  on the target, SetTable with the provisional bit, then the target's window
  (bench, 2026-11-06, about 2½–3 min). Whether a TAP takes an optimizer that
  isn't in its table is open (`cca-tap-management.md`, "Still open").
- **A CCA can reflash a TAP** (G5.26 → G8.59 on the bench), and **a backup CCA
  re-PANs every TAP it brings up.** Both are why the backup CCA stays on the
  bench.

## Questions, in order

1. **Does a second TAP on the same PAN and key take optimizers without
   SetTable?** If an optimizer that loses the first TAP joins the second by
   itself once that one's window is open, a second TAP needs no table writes.
   This decides most of the rest.
2. **How does the CCA bring up a second TAP?** The exact frames for two TAPs:
   the order of SET_ID_BY_MAC, the hop to 1204/1203, the RESET, and every
   GatewayCfg write and STORE, with their payload shapes (not the key).
3. **How does the CCA poll two TAPs?** It alternates, about 22.8 ms apart.
   Each TAP has its own queue cursor and discovery window. We'd check whether
   anything else differs, such as the 0x0B00, the 0x41s or the node-table walks.
4. **Where does an optimizer report when both TAPs hear it?** The NetDiag
   parent and the RS485 id its power reports come through, and whether it
   moves on its own when one TAP's signal is better.
5. **What does a TAP do with an optimizer it no longer serves?** Whether the
   table entry ages out, or both TAPs list it, as "Disabling a TAP" suggests.

## Setup

- The bench CCA (4.0.8) as the master, two bench TAPs, and the bench optimizer.
  Two optimizers would answer question 4 better, if a spare can be borrowed.
- The Pi listening (the default role), with `--capture` on, so every frame is
  kept: `~/tigo-protocol-analysis/captures/`, never committed.
- TAPs on their own supplies, a shared ground, and each TAP's channel noted
  from the CCA UI.

## Steps

1. One TAP and the optimizer, as on 2026-11-06. Capture a clean start as the
   baseline.
2. Add the second TAP to the bus with the CCA running. Capture its detection
   and bring-up up to steady polling of both. Note what the CCA UI shows.
3. Power the first TAP off. Time how long until the optimizer reports through
   the second, and whether the CCA sent anything to make it move.
4. Power the first back on. See whether the optimizer moves back, and what
   either TAP's table holds.
5. With the CCA off and the Pi listening, repeat steps 3–4, which only need
   the TAPs' own behaviour. This answers question 1 without the CCA
   moving anything.

## What it would take in our master

Depending on the answers:

- **Polling two TAPs:** a cursor, discovery window and housekeeping per
  gateway, alternating as the CCA does. The mesh map and `/api/mesh` already
  draw several TAPs side by side.
- **Bringing up the second TAP:** the CCA's frames for it, each one checked
  against the never-send list. If it needs the GatewayCfg write, that's a
  rule the owner would have to lift, with the bench capture as the evidence.
- **Moving optimizers:** none, if question 1 says they move by themselves.
  Otherwise SetTable and EraseFlash, which stay off the list until Phase 2.

None of this changes what the Pi sends today.
