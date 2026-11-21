---
title: Network bridge
description: A small ESP32 at the Tigo cabinet that passes the RS485 bus over Wi-Fi to TigoMonitor on a Raspberry Pi, another Linux machine or Home Assistant, so the monitor can run somewhere else.
---

A network bridge is a small ESP32 at the Tigo cabinet that passes the RS485
bus along over Wi-Fi. TigoMonitor runs somewhere else, on a
[Raspberry Pi or another Linux machine](/esphome-tigomonitor/guides/raspberry-pi/)
or as the
[Home Assistant app](/esphome-tigomonitor/guides/raspberry-pi/#run-it-as-a-home-assistant-app),
and reads the bus from the bridge instead of from an adapter plugged into it.

Use one when the machine you want to run TigoMonitor on isn't at the cabinet:
a Home Assistant box in another room, say, or a Pi in a rack. If the monitor
can sit at the cabinet, you don't need a bridge. Wire an
[ESP32 monitor](/esphome-tigomonitor/guides/getting-started/) or a Pi's RS485
adapter to the TAP directly.

## What it does, and what it doesn't

The bridge decodes nothing. It runs
[stream_server](https://github.com/oxan/esphome-stream-server), which sends
every byte it hears on the bus to whoever connects on TCP port 6638. The
decoding, the dashboard, the history and the panels in Home Assistant all come
from TigoMonitor on the other machine. On its own, a bridge shows Home
Assistant only its own status: whether it's online, whether a client is
connected, and its Wi-Fi signal.

It can't send anything onto the bus. Its UART has no transmit pin, and a pin
on the board holds the transceiver's driver off. So a bridge can feed
TigoMonitor listening alongside the CCA, but it can't be used for
[TigoMonitor running the bus](/esphome-tigomonitor/guides/raspberry-pi/#run-the-bus-in-the-ccas-place)
in place of a CCA. That needs an adapter wired to the machine running it.

## What you need

- **TigoMonitor running on the other machine.** Set it up first, from
  [Raspberry Pi and Linux](/esphome-tigomonitor/guides/raspberry-pi/).
- **One of these boards.** Neither needs PSRAM.
  - An **M5Stack AtomS3 or AtomS3R** on the **Atomic RS485 Base**. This is the
    one we've run on a roof.
  - A **LilyGO T-CAN485**. Its config builds, but it hasn't run on a bus yet.
- **Wi-Fi at the cabinet.** It doesn't have to be strong. An AtomS3 at
  −80 dBm passed 859 frames in 10 seconds with none cut short.

## Build it

1. Open the [Config Builder](/esphome-tigomonitor/config-builder/?build=bridge),
   which opens on **A network bridge**. Pick your board, fill in your Wi-Fi,
   and download the config and its `secrets.yaml`.
2. Flash it over USB the first time, as in
   [Start Here](/esphome-tigomonitor/guides/getting-started/#4-flash-it). Later updates can
   go over Wi-Fi.
3. Power the bridge up, then wire its A, B and ground to the TAP as in
   [Wiring](/esphome-tigomonitor/guides/wiring/). On the AtomS3 the Atomic
   RS485 Base is the same one an AtomS3R monitor uses.

Power it up before you connect A and B. For about a second after power-on,
before its firmware holds the driver off, the AtomS3's base can drive the bus.
Our AtomS3R held the bus for about that long at each boot. If a bridge ever
reboots while it's connected, expect a short gap on the bus.

The same configs are in the repository as
[`example-atoms3-bridge.yaml`](https://github.com/RAR/esphome-tigomonitor/blob/main/boards/example-atoms3-bridge.yaml)
and
[`example-t-can485-bridge.yaml`](https://github.com/RAR/esphome-tigomonitor/blob/main/boards/example-t-can485-bridge.yaml),
with a comment on every pin.

## Point TigoMonitor at it

In TigoMonitor, under **Settings → Bus**, choose **Network bridge (TCP)** and
enter the bridge's address and port, then save:

- `tigo-bridge.local:6638`, if you kept the builder's device name, or
- its IP address, such as `192.168.1.46:6638`.

If the name doesn't resolve, use the IP. Names ending in `.local` don't reach
every network, and Docker containers and Home Assistant apps often can't look
them up. Give the bridge a fixed address in the builder's **Static IP** field,
or reserve one in your router, so the address doesn't change under you.

If the bridge loses power or Wi-Fi, TigoMonitor notices within about a minute
and reconnects when it's back.

## Instead of an ESP32

An RS485-to-Ethernet converter set to TCP server mode, passing bytes through
unchanged, takes the bridge's place the same way. Unlike the ESP32 bridge,
most converters will send onto the bus whatever a client writes to them.
TigoMonitor writes nothing to a network bridge, so this only matters if other
programs can reach the converter.
