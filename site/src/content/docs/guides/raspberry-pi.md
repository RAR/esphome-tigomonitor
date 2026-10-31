---
title: Raspberry Pi and Linux
description: Run Tigo Monitor on a Raspberry Pi, another Linux machine or in Docker instead of an ESP32 — install one package, connect an RS485 adapter or a network bridge, and point it at Home Assistant's MQTT broker.
---


The ESP32 is the main way to run Tigo Monitor. If you would rather use a
Raspberry Pi, there is a Pi build: the same dashboard and the same panel
decoding, packaged for Raspberry Pi OS, with no ESPHome and no YAML. The same
build runs on other Debian and Ubuntu machines and in Docker, and can read the
bus from a small ESP32 bridge at the Tigo cabinet; see
[Other ways to run it](#other-ways-to-run-it).

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

- A **Raspberry Pi Zero 2 W** (any 64-bit Pi works) and a microSD card,
  running **Raspberry Pi OS (64-bit)**, the current release, based on Debian
  13 (trixie). The Lite version is enough. The package does not install on
  the older bookworm release, which Imager lists under Legacy.
- A **USB RS485 adapter**, wired to the TAP the same way as the ESP32 build —
  A, B and ground; see [Wiring](/esphome-tigomonitor/guides/wiring/). An RS485
  HAT on the Pi's own serial pins works too; see
  [Using an RS485 HAT](#using-an-rs485-hat). So does an ESP32 bridge at the
  cabinet; see [Read the bus over the network](#read-the-bus-over-the-network).
- An **MQTT broker** that Home Assistant uses. If you don't have one, install
  the Mosquitto broker add-on in Home Assistant.

## Install it

1. In [Raspberry Pi Imager](https://www.raspberrypi.com/software/), choose
   **Raspberry Pi Zero 2 W**, then **Raspberry Pi OS Lite (64-bit)**.
2. When Imager offers to apply OS customisation, choose **Edit settings** and
   fill in:
   - your **Wi-Fi** name and password — the Zero 2 W has no Ethernet port, so
     without this it cannot reach your network;
   - a **username and password**, and turn on **SSH** under Services;
   - a hostname such as `tigomonitor`.
3. Write the card, put it in the Pi, connect the RS485 adapter, and power it
   on. Once it has joined your Wi-Fi, log in over SSH.
4. Download `tigomonitord_VERSION_arm64.deb` from the
   [releases page](https://github.com/RAR/esphome-tigomonitor/releases) and
   install it:

   ```sh
   sudo apt install ./tigomonitord_VERSION_arm64.deb
   ```

   The service starts straight away and on every boot after.
5. Open `http://tigomonitor.local` (your hostname, then `.local`). Under
   **Tools → RS485 adapter and web UI**, choose your adapter. USB adapters
   are listed by make and model. It opens the adapter as soon as you save,
   and the dashboard fills in as panels report.

## Using an RS485 HAT

A HAT such as Waveshare's
[RS485 CAN HAT](https://www.waveshare.com/wiki/RS485_CAN_HAT) reads the bus on
the Pi's own serial port. The monitor only listens, so a HAT that switches
direction by itself, as that one does by default, needs no extra setup.

Raspberry Pi OS leaves that serial port off. Turn it on, without a login
console on it, and reboot:

```sh
sudo raspi-config nonint do_serial_cons 1
sudo raspi-config nonint do_serial_hw 0
sudo reboot
```

Then, under **Tools → RS485 adapter and web UI**, choose **Raspberry Pi
UART** (`/dev/serial0`).

## Connect it to Home Assistant

1. Open `http://tigomonitor.local` (or your hostname).
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

## Set a password

Out of the box, as on the ESP32, the web UI has no login: anyone on your
network can open it and change its settings, including the CCA's Wi-Fi and
the MQTT broker. To require one, set **Web UI password** under **Tools →
RS485 adapter and web UI**. The browser then asks for the username (`admin`
unless you change it) and password. Pages on other websites can't use the
monitor's settings either way, even with the login saved in your browser.
Over plain HTTP the password crosses your network readable by anything on
it; [HTTPS](#use-https) closes that.

The MQTT password stays on the Pi: the web UI never shows it, and changing
the broker's address or username clears it, so it has to be entered again.

## Use HTTPS

The web UI serves HTTPS once you give it a certificate and its key. Browsers
trust a certificate only for a name, not an IP address, so you need a domain
name pointing at the Pi, `tigo.example.com` say. Let's Encrypt issues
certificates for free and can do it without the Pi being reachable from the
internet: its DNS challenge proves you own the domain through a TXT record,
which certbot writes through your DNS provider's API.

1. Point the name at the Pi's address on your network, with an A record at
   your DNS provider or an override in your router's DNS.
2. Install certbot and the plugin for your DNS provider, and give the
   plugin credentials that can edit your domain's records. On Raspberry Pi
   OS, for example:
   - **Cloudflare:** `sudo apt install certbot python3-certbot-dns-cloudflare`,
     then put an API token in `/root/cloudflare.ini` as the plugin's
     instructions say.
   - **Amazon Route 53:** `sudo apt install certbot python3-certbot-dns-route53`.
     Create an IAM user for it alone, with a policy that allows
     `route53:ListHostedZones` and `route53:GetChange` on `*`, and
     `route53:ChangeResourceRecordSets` only on
     `arn:aws:route53:::hostedzone/YOUR_ZONE_ID`. Put its access key in
     `/root/.aws/credentials`, readable by root only, under `[default]`.
3. Save this as `/usr/local/bin/tigomonitor-cert-hook`, and make it
   executable with `sudo chmod +x`. It copies the certificate where the
   monitor can read it, since `/etc/letsencrypt` is root's alone:

   ```sh
   #!/bin/sh
   set -e
   for f in fullchain.pem privkey.pem; do
     install -m 600 -o tigomonitor -g tigomonitor "$RENEWED_LINEAGE/$f" /var/lib/tigomonitor/$f.new
     mv /var/lib/tigomonitor/$f.new /var/lib/tigomonitor/$f
   done
   ```

4. Get the certificate, with the hook. For Cloudflare:

   ```sh
   sudo certbot certonly --dns-cloudflare --dns-cloudflare-credentials /root/cloudflare.ini \
     -d tigo.example.com --deploy-hook /usr/local/bin/tigomonitor-cert-hook
   ```

   For Route 53, which finds the key in `/root/.aws/credentials` by itself:

   ```sh
   sudo certbot certonly --dns-route53 \
     -d tigo.example.com --deploy-hook /usr/local/bin/tigomonitor-cert-hook
   ```

5. Under **Tools → RS485 adapter and web UI**, set **HTTPS certificate** to
   `/var/lib/tigomonitor/fullchain.pem` and **HTTPS key** to
   `/var/lib/tigomonitor/privkey.pem`, save, and let it restart. The page
   reopens at `https://` on port 443, or 8443 if something else has 443.

certbot renews the certificate on its own and runs the hook each time. The
monitor checks the two files every 10 seconds and uses a renewed pair for new
connections, with no restart. A pair that doesn't load (a key that isn't the
certificate's, say) is logged on the Logs page and the previous one kept. If
the pair won't load when the service starts, the web UI comes up on plain
HTTP instead and the log says why, so you can still reach it to fix the
paths.

## Other ways to run it

None of these are needed for a Pi at the Tigo cabinet.

### Read the bus over the network

The machine running the monitor doesn't have to be at the Tigo cabinet. A
small ESP32 there can pass the bus along over Wi-Fi instead, and the monitor
reads it from the network. The bridge only forwards bytes; the decoding, the
dashboard and the history all stay on the machine running the monitor.

1. Flash
   [`boards/example-t-can485-bridge.yaml`](https://github.com/RAR/esphome-tigomonitor/blob/main/boards/example-t-can485-bridge.yaml)
   to a LilyGO T-CAN485, with your Wi-Fi filled in, and wire its RS485
   terminals to the TAP as in [Wiring](/esphome-tigomonitor/guides/wiring/).
   It runs [stream_server](https://github.com/oxan/esphome-stream-server),
   which sends every byte from the bus to whoever connects on port 6638.
2. Under **Tools → RS485 adapter and web UI**, choose **Network bridge (TCP)**
   and enter the bridge's address, `tigo-bridge.local:6638` or its IP and
   port, then save.

The bridge can't send anything onto the bus: its UART has no transmit pin,
and the transceiver's driver is held off. If the bridge loses power or Wi-Fi,
the monitor notices within about a minute and reconnects when it's back.

An RS485-to-Ethernet converter set to TCP server mode, passing bytes through
unchanged, takes the bridge's place the same way. So far only the T-CAN485
config has been built, and the monitor's side tested against a simulated
bridge.

### On a machine that does other things

The same package installs on 64-bit Raspberry Pi OS, Debian 13 (trixie), or
Ubuntu 24.04 or 26.04, on a machine that already runs other services. For an x86 machine, take the
`amd64` package instead of the `arm64` one. It stays out of their way until
you set it up:

- **It opens no serial port until you choose one.** A Zigbee or Z-Wave stick
  on the same machine is never touched, and listing the ports does not open
  them.
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

### Run it in Docker

On a machine that already runs containers, use the image instead of the
package. It's built for arm64 and amd64:

```sh
docker run -d --name tigomonitord --restart unless-stopped \
  -p 8080:8080 -v tigomonitor:/var/lib/tigomonitor \
  ghcr.io/rar/tigomonitord:latest
```

Then open `http://HOST:8080` and set it up under Tools, as with the package.
[`linux/docker-compose.yml`](https://github.com/RAR/esphome-tigomonitor/blob/main/linux/docker-compose.yml)
does the same with Compose.

- **The bus:** a [network bridge](#read-the-bus-over-the-network) needs
  nothing extra. For a USB adapter, pass it in with
  `--device /dev/serial/by-id/usb-…:/dev/ttyUSB0` and choose `/dev/ttyUSB0`.
  The by-id name stays the same across reboots, where the host's own
  `/dev/ttyUSB0` may change.
- **Restarts:** use `--restart unless-stopped`, not `on-failure`. The web UI's
  **Restart service** exits cleanly and relies on Docker starting it again.
- **The web port** is 8080 inside the container; publish it as any port you
  like. The web UI's port setting does nothing here.
- **HTTPS:** get the certificate on the host as in [Use HTTPS](#use-https),
  but with this hook instead, since the host has no `tigomonitor` account.
  The image's account is user ID 999:

  ```sh
  #!/bin/sh
  set -e
  install -d -m 700 -o 999 /srv/tigo-certs
  for f in fullchain.pem privkey.pem; do
    install -m 600 -o 999 "$RENEWED_LINEAGE/$f" /srv/tigo-certs/$f.new
    mv /srv/tigo-certs/$f.new /srv/tigo-certs/$f
  done
  ```

  Mount the directory with `-v /srv/tigo-certs:/certs:ro`, and set
  `/certs/fullchain.pem` and `/certs/privkey.pem` under Tools. The container
  port stays 8080, now serving HTTPS: publish it as `-p 443:8080` if nothing
  on the host has 443. Renewals reach the container through the mount, and
  the monitor picks them up as it does on the package.
- **The CCA page over Bluetooth** needs the host's BlueZ. Mount
  `/run/dbus/system_bus_socket` into the container, and run it as an account
  the host knows that is in its `bluetooth` group: `--user UID:GID`, plus
  `--group-add` with the host's bluetooth group ID. D-Bus drops connections
  from user IDs the host can't look up, and the image's own account is one of
  those. That account then also needs to own the state directory, so use a
  directory it owns in place of the named volume. This hasn't been tried
  against a CCA yet. The CCA's HTTP API, for firmware older than 4.0.4, needs
  nothing extra.

## If nothing shows up

- **No panels on the dashboard:** check **Diagnostics → Frames processed**.
  If it stays at 0, the Pi is not receiving the bus: check the wiring, and
  that **Tools → RS485 adapter and web UI** names your adapter.
- **Panels on the dashboard but not in Home Assistant:** the Home Assistant
  section of Tools shows the broker's error, such as a refused password. Also
  allow up to 15 minutes on a fresh install for the gateway's next
  announcement.
- **Lots of bad checksums:** A and B are probably swapped.
- **Logs:** log in over SSH and run `journalctl -u tigomonitord -f`, or
  `docker logs -f tigomonitord` for the container.
