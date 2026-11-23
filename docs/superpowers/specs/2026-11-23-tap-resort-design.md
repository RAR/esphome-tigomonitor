# Let the optimizers choose their TAP: the re-sort (Pi)

Status: design, 2026-11-23. This is PR 2 of 2. PR 1 (#182, on main) gave
every TAP one shared key and told each TAP the others' networks (0xE1).

## Why

A TAP that Add a TAP brings in gets no optimizers. Mid Roof (1203) has had
none since 2026-11-22. An optimizer stays on its TAP for as long as that
TAP's network (PAN and key) stays the same. Taking it off the TAP's list
doesn't move it (the analyst, 2026-11-22).

The CCA moves them by changing every TAP's network. Every optimizer then
has to look again, and it joins the TAP it hears that lists it. That is the
re-sort. On 11-15 it put strings A and B on the attic H1 and C and D on the
G8, and A/B's loss fell from about 21% to about 0%.

## What the CCA did on 11-15 (the owner's capture)

The frames in order, from 08:45 EST. Keys and PANs were read as labels only.

1. A TAP bring-up: the 0x0014 broadcasts, the 0x003C re-addressing, 0x0010.
2. PV off (`0B00 00`) to both TAPs.
3. A GatewayCfg write (36-byte block) to each TAP. Each kept its channel
   (1201 on 15, 1202 on 18) and got a new PAN of its own, and both got one
   new key.
4. PV on (`0B00 01`) to both, about a second after the PV off.
5. The stored read (`0D 00 01`) of each, then 0xE1 to each naming the
   other's new network. Both answered status 0.
6. Twice more: a bring-up, PV on, the stored reads and 0xE1. The first of
   these wrote 1201 again with the same values. That is the second zero in
   the production at 08:48:05.
7. A 0x2B erase of each table, then 0x29 writes of 10 entries each,
   alternating between the TAPs. All 44 entries went on both TAPs, each with
   0x8000 set (listed, not joined).
8. A 900 s window on both, ending `01 01`.

Also seen:

- **No Store (0x0012) went to the G8** anywhere in the sequence.
- **Every window ended `01 01` from the second TAP on.** From its bring-up
  on 11-14 to the end of the capture on 11-16, every 0x2D the CCA sent ended
  `01 01`, on both TAPs: 119 per TAP, with counts 4, 20, 24 and 44. The
  `01 00` we send was the one-TAP CCA's. With `01 01` and the list on, an
  optimizer listed on one TAP only still can't join another: nobody moved
  under the CCA after its trim.

What followed:

- **All 40 rejoined in about 12 minutes,** the last at 08:57, each at its
  own address: A and B on the H1, C and D on the G8.
- **Production fell from 4.4 kW to 0** at 08:45:05. It stayed under 0.8 kW
  until 08:48:20 and was back on the morning curve by about 08:53. The
  analyst puts that on the network change, not on the PV off.
- **When the window closed, the CCA trimmed.** Each TAP kept the optimizers
  that had joined it. The four spares that hadn't joined stayed listed
  (0x8000) on both.

## Goals

1. **A re-sort, the CCA's sequence on every TAP the master runs.**
   1. PV off to every TAP.
   2. Each TAP's network written: its channel kept, a new PAN each, one new
      key for all.
   3. PV on to every TAP.
   4. Each TAP's save, as the provision job does it now.
   5. 0xE1 to every TAP.
   6. Each table erased, and every optimizer listed on every TAP at its own
      address, not joined.
2. **Where it starts.** It is the last step of Add a TAP and of Replace a
   TAP, and a row in dangerous options (Settings → Bus). A preview (dry run)
   comes first. It never starts by itself.
3. **Windows end `01 01`** whenever there is more than one TAP and 0xE1 is
   on: the steady renewals as well as the re-sort's, as the CCA's did.
4. **Trimming.** While a re-sort runs, each time a TAP's window closes, an
   optimizer that has joined one TAP is taken off every other TAP's list.
   One that hasn't joined anywhere stays listed on every TAP.
5. **No time limit (the owner's option C).** The re-sort runs until every
   listed optimizer has joined a TAP.
   - The Bus page shows how many have joined each TAP and names the ones
     still missing.
   - **Stop waiting** ends it. The missing ones stay listed on every TAP.
6. **Put the old networks back (the owner's option B, for safety).** From
   the first write until the re-sort ends, a button writes every TAP's old
   network back the same way: PV off, the writes, PV on, the saves, 0xE1,
   and everyone listed everywhere. Nothing presses it for you.
7. **A restart doesn't end it.** The old networks are on disk before the
   first write. After a restart the put-back button, the waiting and the
   trimming carry on.

## Non-goals

- **0x13 PeriodicCfg (slots).** Optimizers keep the slots they hold. Two
  that land on one TAP with the same slot would show as loss on that TAP;
  we watch for it after the first run.
- **The CCA's bring-up before each write.** Our TAPs are already addressed,
  and re-addressing stays a startup-only step.
- **Choosing which optimizer goes where.** The optimizers choose.
- The ESP32. TAP writes are Pi-only.
- The CCA's slow lower-the-threshold tool.

## Anti-goals

- **A key or PAN in a log line, API answer, page, fixture, commit or chat
  output** (PR 1's rule). The one exception is the old-networks file, which
  the owner allowed on 2026-11-23.
- A re-sort that starts without the owner pressing a button.
- A frame shape the CCA didn't send.
- PV off left on. The re-sort's bracket is the only PV off it sends.

## Constraints

- **Behind Allow changes to the TAPs.**
- **Refused, by name, unless:**
  - there are at least two TAPs;
  - every TAP the master runs answers, has had its settings read, and reads
    "same" for its key;
  - no other change is running;
  - PV off isn't held.
- **The old-networks file:**
  - lives in the state directory, mode 0600, owned by the service account
    (`tigomonitor`);
  - is written and synced before the first write;
  - is deleted when the re-sort ends: every optimizer joined, Stop waiting,
    or a put-back whose writes all succeeded;
  - never goes into a backup, export, support bundle or log.
- **The new networks.** Each PAN is random, differs from the others and
  from every TAP's current one, and is picked the way Add a TAP picks one.
  The key is 16 random bytes, never all zero.
- **The gate.**
  - The window may end `01 01` only in the renewal's shape: BA BE, 02, 900 s,
    a count above 0.
  - `0B00 00` is allowed only inside the re-sort's bracket.
  - Everything else is a shape the gate already allows.
- **G8s save with the Store, as the provision job does now,** although the
  CCA sent none on 11-15. A G8's stored copy doesn't change without it
  (2026-11-13), and Mid Roof's re-key saved that way on 2026-11-22.

## Approaches

- **A. A re-sort job in the master (picked).** One job runs the steps in
  order, as the provision and table jobs do now, and reuses them. The
  waiting and the trimming are rules the master applies whenever it reads
  the tables and the file is there, so they survive a restart.
- **B. The web page drives it a call at a time.** It can't keep the order
  between steps, can't hold the bracket to a second, and stops when the page
  closes or TigoMonitor restarts.
- **Dropped:**
  - Listing everyone without a network change: it moves nobody.
  - A put-back that runs by itself: the owner chose C, with B as a button.

## Decisions made with the owner

- **The re-sort is PR 2.** It is the last step of Add a TAP and of Replace a
  TAP, plus a row in dangerous options, and never automatic (2026-11-22).
- **No time limit (C):** "I don't think the CCA would set anything back"
  (2026-11-23).
- **A put-back button (B), for safety** (2026-11-23).
- **The old networks may be written to disk** (2026-11-23).

## My calls (correct any)

- **The PV bracket copies the CCA's:** about a second, to every TAP.
  Production drops anyway when the networks change.
- **One pass of writes,** not the CCA's second write of 1201.
- **The trim runs at every window close** while a re-sort runs, not only at
  the first.
- **While a re-sort runs, other changes are refused:** table changes,
  provisions, channel changes and re-keys. Stop waiting and the put-back are
  the exceptions.
- **"Every optimizer" means every entry on every TAP's table** when the
  re-sort starts, by MAC, at its own address. The re-sort is refused if two
  TAPs list one address with different MACs, or one MAC at two addresses.
- **A night run is allowed.** The optimizers should join at sunrise. The
  warning says so.
- **The warning:** production drops for a few minutes. On 11-15 it fell to 0
  for about 3 minutes and was back by about 8. Run it at low sun.
- **The put-back is a re-sort to the old networks.** Every optimizer looks
  again, including those already on a new network. Once its writes succeed,
  the file is deleted and the button goes; the waiting goes on as before.
- **`01 01` starts on the roof as soon as PR 2 is installed,** with no
  re-sort. Nobody should move: each optimizer is listed on one TAP only.
- **A step that fails stops the job** and names the TAP. PV on still goes
  out after a PV off. The file stays and the put-back button is offered.
- **The put-back needs every TAP in the file to answer.** Otherwise it is
  refused by name and the file stays.
- **In Replace a TAP, the old TAP is forgotten before the re-sort step.**
  Otherwise the re-sort refuses, because the old TAP doesn't answer.

## The roof test (after merge, the owner's go-ahead)

1. **Install on the Zero.** Every TAP's renewals now end `01 01`. All 40
   keep reporting from the TAP they were on.
2. **At low sun, with the owner:** preview, then re-sort. The Bus log shows,
   in order:
   - PV off;
   - three network writes;
   - PV on;
   - the saves;
   - 0xE1 to each TAP, status 0;
   - each table erased and all 40 listed on each TAP;
   - the windows.
3. **Watch the per-TAP counts** until all 40 have joined. Time it against
   the CCA's 12 minutes. C should go to Mid Roof, which sits behind it.
4. **Production:** compare the dip with 11-15's.
5. **Afterwards:**
   - each TAP lists only its own optimizers;
   - every key reads "same";
   - the old-networks file is gone;
   - the next day's loss per string is compared with 11-17's (A 0.3%,
     B 0.0%, C 4.9%, D 4.1%).

The put-back's first run is on the bench. On the roof it would cost a
second dip.

## Testing

- **Host (master_core_test, table_edit tests, master_protocol_test):**
  - the frame order of the job;
  - every refusal;
  - the new-network rules;
  - the file written before the first frame and deleted at each end;
  - resuming from the file after a restart;
  - the trim rule, and no trim without a re-sort;
  - the `01 01` renewal and its gate shape;
  - `0B00 00` only inside the bracket;
  - the put-back's order;
  - no key or PAN in any message.
- **mock_taps + a new resort-e2e.sh (in a pi.yml shard):**
  - three mock TAPs and a re-sort;
  - the mock clears 0x8000 bits to stand in for joins: a split, the trim
    after it, one that never joins, Stop waiting, the put-back, and a
    restart in the middle.
- **Site:**
  - the row's words and its warning;
  - the preview and start bodies;
  - the waiting display, Stop waiting and the put-back;
  - the step at the end of Add a TAP and of Replace a TAP.
- **Bench (two G8s):** the writes, the saves, the file, the put-back and
  the `01 01` windows. A real optimizer can't join on the bench.

## Left to the builder

File layout, names, the file's name and format, the exact UI copy within
the house rules, how the job splits into steps, and the mock's detail.
