# Standby coordination: master and standby talk to each other

Branch: `feature/cca-replacement`. Status: design, not built. Builds on
[the standby failover design](2026-10-29-standby-failover-design.md), whose
"Later" list has the first half of this ("Planned handoff over the LAN").

## Why

Standby failover works on silence alone: a standby takes the bus once it has
heard no master request for `standby_takeover_s` (3 s by default). That stays
the rule. Two gaps are left that only a side channel can close:

1. **A planned stop still costs the takeover time.** Installing a build on
   the active master (dpkg, or a Docker redeploy) stops it, and the bus is
   silent for 3 s plus up to 0.5 s of jitter before the standby speaks. The
   lease absorbs that easily, but it is a gap in telemetry on every update,
   and updates are frequent.
2. **A deaf standby steals the bus.** If the standby's receiver fails (a
   loose A/B wire, a dead transceiver, a wedged UART), it hears silence and
   takes over. The running master hears the standby's requests and, being a
   standby itself, yields to it. Now the device running the bus is the one
   that can't hear the replies. Today nothing stops this; `second_master`
   and `no_reply` only report it afterwards.

## Goals

1. **A planned stop hands the bus over in well under a second**, with no
   action from the owner: stopping the service does it.
2. **A standby that hears nothing while its peer is demonstrably running
   the bus does not take over,** and says why on `/api/health` and MQTT.
3. **Silence on the bus remains the authority.** With the side channel down,
   lost or lying, the pair behaves exactly as it does today.
4. **Nothing new on the Tigo bus.** The coordination messages never touch
   RS485; the CCA replacement safety rules are unaffected.

## Non-goals

- Electing a master over the side channel. The different takeover times
  already break ties, and the bus is the only place that proves a master
  can reach the TAP.
- Sharing state for a warm takeover (lease, cursor, 0x41s). The failover
  design found all of it is learned by listening or re-read in the first
  exchange; nothing measured says it's worth a protocol.
