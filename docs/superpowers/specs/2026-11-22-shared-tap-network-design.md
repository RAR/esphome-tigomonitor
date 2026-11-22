# One network for every TAP: a shared key and known networks (Pi)

Status: design, 2026-11-22. This is PR 1 of 2. PR 2 is the re-sort that
lets optimizers choose their TAP. It builds on this one and gets its own
design; what we already know for it is at the end.

## Why

Add a TAP gives every new TAP a key of its own: `main.cpp` draws 16 random
bytes for each one. The CCA doesn't. Its TAPs share one key, and each keeps
its own channel and network ID (PAN). With SetKnownNetworks (0xE1) it tells
each TAP the other TAPs' channel, PAN and key.

The analyst confirmed this (2026-11-22). An optimizer can't join a TAP on a
different key, and the CCA sends 0xE1 to every TAP, naming the others. An
earlier note (2026-11-13, bench) said keys were per TAP. The roof capture
replaces it.

So no optimizer can join a TAP that TigoMonitor added. The roof's third TAP,
Mid Roof (1203, added 2026-11-22), is like that now. PR 2's re-sort would
put nobody on it until this is fixed.

### What the CCA did (the owner's 2026-11-14 to 11-16 roof capture)

Keys and PANs are read as labels only. None appears here or in any output.

1. **When it brought up a second TAP** (the H1, now 1201), it gave it the
   existing TAP's PAN and key on another channel: 15, while the G8 at 1202
   was on 18.
2. **Then it sent 0xE1 to each TAP**, carrying the other TAP's channel, PAN
   and key.
3. **At the 11-15 rediscovery it re-keyed both TAPs.** Each kept its
   channel, got a new PAN of its own, and both got the same new key. Each
   network write was bracketed by PV off (`0B00 00`) and PV on (`0B00 01`)
   to both TAPs, about a second apart. Then came 0x2B erases, and 0x2D
   windows ending `01 01`. The 0xE1s then carried the new networks.
4. **Through 11-16 it repeated a cycle about 18 times:** a GatewayCfg read
   of each TAP, then 0xE1 to each.

### 0xE1, from the analyst (static, checked against the capture)

```
request: 0B0F  00 00 00 E1 <seq>  <count> 00 00 00  { <ch> 00 <PAN hi> <PAN lo> <key: 16> } × count
reply:   0B10  00 0E 00 E2 <seq>  <status> <own ch> <own PAN: 2>
```

- `count` is the number of other TAPs, at most 16. Each record is 20 bytes.
  With two TAPs that's 24 bytes of args, as captured. With three it's
  `02 00 00 00` and two records, 44 bytes.
- **Order.** The records come in the CCA's list order, skipping the TAP
  being addressed. Whether that is by id or by when each TAP was added is
  open. We use our own TAP order, the one polling uses.
- **It sets the TAP's whole list**; it doesn't add to it. A TAP that can't
  hold another network answers status 3.
- **The status byte.** 0 is success, 1 an invalid channel, 2 an invalid PAN,
  3 no room. The echo after it is the TAP's own network and isn't checked.
  The H1's `00 00 00 00` is success with an empty echo.
- **A cross-TAP join needs both pieces:** the shared key, and 0xE1 on each
  TAP naming the others. Do both directions.
- **Whether a TAP keeps the list through a power cut is open.** Don't assume
  it does.

## Goals

1. **A new TAP gets the shared key.** Add a TAP gives it the key the other
   TAPs share, and still picks its channel and PAN as now. The first TAP on
   a system, with no others, gets a new random key as now.
2. **Each TAP is told the others' networks (0xE1), in the shape above.** It
   happens:
   - after any TAP's network changes: Add a TAP's radio step, the re-key
     below, or a channel change from #180;
   - once when the master starts, after it has read each TAP's settings;
   - whenever a TAP that was lost answers again.
3. **A Network key row per TAP in dangerous options** (Settings → Bus). It
   says in words whether the TAP's key is the same as the others' or
   different. Preview is a dry run. Write gives the TAP the shared key and
   keeps its channel, PAN and profile. This fixes Mid Roof.

## Non-goals

- The re-sort (PR 2).
- 0x13 PeriodicCfg, which goes with moves (PR 2).
- Moving any optimizer. Taking one off its TAP's list doesn't move it (the
  analyst, below), so PR 1 has no move test.
- Re-keying the whole site.
- The ESP32. TAP writes are Pi-only.
- A timed, periodic 0xE1. We send it on the events above.

## Anti-goals

- A key anywhere outside the daemon's memory and the TAP: no log line, API
  answer, page, fixture, commit or chat output. Only the words "same as the
  others" or "different".
- A change to 1201's or 1202's own network. They are only told about the
  others.
- A frame shape the CCA didn't send.
- Re-keying a TAP that has optimizers without saying they'll drop off it.

## Constraints

- **0xE1 is allowed only in the shape above, behind Allow changes to the
  TAPs.** It moves off the never-send list. The owner cleared it on
  2026-11-16 for a shown need in the CCA's shape, and this is that need.
