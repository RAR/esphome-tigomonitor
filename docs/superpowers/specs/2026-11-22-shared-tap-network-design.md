# One network for every TAP: a shared key and known networks (Pi)

Status: design, 2026-11-22. PR 1 of 2. PR 2, the re-sort that lets
optimizers choose their TAP, builds on it and has its own design.

## Why

Add a TAP gives every new TAP a key of its own: `main.cpp` draws 16 random
bytes for each one. The CCA doesn't do that. Its TAPs share one key, each
keeps its own channel and network ID (PAN), and it tells each TAP the other
TAPs' channel, PAN and key with SetKnownNetworks (0xE1). By the analyst's
reading, the shared key is what lets an optimizer decode another TAP's
beacons and join it. An earlier note (2026-11-13, bench) said keys were per
TAP. The roof capture below is newer and replaces it.

So a TAP added by TigoMonitor is a TAP no optimizer can join. Add a TAP, and
Replace a TAP's handover, end with a TAP nobody uses. The roof's third TAP,
Mid Roof (1203, added 2026-11-22), is in that state now. PR 2's re-sort
would erase every TAP's list for nothing until this is fixed.

### What the CCA did (the owner's 2026-11-14 to 11-16 roof capture)

Read by label only. No key or PAN appears here or in any output.

1. The CCA brought up a second TAP (the H1, now 1201). It wrote it the
   existing TAP's PAN and key on another channel (15; the G8 at 1202 was on
   18).
2. It sent 0xE1 to each TAP, carrying the other TAP's channel, PAN and key.
   Each one was the same 24-byte shape: `01 00`, then one 22-byte record of
   `00 00`, the channel (1 byte), `00`, the PAN (2), the key (16). A
   request is `0B0F 00 00 00 E1 <seq> <24 bytes>`. The reply is
   `0B10 … E2` with 9 bytes, echoing the TAP's own channel and PAN. The H1
   sometimes replied with zeros there.
3. In the rediscovery on 11-15 it rewrote both TAPs. Each kept its channel,
   got a new PAN of its own and a new key, the same new key on both. The
   0xE1s went on carrying the other TAP's new network.
4. Through 11-16 it repeated a cycle about 18 times: a GatewayCfg read of
   each TAP, then 0xE1 to each.

So the rule is one key per site, a channel and PAN per TAP, and each TAP
told the others' networks.

## Goals

1. **Add a TAP gives the new TAP the key the other TAPs share.** It still
   picks the TAP's channel and PAN as it does now. The first TAP on a
   system, with no others to share with, gets a new random key as now.
2. **Each TAP is told the others' networks (0xE1).** This happens when a
   TAP's network changes (Add a TAP's radio step, the re-key below, a
   channel change from PR #180), and once when the master starts, after it
   has read each TAP's settings.
3. **A Network key row per TAP in dangerous options** (Settings → Bus). It
   says whether the TAP's key is the same as the other TAPs' or different,
   as words only. Preview is a dry run. Write gives the TAP the shared key
   and keeps its channel, PAN and profile. This is how Mid Roof gets fixed.
4. **A handover of chosen optimizers.** `POST /api/master/taps/handover`
   takes an optional list of optimizers, so one or two can be listed on
   another TAP without the rest. With the existing table remove, that is
   enough for the field test below.

## Non-goals

- The re-sort (PR 2): erasing every list, listing everyone everywhere,
  trimming, and 0x13 PeriodicCfg after moves.
- Re-keying the whole site, as the CCA's rediscovery did.
- The ESP32. Add a TAP and these writes are Pi-only, like the rest of the
  TAP writes.
- A periodic 0xE1. The CCA repeated it every few hours. We send it at start
  and on change (see Decisions).

## Anti-goals

- **A key anywhere outside the daemon's memory and the TAP:** no log line,
  API answer, page, test fixture, commit or chat output. Classification
  words only ("same as the others", "different").
- **Any change to 1201 or 1202's own network.** They are only *told* about
  the others.
- **Any frame shape the CCA didn't send.**
- **Re-keying a TAP that has optimizers without saying they will drop off
  it.**

## Constraints

- 0xE1 moves from never-send to allowed only in the captured shape, behind
  Allow changes to the TAPs. The owner cleared 0xE1 on 2026-11-16 for when a
  need is shown and the shape is the CCA's. This is the need.
- The shared key is read from the other TAPs with a GatewayCfg read
  (`0D 00 00`), which the master already sends at start (read_tap_config).
  If the other TAPs disagree, or none answers, Add a TAP and the re-key
  refuse with "your TAPs don't share a key" or "no other TAP answered".
  They never pick one.
- A G8 saves its radio settings only with the gateway Store. The provision
  job already sends it (saved_by_store).
