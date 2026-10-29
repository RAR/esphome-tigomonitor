# Standby failover: a second bus master that takes over

Branch: `feature/cca-replacement`. Status: design, not started. Builds on
[the CCA replacement design](2026-10-26-cca-replacement-design.md).

## Why

Since the CCA died (2026-10-29) one Pi is the only bus master, with nothing
behind it. Every install, reboot or hardware fault stops the bus, and while the
bus is stopped the array is not producing. Shutting that Pi down to mount it in
the enclosure had to wait until dark. The owner wants a second device that
takes over when the first one fails, and that can be serviced in daylight.

## Goals

1. **A standby takes over a silent bus, unattended,** quickly enough that the
   optimizers' 900 s run lease never runs out (it is renewed when it reads 0;
   see the parent design, "The keep-alive").
2. **No flapping.** When the failed device comes back it stands by. It does not
   take the bus back.
3. **Any master the standby hears wins,** whether it's another of ours or a CCA.
   This lets a CCA be the primary with a Pi or an ESP32 standing by. A CCA can't
   step aside, so ours always has to.
4. **Both devices run the same configuration.** "Primary" and "secondary" are
   only a takeover priority, not two kinds of code.
5. **You can see it:** which device is running the bus, that the other one is
   standing by and hearing it, and when a takeover happened (MQTT, `/api/master`,
   `/api/health`).

## Non-goals

- Running two masters at once to share the load.
- Fencing hardware (a relay that cuts a hung master's transceiver). It's
  covered under Later, and isn't needed for safety (see below).
- A message link between the two devices in the first version. Silence on the
  bus is the only signal (see "Why not CAN first").

## Anti-goals

- **Weaker rapid shutdown.** This changes the old safety rule, so the change
  has to be made on purpose. Under the CCA, and under one Pi, cutting the
  master's power stopped the array. **With a standby, cutting one master's power
  only hands the bus to the other one.** So rapid shutdown must not depend on a
  master dying. Since 2026-10-29 the TAP is fed from the RSD supply, and cutting
  that silences the mesh whatever the masters are doing. That has to be the
  shutdown procedure, and it has to be confirmed before a standby is ever left
  armed on the roof (open question 1).
- **A takeover that strands hardware.** The standby must not re-address, store
  or reset a TAP that is already running. It continues the session that is
  running; it doesn't start a new one.
- **Two masters fighting** for longer than one exchange each.

## What we know

- **Master traffic is continuous.** The 0x0148 polls go out several times a
  second, all night as well, and a 0x0B0F lease check every 20 s. A request is
  any frame without the 0x8000 reply bit. A listener therefore knows to within
  a fraction of a second whether a master is alive.
- **A new master can pick up a session that is already running.** Every install
  and restart on the Pi so far stopped the bus for ~2 s, and the daemon resumed
  without any bring-up. The TAP can't tell one master from another: requests
  carry only the destination, never a source. A different device is new; the
  bytes on the wire are not. Tonight's shutdown for the enclosure lasted
  minutes, not seconds, and is the first longer test. Check how it recovered.
- **The state a master carries can all be learned by listening:**
  - the poll cursor, which `resyncs` already takes from the TAP's reply;
  - the lease, from the 0x2F replies on the bus;
  - the 0x41 targets (listen mode already learns them: `learned_41`);
  - the node count for the renewal, from the lease reply.

  The hourly 0x0B00 and the 0x26 walks just restart their timers.
- **Two masters are already detected.** In `transact()`, a request we didn't
  send makes the master stop with `second_master`. Today that fault is final:
  the master stops transmitting until it is restarted. For a standby it has to
  mean "go back to standing by".
- **A hung master that keeps its driver on fails safe.** It jams the bus,
  nobody's lease renewal gets through, and the array goes safe. That costs
  availability, not safety, which is why fencing can wait.

## How it works (decided)

### A third bus role, `standby`

`bus_role`: `listen` | `master` | `standby`. A device in `standby` does what
`listen` does: it decodes everything for panels, history and MQTT, and learns
the 0x41s. It also watches for master requests.

States: **standing_by** → **taking_over** → **active** → back to standing_by.

- **standing_by:** nothing is transmitted. It tracks the time since the last
  master request, and keeps the lease reading and the node count from the 0x2F
  replies.
- **Takeover:** after `takeover_after_s` seconds with no master request, it
  goes to **taking_over**. Defaults: 3 s for the preferred device and 8 s for
  the other; the setting is all that makes a device "primary". It listens for
  one more random 0–500 ms, and only transmits if the bus is still silent.
- **taking_over:** the first request is a lease check, not a poll. If the TAP
  answers at the running id, it goes to active and runs the usual schedule. If
  the TAP answers only at the factory address, the TAP was power-cycled, and
  the `master_startup` rules apply exactly as at boot. With nothing answering
  at all, it goes to active anyway and lets `no_reply` report it, the same as a
  master with no TAP.
- **active:** the same as `bus_role: master` today, with one difference: a
  `second_master` sends it back to standing_by rather than stopping it for
  good. It waits out its own takeover time before trying again.
- **At boot** a standby always starts in standing_by. It never transmits until
  it has listened for its full takeover time. That's how a device that comes
  back after a failure finds the other one running the bus and stays out of the
  way (goal 2).

### Collisions

If both devices start transmitting together, as after a power cut that took
both down, the existing foreign-request check sees the other's requests. Both
drop back, and the different takeover times break the tie. A collision corrupts
at most an exchange or two (bad checksums, a timeout), which the 900 s lease
easily absorbs.

**Ruling (mine):** no backoff growth or limit on takeover attempts. With fixed,
different delays a tie can't repeat. If it ever does, the attempt counter shows
it.

### The fail-safe heartbeat is unchanged

Each device keeps the rule that only its main loop renews the lease. A hung
standby renews nothing, and a hung active master stops renewing and gets
replaced. Neither can keep the lease going after it stops working.

### What you see

- `/api/master` and the MQTT master state gain:
  - `role` (`standby`);
  - `standby_state`: `standing_by`, `taking_over` or `active`;
  - `master_heard_s_ago`;
  - `takeovers` (a count) and `last_takeover` (an ISO timestamp);
  - `yields` (times it gave the bus back to another master).
- The MQTT `status` reads `standing_by` while healthy, and `problem` stays
  false. The binary sensor stays meaningful on both devices.
- `/api/health` on a standby has one new problem: "standby: no master heard
  for N s and the takeover did not start" (only when the takeover itself
  faulted). A standby hearing a healthy master is ok.
