# Bench findings, round 2 — overnight plan

> Executed inline (superpowers:executing-plans), owner's go-ahead 2026-11-08 ~00:10 UTC: "go for all of them, make it your overnight project". Branch `feature/bench-findings-2` off `feature/mesh-map` (it builds on the mesh map).

**Goal:** four things the 2026-11-06 bench listening taught us, built on the Pi: the TAP's radio settings on the Bus page, an automatic discovery window like the CCA's, each optimizer's settings, and relays on the mesh map.

**Spec:** this file (small items; the 2026-11-06 findings are in `docs/cca-tap-management.md`).

## Global Constraints

- **Nothing new reaches the roof without the owner.** Tasks 2 and 3 add bus behaviour; both are settings that default **off**, and neither is switched on on a Pi by me. Installing a build with them off is fine (behaviour unchanged).
- Never send a frame the CCA never sent. Task 2's read is the CCA's own bring-up read (tunnel 0x0D `00 00`), already allowed by `classify_request`.
- **The GatewayCfg block carries the PAN and the network key** (bytes 2–3, 15–30). Decode only channel (byte 1), SLT (14) and SLm (35). Never store, log, expose or commit the rest.
- No MACs or IDs in the API, logs, fixtures, tests or commits; fake prefix `04C05B0000` in tests.
- `/api/*` shape change → `site/screenshots/fixtures.mjs` in the same commit.
- Builds in `tigod-dev` Docker; `node --check` app.html; commits `git -c user.email=andrew@eiknet.com`, no trailers.

## Review Focus

1. A GatewayCfg reply of the wrong length, or a write's echo (`01 00` + 36 bytes), must not be decoded as a read.
2. Discovery auto: at night every optimizer is "unheard today" until sunrise; the policy must not hold the window open all night for that.
3. Discovery auto with no layout (nobody placed any panels): fall back to today's behaviour (always renew), not "never".
4. The owner's manual "let it close" switch must still win over auto.
5. A restart mid-day must not make every optimizer look missing (last-heard times are saved in state.json).

---

### Task 1: The TAP's radio settings, from any GatewayCfg read heard
- `bus_watch.h`: `struct TapRadio { uint16_t tap; int channel, slt, slm; }`; `bool parse_gateway_cfg(const TunnelReply &t, TapRadio &r)` — sub 0x0E, args exactly 37 bytes, status byte 0; BusWatch queues one per reply heard (`take_radios()`), from either master.
- `bus_log`: keep the latest per TAP with `heard_unix`, saved in versions.json (`radios`), loaded at start; `/api/versions` gains `radios: [{tap, channel, slt, slm, heard_unix}]`.
- UI: Bus page, the TAP line shows "Channel 18 · admits above 90" when known.
- Tests: bus_watch_test (decode; write echo refused; short refused; no key bytes in any field); bus-log e2e or host test for persistence; site test for the card; fixture.

### Task 2: Read it once at master start (setting, default off)
- Setting `master_read_tap_config` (settings.cpp/h, /api/settings GET/POST, Settings › Bus switch "Read the TAP's radio settings at start").
- `OpeningPlan` gains `config_read`; when on, the opening sends tunnel 0x0D `00 00` once.
- Tests: master_core_test (off: no 0x0D; on: exactly one 0x0D 00 00 in the opening).

### Task 3: Discovery like the CCA's (setting `discovery_auto`, default off)
- Pure `bool window_wanted(const std::vector<Expected> &, int64_t now, bool daylight)` in a shared header: wanted if no expected list (fallback), else if daylight and any expected optimizer unheard since local midnight.
- Daylight = some optimizer reported in the last 10 minutes.
- Expected = panels placed in the layout (by barcode), with last heard from the Monitor.
- main.cpp evaluates every 60 s and calls `MasterCore::set_window_wanted(bool)`; effective renewing = owner switch && wanted. `/api/master` gains `discovery_auto`, `window_wanted`, and `missing_expected` (count).
- Tests: host tests of `window_wanted` (focus 2, 3, 5); master_core_test (owner off beats wanted; not wanted → no renewals).
- UI: Bus page discovery card says "Auto: open while N optimizers in your layout haven't reported today" / "Auto: closed, all reported".

### Task 4 (after sunrise): relays on the mesh map
- From the Pi5's sunrise capture: the 40 NetDiag joins' data bytes 16–22. Find the parent and the join RSSI if the data shows them (a parent field equal for all on a single-TAP star is itself the answer). Decode only what the data supports; flip `parent_confirmed` only if placed.

### Task 5 (after sunrise): optimizer settings
- From the `!Smrt` answers in the capture (the Zero queries every optimizer at its first report): confirm token positions (3 HwVout, 10 stored flag, 11 AcLoss, 15 BaseOverride).
- BusWatch keeps each optimizer's latest `!Smrt` answer; bus_log saves it; `/api/optimizer-settings`; Devices page column or panel flagging the odd ones out.

### Then
Docs (cca-tap-management.md findings; raspberry-pi guide), CHANGELOG, one Fable review of the branch, CI build. No install without the owner.
