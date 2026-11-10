# Mesh map — design

**Status:** approved in conversation 2026-11-07 (option C of the mockups). Pi only.

## Intent

Show how every optimizer reaches the bus: which TAP it reports through, whether it relays through a neighbour, and how good its link is. Today the only signal measure is a bus-wide missed-frame count; a weak optimizer is invisible until it drops out.

The owner picked a free-form network graph ("C"): each TAP a hub, its optimizers clustered around it, every line coloured by that optimizer's link quality. On a single-TAP roof it draws as one hub; it is built to show multi-TAP systems and relays when they exist.

## Goals

- A **Mesh** tab on the Topology page (tabs: Layout | Mesh, the `.page-tabs` pattern Settings and Diagnostics use). Linux only (`data-linux-only`); the ESP32 UI keeps Topology as it is.
- One node per optimizer, one hub per TAP, a line from each optimizer to the TAP it reports through. (Drawing a relay hop to a neighbour waits for the parent field; see NetDiag.)
- Each line and node coloured by link quality: good, fair, poor, or silent.
- Hover (and tap, on touch) fills a side panel: label and string, RSSI now, a 24-hour RSSI sparkline, reports lost today, the TAP it reports through (and "via X" for a relay), and when it last joined.
- Read-only. **No new bus frames.** Everything comes from what the Pi already hears, as master or as listener.

## Non-goals

- ESP32 support (later, if wanted; the decoders live in shared headers so it stays possible).
- Persisted history. The sparkline buffer lives in memory and starts empty after a restart.
- Any control from the map (moving a node, resetting it, re-keying). Those frames stay on the never-send list.
- MQTT/HA entities for link quality. Possible later; not in this work.

## Anti-goals

- Shipping a parent decode that's a guess. If the NetDiag layout isn't confirmed, every optimizer is drawn straight to the TAP it reports through (true regardless) and the panel says "parent: not yet reported".
- Any MAC, PAN or network key in a fixture, test, log line or commit. The map shows labels and short addresses; MACs appear only where the UI already shows them (Devices).

## Data

### Where it comes from

`Monitor::on_frame_` (linux/src/monitor.cpp) already walks every 0x0149 poll reply with `walk_power_packets` and decodes type `31`. The new work hooks the same walk:

| Need | Source |
|---|---|
| Which TAP | the poll reply's address (reply address with 0x8000 cleared): 1201, 1202, … |
| RSSI per report | `PowerReading::rssi`, already decoded |
| Reports lost | gaps in the slot counter (below) |
| Last join | type-`09` NetDiag packets in the same replies |

### Reports lost

**There is no per-report sequence number in the packet** (checked 2026-11-07 against the bench captures: no byte or 16-bit word steps by one between consecutive reports; the CCA UI's "serial" column is the CCA's own count). Loss comes from the slot counter instead:

- `slot_counter` is 2 bytes: top 2 bits Hslot (60 s each), low 14 bits 5 ms ticks, so 48,000 ticks wrap every 4 min. Reports land on 400-tick (2 s) bins: on the bench, gaps of 400 were the norm, 800/1200/1600 meant 1/2/3 bins missed, and 0 meant a duplicate.
- Per optimizer: lost += round(gap / cadence) − 1, where cadence is the smallest non-zero gap seen today (never below 400). Duplicates and out-of-order arrivals are ignored. A gap whose wall-clock time is 200 s or more isn't counted (the counter may have wrapped, and an optimizer that stops at dusk hasn't lost anything).
- `/api/mesh` reports `"loss_method": "slot"`.

### NetDiag (type 09)

Sent once per (re)join, which on a roof is mostly once per sunrise. What the bench captures show (2026-11-07, 13 packets):
- packet header bytes 3–4 repeat the TUID; the data is 23 bytes;
- data bytes 8–15 are the joining optimizer's own MAC (they match its TAP table entry);
- data bytes 0–7 are an 8-byte ID that is neither in the table nor a TAP MAC from GET_MAC. It may be a network-wide ID, so it is **never** logged or exposed;
- data bytes 16–22 vary between joins and hold the rest (RSSI and probably the parent). Their layout isn't known.

