---
title: Start Here
description: What Tigo Monitor does, what you need to buy, and the five steps from parts on the desk to a live per-panel dashboard.
---

If your roof has Tigo optimizers on it, your panels are already reporting how much
power each one makes. That information goes to Tigo's box and, usually, to Tigo's
cloud. **This project lets you keep a copy for yourself** — on a small ESP32 board
you buy for about $30, with its own web dashboard and a direct feed into Home
Assistant. Nothing leaves your house, and there's no subscription.

It only *listens*. It is wired so it physically cannot talk back to your solar
equipment, so it can't disturb anything.

## What you'll end up with

- A web page on your home network showing **every panel individually** — watts,
  volts, amps, temperature — updating live.
- Charts of today, this week, this month, and this year, stored on the device
  itself so they survive a power cut.
- Home Assistant sensors, including a feed for its **Energy dashboard**.

## Words you'll run into

Solar and Tigo documentation both assume you already know these. You don't need
to memorise them, but they'll show up in the rest of the guides.

| Word | What it actually means |
|------|------------------------|
| **Optimizer** | The small box clipped behind each solar panel. Tigo's product. It's what reports the per-panel numbers. |
| **CCA** | Tigo's green wall box that collects from the optimizers and uploads to Tigo. Short for Cloud Connect Advanced. |
| **TAP** | A small Tigo radio antenna box that relays between the optimizers and the CCA. Some systems have one, some have several. |
| **RS485** | The two-wire cable running between the CCA/TAP and your optimizers. This is the conversation we quietly listen to. |
| **ESP32** | The little WiFi microcontroller board that does the listening. The AtomS3R is the one we recommend. |
| **ESPHome** | Free software that builds and installs the firmware onto that board for you. You'll use it once at setup. |
| **String** | One row/chain of panels wired together. |
| **MPPT** | One input on your inverter. Usually one or two strings plug into each. |
| **Frame** | One short message on the RS485 cable. Each one carries one reading from one panel. |
| **PSRAM** | Extra memory on the ESP32. You need a board that has it if you have 15 or more panels. |

## What to buy

For most people, two parts that click together:

| Part | Why |
|------|-----|
| **M5Stack AtomS3R** (~$20) | The ESP32 board. Has the extra memory, so it handles any size array. |
| **M5Stack Atomic RS485 Base** (~$10) | Snaps onto the bottom of the AtomS3R and lets it read the solar cable. |
| A few feet of twisted-pair wire | 22–24 AWG, to reach your CCA or TAP. |

You'll also need somewhere to run [ESPHome](https://esphome.io) — the Home
Assistant add-on is the easiest, but the command-line tool on a laptop works
just as well.

Cheaper alternative: any ESP32 dev board plus a MAX485 module. It works, but if
you have 15+ panels get one with PSRAM. See [Wiring](/esphome-tigomonitor/guides/wiring/).

## The five steps

### 1. Make it safe

**Before you open anything**, shut down and isolate your solar array using your
inverter's DC disconnect. Solar panels in daylight keep producing lethal voltage
even with the inverter switched off — turning the inverter off is *not* enough.

If that sentence made you uneasy, get a solar installer or electrician to do the
wiring part. That's a completely reasonable call and in some places it's the law.

### 2. Wire it in

Three wires: **A**, **B**, and **ground**, connecting your CCA or TAP to the
RS485 base. The board sits in the middle of the existing cable run and eavesdrops.
Ground is not optional — RS485 needs a shared reference to read reliably.

Full diagrams, terminal-by-terminal: **[Wiring guide](/esphome-tigomonitor/guides/wiring/)**.

### 3. Build your config file

ESPHome needs a configuration file describing your board. Rather than writing one
by hand, use the **[Config Builder](/esphome-tigomonitor/config-builder/)** — pick
your board, answer a few questions, copy the result.

### 4. Flash it

Plug the board into your computer by USB and run:

```bash
esphome run my-tigo-monitor.yaml
```

ESPHome downloads everything it needs, builds the firmware, and installs it. The
first build takes several minutes; later ones are quick. When it finishes it
prints the device's IP address.

### 5. Open the dashboard

Go to `http://<that-ip-address>/` in a browser.

Panels appear on their own over the first few minutes as each one happens to
report in — this is normal, they take turns on the cable. At first they'll have
placeholder names like "Module 4F2A". Naming them is the next step.

## Putting names on the panels

Out of the box the device knows there *is* a panel, but not that it's "South Roof
3". There are three ways to fix that, in order of least effort:

1. **Import from Tigo's cloud.** Sign in with your Tigo account on the device's
   Tigo Cloud page and pull the layout your installer already set up. Needs
   `cloud_import: true` in your config.
2. **Read it from the CCA.** If your CCA is on older firmware (before 4.0.4), the
   device can ask it directly over your network.
3. **Type them in.** The Tools page lets you name everything by hand. Tedious
   with 30 panels, but it always works.

Details for all three: [Configuration guide](/esphome-tigomonitor/guides/configuration/).

## How to tell it's working

Open the **Diagnostics** page on the device and look at two numbers:

- **Active devices** should match your panel count. Give it 10–15 minutes to
  find them all.
- **Missed packets** will always tick up slowly — that's normal on a shared
  cable. If it's climbing fast, see
  [Reducing frame loss](/esphome-tigomonitor/guides/uart-optimization/).

Nothing showing up at all? Start with the
[Troubleshooting guide](/esphome-tigomonitor/guides/troubleshooting/).

## Where to go next

| If you want to… | Read |
|-----------------|------|
| See it in Home Assistant | [Home Assistant](/esphome-tigomonitor/guides/home-assistant/) |
| Change a setting | [Configuration](/esphome-tigomonitor/guides/configuration/) |
| Fix something | [Troubleshooting](/esphome-tigomonitor/guides/troubleshooting/) |
| Keep long-term history on the device | [Saving history to flash](/esphome-tigomonitor/guides/tsdb-integration/) |
| Pull the data into your own scripts | [Web server & API](/esphome-tigomonitor/guides/web-server/) |
