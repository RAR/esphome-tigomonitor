# System setup: replacing, adding and retiring optimizers — design

2026-11-09 · branch `feature/system-setup` (on `feature/optimizer-table`, PR #110) · **status: built (this branch); the replace not yet tried on hardware.** Mockups: the "System setup" boards of the canvas published from this session (private to the owner).

## Why

PR #110 gave the Raspberry Pi the TAP-table operations a CCA has: add an optimizer by its label, remove one, wait for joins. They sit in a panel on the Devices page as two forms with dry-run buttons. The owner found that messy, and it asks the wrong question: an owner doesn't think "write a record to the TAP's table", they think "I'm swapping the dead optimizer at B4". That task also needs the layout updated, which a CCA's cloud does for you and nothing here does.

## What it is

A **System setup** page in the Pi build's navigation, holding:

- **A notice when an optimizer has gone quiet** (silent since a given time in daylight), with **Replace <name>**.
- **Three tasks**, each a step-by-step flow: **Replace an optimizer**, **Add an optimizer**, **Retire an optimizer**. (A second TAP, later.)
- **What the TAP holds**, in one line (listed, joined, listed and never joined), with a link to the full read-only list.

The add/remove panel at the bottom of the Devices page goes; the Devices page keeps a read-only link to the TAP's list.

## Replace, step by step (the mockups)

1. **Which one is coming out.** The layout's optimizers, quiet ones first (string, panel, silent since).
2. **What's going in.** The label, typed or scanned; the check letter is verified as it's typed, with hints for easily confused characters.
3. **Check.** The table is read and the plan shown in plain words, the bus frames folded away. One button: **Replace <name>**. Nothing is sent before it.
4. **Doing it.** Live progress: saved, erased, written back, read back. It finishes on its own if cut off; leaving the page doesn't stop it.
5. **Waiting for it to join.** Time waited, with what to do on the roof meanwhile (it needs PV voltage), and **Stop waiting**. Up to 30 min.
6. **Done.** The new optimizer in the old one's place (string, panel, name), its first reading, its settings as read.

**Add** is steps 2–6 with the place chosen at step 6 (or left unassigned). **Retire** is steps 1, 3 and 4, then out of the layout.

## Decisions

- **A replace is one erase-and-rewrite, as the CCA writes a table:** the old record left out, the new one written provisional **at the old one's address**, every other record written back as read. One gap of under a second (0.78 s for 43 on the roof) rather than a removal and then an add. The new optimizer reports under the address the history, mesh map and loss counts already use. **Bench first:** reusing an address in the same rewrite that drops its old record hasn't been seen on hardware.
- **The layout follows the replace on the Pi, as the CCA's cloud does:** the old label's slot, name and string go to the new label; the old label is kept in the log. Same semantics as the Topology editor's existing **Replace…**, done on the server.
- **An optimizer the owner removed leaves the Devices list** (owner, 2026-11-10): a retired one as soon as the removal succeeds, the old one of a replace once the new one has its place; its history stays. One that drops off the TAP any other way (a dead unit, a CCA) stays, tagged "Not on the TAP" and then "Missing", so a failure isn't hidden.
- **Every change checks first and asks** (the plan in plain words, then one button); no separate dry-run buttons.
- **One change on the bus at a time; joins waited for together** (PR #110's JoinWatch).
- **All of it stays behind `master_table_writes`.** With it off, the page shows what each task would do but its buttons say why they can't run.
- **The Devices page's panel goes**, leaving a read-only link to the TAP's list.

## What's built for it

- **Master core:** a `TableReplace` job (erase, write back the table minus the old record plus the new one at the old id with bit 15 set, read back; the saved-list and resume rules of the removal); the add and removal already exist.
- **Daemon:** the layout move (old label → new label) after the table change; one API per task, each with a check (dry run) that returns the plan in words; the "gone quiet" list.
- **Page:** System setup and the three flows; the Devices panel removed.
- **Tests:** host tests for `TableReplace` (as for the removal, plus the new record at the old id, provisional); table-e2e runs for each flow; site tests for the flows.
- **Bench:** a replace on the bench TAP, then a TAP power cycle, before the roof.

## Out of scope

A second TAP; scanning a label with the phone camera (the Scan button is a later step; typing works now); writing settings to a new optimizer (none has needed one: the roof's 40 and the spare all read the same as the rest).