- **The shared key is read from the other TAPs** with a GatewayCfg read
  (`0D 00 00`), which the master already sends at start. If the other TAPs
  disagree, or none answers, Add a TAP and the re-key refuse with "your
  TAPs don't share a key" or "no other TAP answered". They never pick one.
- **A G8 saves only with the gateway Store.** The provision job already
  sends it.
- **No real PAN, key or MAC in commits, fixtures or tests.** The capture
  check runs outside the repo and prints only "identical" or "different".

## Approaches

- **A. The network fix and the re-sort in one PR.** The first real test of
  any part would come with the riskiest part.
- **B. The network first, then the re-sort (picked by the owner,
  2026-11-22).** Each part is proven on the roof before the next one
  depends on it. Replace a TAP needs this part anyway.
- **Dropped:**
  - Listing everyone on every TAP without a network change: it moves
    nobody.
  - The CCA's slow lower-the-threshold tool: unexplored, so later.

## Decisions made with the owner

- Option B, this design first.
- A re-key in dangerous options, because Mid Roof has the wrong key now.
- (For PR 2) The re-sort is always the last step of Add a TAP and of Replace
  a TAP, started with a button and never on its own. At any other time it
  is a button in dangerous options.

## My calls (correct any)

- **"The others" are the TAPs this master has read this run.**
  - A TAP never read isn't named in anyone's 0xE1 and isn't sent one.
  - A lost TAP's last-read network is still sent.
  - Forget a TAP re-sends, so no TAP is told about one that's gone.
- **Order.** The new or re-keyed TAP's settings are written and saved
  first. Then 0xE1 goes to every TAP.
- **A status other than 0 is a failure, said by name** ("no room for
  another network"). It isn't retried in a loop. It is tried again on the
  next event above.
- **No PV off around PR 1's writes.**
  - The CCA brackets every network write with PV off. Its writes changed
    the networks optimizers were on, and its re-key dips fell on them.
  - A new TAP has no optimizers. Mid Roof has none. 0xE1 changes no TAP's
    own network.
  - The re-key row on a TAP with optimizers warns, in red, that they drop
    off it.
- **The add step and the re-key read the other TAPs themselves** if their
  settings haven't been read yet.
- **More than 16 other TAPs is refused** rather than sent.

## The roof test (after merge, the owner's go-ahead)

1. Install on the Zero. At start, the Bus log shows 0xE1 to 1201 and 1202,
   each naming the other two, and to 1203, all status 0.
2. Re-key Mid Roof from dangerous options: preview, then write. Then 0xE1 to
   all three again, status 0.
3. Each TAP's Network key row reads "same as the others".
4. Nothing should change for the 40 optimizers. Check that none drops
   (Devices, the Bus log's joins) through steps 1 to 3.

## Testing

- **Host (tap_config_test):**
  - A provision given the shared key writes it, with the channel and PAN as
    chosen.
  - The re-key keeps channel, PAN and profile and changes only the key.
  - TAPs that disagree are refused, and so is no other TAP answering.
  - The key never appears in a message.
- **Host (master_protocol_test):**
  - The 0xE1 encoder for one and two other TAPs, from made-up networks.
  - The allow list takes exactly that shape: count 1 to 16, `00 00 00`,
    and 20 × count bytes. Nothing else.
  - The reply's status words.
  - The send rules: at start after the reads, after a change, after a lost
    TAP answers, and never for an unread TAP.
- **Capture check (outside the repo):** our encoder, given the network a
  captured 0xE1 carried, makes the captured bytes. It prints "identical" or
  "different" only.
- **mock_tap + provision-e2e:**
  - The mock answers 0xE1 with status 0 and its own network, and logs each
    request's count and channels, without the key.
  - Covered: Add a TAP with a second TAP taking the first's key, the
    re-key, and the 0xE1s after both.
- **Site:** the Network key row's words, its preview and write bodies, and
  its warning.
- **Bench:** two G8 bench TAPs on a shared key, 0xE1 to each, status 0 read
  from both.

## For PR 2 (the re-sort), learned so far

- **Taking an optimizer off its TAP's list doesn't move it.** The table is
  scheduling, not admission. Only a network change, a new PAN or a new key,
  makes optimizers rejoin. The CCA's re-sort gave each TAP a new PAN, both a
  new shared key, sent 0xE1, erased, listed everyone, and opened 0x2D
  windows ending `01 01` (known networks on) on every TAP.
  - Our allow list only takes the steady renewal ending `01 00`.
  - Its first roof run is also the first real join to a TAP TigoMonitor set
    up.
- **It costs production.** The inverters' PV fell from 4.4 kW to 0 at
  08:45:05. It stayed under 0.8 kW until 08:48:20, with a second zero at
  08:48:05 (the second network write), and was back on the morning curve by
  about 08:53.
  - Both drops fell on the CCA's PV-off brackets, which were also its
    re-keys, so the two causes can't be separated from this run.
  - Run it at low sun.
- **Replace a TAP:** a dead TAP's optimizers lose their network, so they do
  rejoin. A live old TAP must be unplugged first.

## Left to the builder

File layout, names, exact UI copy within the house rules, and the mock's
reply detail.