- More than two devices.
- Fencing (cutting a jammed master's transceiver). Still under Later.

## Anti-goals

- **A side channel that can keep the bus dead.** A veto must expire on its
  own the moment the peer stops proving itself; no message can hold off a
  takeover for longer than a message's freshness window.
- **A side channel that can start a fight.** A handoff message only lets a
  standby skip its wait; if the old master is in fact still talking, the
  existing `second_master` yield sorts it out, as it does today.

## How it works

### The message

Each device in `standby` role with a peer configured sends one small status
message once a second, and at once on any state change:

| Field | Meaning |
|---|---|
| `v` | 1 |
| `device` | its `device_id` |
| `state` | `standing_by`, `taking_over`, `active`, `released` |
| `seq` | increments every message; a receiver drops anything not newer |
| `reply_ms_ago` | active: since the TAP last answered it (from `last_reply_unix`; -1: never) |
| `heard_ms_ago` | standing by: since it last heard a master request |

A message is **fresh** for 2.5 s after it arrives (two missed sends and a
margin). A stale peer counts as no peer.

### Handoff on a planned stop

1. The active device gets SIGTERM (systemd stop, dpkg upgrade, Docker stop),
   or `POST /api/master/handoff`.
2. If its peer is fresh, `standing_by`, and hearing the bus
   (`heard_ms_ago` < 1000), it finishes the exchange in flight, stops
   transmitting, and sends `released`. Otherwise it stops as today: there is
   no one ready to hand to, and the peer's silence timer covers it.
3. The standby, on a fresh `released` from its peer, waits until it has
   heard 100 ms of bus silence (so the last reply has drained), then takes
   over at once: no `standby_takeover_s`, no jitter.
4. The old master, if it keeps running (the API case), is now standing by
   and hears the new one. If nobody took over, its own takeover time brings
   it back, so a lost `released` costs today's gap and no more.

Expected gap: the 100 ms of silence plus the delivery time, a few
milliseconds on the LAN. Against 3–3.5 s today.

### Veto: don't take a bus you can't hear

A standby whose takeover timer has run out checks its peer first. If the
peer is fresh, says `active`, and its `reply_ms_ago` is under 2000, then the bus is alive at the peer's end and this standby is the
one that's deaf. It does not take over. It raises a new fault,
`deaf` ("hearing nothing on the bus while <peer> is running it and getting
replies"), which is a `/api/health` problem and turns the MQTT problem
sensor on.

Why replies and not requests: a master with a dead transmitter still
counts its own requests. Only the TAP's replies prove the wire works. A
master whose replies stop is not proving anything, and the standby takes
over as today. (The existing `answers` counter is the wrong one: it counts
optimizers' answers to 0x06 reads, which are off by default.)

The veto lasts exactly as long as fresh messages with a recent reply keep
coming. When they stop, the next silence check takes over as usual.

### Transport: MQTT first

The Pi already has an MQTT connection with broker auth, and the pair
already publishes its master state there. So the first carrier is MQTT:

- Topic `<base_topic>/coord`, not retained, QoS 0. Each device subscribes
  to its peer's.
- New setting `standby_peer`: the peer's `base_topic` (empty: off). The
  Tools page shows it under the standby role, with the peer's last state
  and age.
- No broker, no peer, or a stale peer: everything above is skipped.

**Ruling (mine):** MQTT rather than direct UDP. UDP would need a peer
address and its own authentication; MQTT has both already. The cost is
that a broker outage disables handoff and veto, which goal 3 makes safe.

### What a forged message can do

Anyone who can publish to the broker as the peer can:

- send `released` early: the standby starts talking over the live master,
  both see foreign requests, and the existing yield resolves it within an
  exchange or two (the same as two standbys starting together);
- send `active` with a recent reply: a standby that really should take
  over doesn't, for as long as the forgeries continue.

The second is a real way to suppress failover, and it needs broker
credentials. The broker's ACL is the defence; the docs should say to give
each monitor its own login with write access only to its own topics.

## Later: CAN as a second carrier

The same message fits one classic CAN frame (8 bytes): `state` and flags
(1), `seq` (2), `reply_ms_ago` and `heard_ms_ago` in 10 ms units
(2 each, saturating), and one spare byte. CAN would make handoff and veto work
without WiFi or a broker, and would suit an ESP32 pair (the Waveshare
ESP32-S3-RS485-CAN has the transceiver; ESPHome's `esp32_can` drives it).
A Pi needs an MCP2515 HAT or USB adapter, and in Docker host networking or
the `can0` interface moved into the container. Both carriers can run at
once; a receiver takes the newer `seq`.

Not first, because the Pi pair has no CAN hardware today and MQTT gives
both features to the pair that exists.

## Testing

Against the mock, extending `linux/scripts/standby-e2e.sh` (which already
runs two daemons on `mock_tap.py --extra-link` and a real broker). The deaf
and dead-transmitter cases need two new mock flags: one that stops sending
the bus to one link, and one that drops one link's writes.

- Stop A with SIGTERM; B takes over; the longest silence on the mock bus
  is under 500 ms (against ~3 s with `standby_peer` empty, measured in the
  same run).
- `POST /api/master/handoff` on A; B active, A standing by, `yields` and
  `takeovers` as expected.
- A `released` with an old `seq`, or older than 2.5 s, is ignored.
- Broker stopped: SIGTERM on A gives today's gap and B still takes over.
- Deaf standby: the mock stops sending the bus to B only. B does not
  take over, reports `deaf`; then stop A and B takes over once A's
  messages go stale.
- A with a dead transmitter (the mock drops A's writes): A's replies
  stop, B takes over despite A saying `active`.

On the roof, with the owner present: Zero active, Pi5 standing by with
`standby_peer` set both ways; install a build on the Zero and read the gap
from the Pi5's log; then pull the Pi5's RS485 A wire and check it reports
`deaf` without taking over.

## Open questions (the owner's)

1. Is the Zero + Pi5 pair the one this is for, or is an ESP32 standby
   coming soon enough that CAN should come first?
2. Does the broker give each monitor its own login today? The veto is
   only as trustworthy as the broker's ACL.
3. Should a standby whose veto has held for a long time (say an hour) do
   anything beyond the health problem, such as a louder MQTT event?
