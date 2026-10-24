---
title: Raspberry Pi
description: Run Tigo Monitor on a Raspberry Pi Zero 2 W instead of an ESP32 — flash a ready-made SD card image, plug in a USB RS485 adapter, and point it at Home Assistant's MQTT broker.
---

The ESP32 is the main way to run Tigo Monitor. If you would rather use a
Raspberry Pi, there is a Pi build: the same dashboard and the same panel
decoding, packaged as an SD card image that needs no ESPHome and no YAML.

:::caution[Experimental]
The Pi build is new and has not yet run against a real Tigo bus — only
against recorded and synthetic traffic. If you try it, please
[open an issue](https://github.com/RAR/esphome-tigomonitor/issues) with what
you saw, working or not.
:::

## How it differs from the ESP32 build

- **Setup happens in the browser.** You set the MQTT broker, the serial port
  and power calibration on the Tools page; there is nothing to compile.
- **Home Assistant connects over MQTT**, not the ESPHome integration. Each
  panel appears as its own device, named by its barcode, as soon as the
  gateway announces it. The gateway announces its panels every 15 minutes or
  so, which is the longest you wait on a fresh install.
- **Not in the Pi build yet:** CCA sync (string and inverter names), Tigo
  cloud import, and the History charts. Without a CCA, every panel is shown in
  one group called "All panels". Daily energy and the Home Assistant Energy
  dashboard feed do work.

## What you need

- A **Raspberry Pi Zero 2 W** (any 64-bit Pi works) and a microSD card.
- A **USB RS485 adapter**, wired to the TAP the same way as the ESP32 build —
  A, B and ground; see [Wiring](/esphome-tigomonitor/guides/wiring/). An RS485
  HAT on the Pi's own serial pins works too.
- An **MQTT broker** that Home Assistant uses. If you don't have one, install
  the Mosquitto broker add-on in Home Assistant.

## Flash the card

1. Download `tigomonitor-VERSION-arm64.img.xz` from the
   [releases page](https://github.com/RAR/esphome-tigomonitor/releases).
2. Open [Raspberry Pi Imager](https://www.raspberrypi.com/software/), choose
   **Raspberry Pi Zero 2 W**, then **Use custom** and pick the file.
3. When Imager offers to apply OS customisation, choose **Edit settings** and
   fill in:
   - your **Wi-Fi** name and password — the Zero 2 W has no Ethernet port, so
     without this it cannot reach your network;
   - a **username and password**, which you only need if you want to log in
     over SSH;
   - a hostname, if you don't want the default `tigomonitor`.
4. Write the card, put it in the Pi, connect the RS485 adapter, and power it
   on. The first boot takes a few minutes.

## Connect it to Home Assistant

1. Open `http://tigomonitor.local` (or your hostname). The dashboard fills in
   as panels report.
2. Go to **Tools → Home Assistant** and enter the broker's address, port, and
   a username and password the broker accepts. With the Mosquitto add-on that
   is your Home Assistant's address, port 1883, and any Home Assistant user.
3. Save. The section header changes to *connected* within a few seconds, and
   panels appear in Home Assistant under **Settings → Devices & services →
   MQTT**.

For the Energy dashboard, add the **Tigo Monitor Energy** sensor under
**Settings → Dashboards → Energy → Solar production**.

## If nothing shows up

- **No panels on the dashboard:** check **Diagnostics → Frames processed**.
  If it stays at 0, the Pi is not receiving the bus: check the wiring and
  that **Serial port** under Tools is `auto` or names your adapter.
- **Panels on the dashboard but not in Home Assistant:** the Home Assistant
  section of Tools shows the broker's error, such as a refused password. Also
  allow up to 15 minutes on a fresh install for the gateway's next
  announcement.
- **Logs:** log in over SSH and run `journalctl -u tigomonitord -f`.