- The key is never printed. `describe()` already says "key hidden". The new
  "same / different" comes from comparing in the daemon.
- No real PAN, key or MAC in commits, fixtures or tests. The capture check
  runs outside the repo and prints only "identical" or "different".

## Approaches

- **A. One PR for the network fix and the re-sort.** Fewer PRs, but the
  first real test of any part would come with the riskiest part, erasing
  every list on the roof.
- **B. The network first, then the re-sort (picked).** Each piece is proven
  on the roof before the next depends on it, and Replace a TAP needs this
  part anyway. The owner picked B on 2026-11-22.
- Dropped: listing everyone on every TAP without erasing (on 11-14 no
  optimizer moved on its own), and the CCA's slow lower-the-threshold tool
  (unexplored, later).

## Decisions made with the owner

- Option B, this design first.
- A re-key action in dangerous options, because Mid Roof has the wrong key
  now.
- (For PR 2, recorded here.) The re-sort is always the last step of Add a
  TAP and of Replace a TAP, started with a button, never on its own. A
  re-sort at any other time is a button in dangerous options.

## My calls (correct any)

- **"The others" are the TAPs this master has read this run.** A TAP whose
  settings were never read isn't included in anyone's 0xE1, and isn't
  sent one. A lost TAP's last-read network is still sent, so its
  optimizers can still be taken in elsewhere. Forget a TAP re-sends, so
  no TAP is told about a forgotten one.
- **Order:** the new or re-keyed TAP's settings are written and saved
  first, then the 0xE1s go to every TAP.
- **Start and change only, no periodic 0xE1.** Whether a TAP keeps the
  list through a power cut isn't known. The send at start covers a
  restart of the Pi, not of a TAP. A TAP that comes back from a power cut
  (lost, then answering again) is sent its 0xE1 again.
- **No PV off around these writes.** The CCA turns PV off around
  GatewayCfg writes. A new TAP has no optimizers, and a re-keyed TAP's own
  optimizers drop off it either way. The other TAPs' networks don't
  change.
- **The re-key warns but doesn't refuse** on a TAP with optimizers listed.
  The warning is red and says they lose it until they rejoin. It's a
  dangerous option.
- **The add step reads the other TAPs itself** if their settings haven't
  been read yet, rather than depending on read_tap_config.
- **The narrowed handover takes optimizer addresses** (`"optimizers":
  ["0003"]`), as the Bus log and Devices show them.

## Open (the owner's)

- **0xE1 with more than one other TAP.** Every captured 0xE1 had one other
  TAP: `01 00` and one record. The roof now has three TAPs, so each TAP has
  two others, and no capture shows that shape. The lead `01` may be a
  count, but that's a guess, and a guess is a frame the CCA never sent.

## The field test (after merge, the owner's go-ahead, daylight)

1. Install on the Zero.
2. Re-key Mid Roof from dangerous options: a preview, then a write. Then
   the 0xE1s go to all three TAPs.
3. Pick one optimizer the owner expects Mid Roof to hear well. List it on
   Mid Roof with the narrowed handover. Remove it from its TAP with the
   existing table remove. Watch the join in the Bus log.
4. Report what happened: whether it joined, how long it took, and its loss
   after.

If it joins, PR 2 can rely on joins to a TigoMonitor-set-up TAP. If it
doesn't, removing it from its TAP may not be enough to make it leave. That
would point PR 2 at the CCA's way of forcing a rejoin, the site re-key.

## Testing

- **Host (tap_config_test):**
  - A provision given a shared key writes it, with the channel and PAN as
    chosen.
  - The re-key keeps the TAP's channel, PAN and profile and changes only
    the key.
  - Other TAPs that disagree are refused; no other TAP answering is
    refused.
  - The key never appears in a message.
- **Host (master_protocol_test):**
  - The 0xE1 encoder makes the captured layout from a made-up channel, PAN
    and key.
  - The allow list takes exactly that shape and nothing else.
  - The send rules: at start after the config reads, after a network
    change, and never for a TAP whose settings haven't been read.
- **Capture check (outside the repo):** our encoder, fed the network the
  capture's 0xE1 carried, gives the captured bytes. It prints "identical"
  or "different" only.
- **mock_tap + provision-e2e:** the mock answers 0xE1 as the TAPs did and
  logs each without the key. Covers Add a TAP with a second TAP taking the
  first's key, the re-key, and the 0xE1s after both.
- **Site:** the Network key row's words, its preview and write bodies, and
  its warning.
- **Bench:** two G8 bench TAPs, the shared key and 0xE1s, read back.

## Left to the builder

File layout, names, the exact UI copy within the house rules, and the
mock's reply detail.
