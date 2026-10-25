---
title: Raspberry Pi
description: Run Tigo Monitor on a Raspberry Pi Zero 2 W instead of an ESP32 — flash a ready-made SD card image, plug in a USB RS485 adapter, and point it at Home Assistant's MQTT broker.
---

The ESP32 is the main way to run Tigo Monitor. If you would rather use a
Raspberry Pi, there is a Pi build: the same dashboard and the same panel
decoding, packaged as an SD card image that needs no ESPHome and no YAML.

:::caution[Experimental]
The Pi build is new and has run on one installation so far: a Pi Zero 2 W
with a Waveshare RS485 CAN HAT, on a 40-panel bus, where its readings
matched the ESP32's on the same bus panel for panel. If you try it, please
[open an issue](https://github.com/RAR/esphome-tigomonitor/issues) with what
you saw, working or not.
:::

## How it differs from the ESP32 build

- **Setup happens in the browser.** You set the MQTT broker and choose the
  serial port on the Tools page, and what the ESP32 takes from YAML (power
  calibration, night-mode timeout, reset at midnight, a CCA IP) under
  **Tools → Device Configuration**. There is nothing to compile.
- **Home Assistant connects over MQTT**, not the ESPHome integration. Each
  panel appears as its own device, named by its barcode, as soon as the
  gateway announces it. The gateway announces its panels every 15 minutes or
  so, which is the longest you wait on a fresh install.
- **Inverters are set on the Tools page**, not in YAML: each inverter and
  the MPPTs it owns. A Tigo cloud import fills them in for you when none are
  set.
- **History is kept on the SD card**, with a snapshot every minute
  rather than the ESP32's 30, for two years. Panels are not recorded at
  night, once the bus has been silent for the night-mode timeout, which
  roughly halves what the card stores.
- **The CCA Info page talks to the CCA over the Pi's Bluetooth**, as the
  ESP32 does with `cca_source: ble`. The Pi finds the nearest CCA by itself;
  there is no MAC to enter. For a CCA on firmware older than 4.0.4, set its
  IP under Device Configuration to use its local HTTP API instead.

## What you need

- A **Raspberry Pi Zero 2 W** (any 64-bit Pi works) and a microSD card.
- A **USB RS485 adapter**, wired to the TAP the same way as the ESP32 build —
  A, B and ground; see [Wiring](/esphome-tigomonitor/guides/wiring/). An RS485
  HAT on the Pi's own serial pins works too; see
  [Using an RS485 HAT](#using-an-rs485-hat).
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

## Using an RS485 HAT

A HAT such as Waveshare's
[RS485 CAN HAT](https://www.waveshare.com/wiki/RS485_CAN_HAT) reads the bus on
the Pi's own serial port. The monitor only listens, so a HAT that switches
direction by itself, as that one does by default, needs no extra setup.

The SD card image turns the serial port on for you. On a stock Raspberry Pi
OS install, turn it on, without a login console on it, and reboot:

```sh
sudo raspi-config nonint do_serial_cons 1
sudo raspi-config nonint do_serial_hw 0
sudo reboot
```

Under **Tools → RS485 adapter and web UI**, choose **Raspberry Pi UART**
(`/dev/serial0`), or leave the SD card image's **Automatic**, which uses it
when no USB adapter is plugged in.

## On a machine that does other things

The arm64 `.deb` package from the releases page installs on 64-bit Raspberry
Pi OS or Debian 13 (trixie), including on a machine that already runs other
services. It stays out of their way until you set it up:

- **It opens no serial port until you choose one.** A Zigbee or Z-Wave stick
  on the same machine is never touched. Choose the RS485 adapter under
  **Tools → RS485 adapter and web UI**; the list names USB adapters by make
  and model. Listing the ports does not open them. (The SD card image starts
  at **Automatic** instead, since nothing else runs on it.)
- **Its web UI moves to port 8080 if port 80 is taken**, and the log says so.
  Set another port in the same card; it takes effect when the service
  restarts, and the page offers to restart and follow it there. The
  `tigomonitor.local` network-browser entry still says port 80: edit
  `/etc/avahi/services/tigomonitor-http.service` to match.

Installing the package starts the service straight away. To keep it off a
port another service may want, such as 8080, set the port before you
install. The monitor reads `settings.json` when it first starts, and
installing never overwrites it:

```sh
sudo mkdir -p /var/lib/tigomonitor
echo '{"web_port": 8099}' | sudo tee /var/lib/tigomonitor/settings.json
sudo apt install ./tigomonitord_VERSION_arm64.deb
```

The same file takes `"serial_port"` too. Anything left out keeps its
default, and the web UI edits the file from then on.

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

## Name your panels and strings

Out of the box every panel is named by its barcode and sits in one group
called "All panels". There are three ways to get your real layout:

- **From Tigo's cloud:** open **Tigo Cloud**, choose **Configure**, and log
  in with the account you use in the Tigo app. Then press **Import from Tigo
  cloud** on the Topology page. Panel names, strings, MPPTs and inverters
  all come across. The Pi keeps only the login token, not your password.
- **From an ESP32:** on the ESP32's **Node Table** page choose **Export**,
  then **Import** the file on the Pi's. Names, strings, and your Topology
  renames and panel ratings come across. If you haven't set up inverters on
  the Pi yet, the backup's inverter names wait and apply once you do.

- **From the CCA, on firmware older than 4.0.4:** set the CCA's IP under
  **Tools → Device Configuration**, then press **Refresh from CCA** on the
  CCA Info page.

If the strings land under **Unassigned**, go to **Tools → Inverters** and
list which MPPTs each inverter owns.

## If nothing shows up

- **No panels on the dashboard:** check **Diagnostics → Frames processed**.
  If it stays at 0, the Pi is not receiving the bus: check the wiring and
  that **Serial port** under Tools names your adapter, or is **Automatic**.
- **Panels on the dashboard but not in Home Assistant:** the Home Assistant
  section of Tools shows the broker's error, such as a refused password. Also
  allow up to 15 minutes on a fresh install for the gateway's next
  announcement.
- **Lots of bad checksums:** A and B are probably swapped.
- **Logs:** log in over SSH and run `journalctl -u tigomonitord -f`.