So the decoder confirms *who joined and when*, nothing more. The analyst's field list (NWID, TUID, ParN, Stat, MAC, RSSI, Algn) names what is in there but their offsets came from a different binary.

- Decode NetDiag as a pure function in `components/tigo_monitor/tigo_protocol.h`, host-tested with a synthetic packet built from the fake prefix `04C05B0000`.
- **Parent: not decoded in this work.** The API sends `"parent": null` and `"parent_confirmed": false`, and every optimizer is drawn to the TAP it reports through. Finding the parent needs a roof capture across a sunrise; that is a follow-up, not part of this plan.

### Per-optimizer state (in the Monitor)

- `rssi_now`, plus a ring of 288 five-minute averages (24 h). About 0.6 KB per optimizer.
- `reports_today`, `lost_today`, reset at local midnight.
- `tap` (RS485 id it was last heard through), `last_join_unix`.

## API

`GET /api/mesh` on the Pi (http_server.cpp), same auth as the other `/api/*` endpoints:

```json
{
  "loss_method": "slot",
  "parent_confirmed": false,
  "taps": [{"id": "1201"}],
  "nodes": [{
    "addr": "0012", "label": "C9", "string": "C",
    "tap": "1201", "parent": null,
    "rssi": 74, "rssi_24h": [76, 74, null, …],
    "reports_today": 2655, "lost_today": 225,
    "last_heard_s": 2, "last_join_unix": 1794049920
  }]
}
```

The sparkline array is oldest first, `null` where nothing was heard. This is a new `/api/*` shape, so `site/screenshots/fixtures.mjs` gains a `piRoutes['/api/mesh']` fixture (fake labels, no MACs) in the same commit, and the privacy test covers it.

## UI

- The mockup at `.superpowers/brainstorm/*/content/mesh-map.html` (option C) is the reference: dark/light tokens from app.html, TAP hubs as rounded rects, optimizers as circles labelled with their panel label, dashed line for a relay hop or a silent node.
- **Layout:** each TAP's optimizers placed around it at a distance that grows as RSSI falls, spread by golden angle so labels don't stack; several TAPs side by side, joined by a dotted RS485 line. Deterministic (same data → same picture), no physics library.
- **RSSI is a raw byte, not dBm.** `PowerReading::rssi` is the unsigned byte at char 38; the existing UI labels it "dB". On the roof (2026-11-07, 40 optimizers) it spans 66–222, median about 120; the one bench optimizer beside its TAP sent 199. Higher looks better, but the scale is unknown, so the map shows the raw number and never converts it to dBm.
- **Quality tiers (first cut, my call; tune on the roof before release).** Loss carries the meaning, because it is measured; RSSI only flags the weak tail:
  - poor: > 5 % lost, or RSSI < 80;
  - fair: 1–5 % lost, or RSSI < 110;
  - good: everything else;
  - silent: not heard in the last 10 minutes (dashed, grey).
  - The thresholds live in one JS constant so they're easy to retune.
- Legend under the graph. Refresh every 10 s while the tab is visible.
- Empty states: no reports yet ("Waiting for the first reports…"); listener with no TAP reply heard.
- Works at phone width: the side panel drops below the graph.

## Testing

- Host tests: NetDiag decode, slot-gap loss counting (gap, duplicate, out of order, wrap, dusk, midnight reset), the 5-minute ring.
- Site tests (Playwright, `site/test/`): the Mesh tab appears only on Linux, renders a node per fixture optimizer, colours by tier, fills the side panel on hover, and shows the "parent not yet reported" note.
- Screenshot: `pi-mesh` added to `PI_VIEWS`.
- Hardware: run on the Pi5 for a day; check the roof's gap distribution is dominated by 400 ticks and that today's loss figures look sane against the raw RSSI.

## Left to the builder

- How `parent_confirmed` becomes true (follow-up): my call is a code constant, flipped in the commit that confirms the field against a roof sunrise, not runtime detection.
- Exact sizing, spacing and label collision handling in the graph.
- Where the per-optimizer state struct lives (inside `Panel` or alongside it).
- Wording of empty states and tooltips, in the UI's existing voice.