- Every takeover and yield is logged at warning level, so the Logs page shows
  the history.

### Settings

- `bus_role: standby`
- `standby_takeover_s` (an integer, 2–60, default 3)
- The existing `master_*` options apply when the device is active.

The Tools page gets the third role option. Its description says that a standby
does not stop the array when its peer loses power, and points to the RSD
supply.

## Why not CAN first

A CAN or LAN heartbeat proves the peer is running, not that it can reach the
TAP. Silence on the bus proves the real thing, from start to finish. Even a
planned handoff needs no link: stopping the service makes the bus silent, and
the standby takes over within its 3 to 8 s. A link would only save those few
seconds of telemetry. It's listed under Later, behind fencing.

## Testing

- **Without hardware:** the master e2e gets a two-daemon run against
  `mock_tap.py`. That needs a shared fake bus: one TAP and two masters, with
  each master's writes echoed to the other, since a pty pair has only two ends.
  A small bus broker in `linux/tools/` is the first piece of work. The cases:
  - Kill A; B takes over within its takeover time and its lease checks get
    answered.
  - Restart A; A stays standing_by and its `master_heard_s_ago` stays under 1.
  - SIGSTOP A for 20 s; B takes over. On SIGCONT, A sees B's requests and
    yields; `yields` = 1 and A is not active.
  - Start both together; exactly one ends up active within 10 s.
  - Neither device ever sends a re-address or a bring-up frame to a TAP that is
    answering at the running id.
- **On the roof, in this order, each step with the owner present:**
  1. One Pi in `standby` beside the running Pi master. It should stand by, with
     zero frames transmitted. The transmit counter proves that.
  2. Stop the master service; the standby takes over; check the takeover time
     and the panel data.
  3. Start the old master in `standby`; it stands by.
  4. When the backup CCA arrives: CCA as primary, Pi standing by. Unplug the
     CCA; the Pi takes over. Plug it back in; the Pi yields to it.

## Open questions (the owner's)

1. **Is cutting the RSD supply (and with it the TAP) now the shutdown
   procedure,** and has it been checked that this alone puts the array in safe
   mode with a master still running? This gates arming a standby on the roof.
2. Which device is the standby: a second Pi, or an ESP32
   ([ESP32 bus master design](2026-10-29-esp32-bus-master-design.md))?
3. How long can the TAP go without a master before it needs the bring-up
   again? Tonight's outage for the enclosure gives the first data point.

## Later

- **Planned handoff over the LAN** (an HTTP call to the peer): "take over now",
  so a planned swap costs about a second rather than the takeover time.
- **Fencing:** the standby cuts a jammed master's transceiver power through a
  relay, for the stuck-driver case. The Waveshare and LilyGO boards' CAN ports
  could carry that command too.
