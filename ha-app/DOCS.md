# TigoMonitor

TigoMonitor shows each Tigo optimizer's power, voltage and temperature,
decoded from the RS485 bus between the Tigo TAP and the CCA, and sends them to
Home Assistant over MQTT. This app is TigoMonitor's Raspberry Pi build, run
by Home Assistant.

## Connect it to the bus

Open TigoMonitor from the sidebar, then choose under **Settings → Bus**:

- **Network bridge (TCP):** a small ESP32 at the Tigo cabinet that passes the
  bus along over Wi-Fi. Enter its address, such as `10.0.0.46:6638`. The
  [Config Builder](https://tigomonitor.overwattlabs.com/config-builder/?build=bridge)
  writes the bridge's config, and
  [Network bridge](https://tigomonitor.overwattlabs.com/guides/network-bridge/)
  covers the boards and the wiring.
- **A USB RS485 adapter** plugged into this machine, wired to the TAP's A, B
  and ground. Choose it by its `/dev/serial/by-id/` name, which stays the
  same across reboots.

## MQTT

If the Mosquitto broker app is installed, TigoMonitor uses it from its first
start, and the panels appear in Home Assistant through MQTT discovery. It's
set once; change it under **Settings → MQTT**.

## Signing in

Home Assistant signs you in before it shows the page, so TigoMonitor doesn't
ask again, and changes to the bus need no web password here. To open the web UI outside Home Assistant, give port 8080 a host
port under this app's **Network** settings, and set a password under
**Settings → Web UI**: that port answers anyone on your network.

Leave HTTPS off under **Settings → Web UI**. Home Assistant reaches the app
over plain HTTP, and does HTTPS itself.

## Configuration

The app's **Configuration** tab has two switches, both off at first. Each
takes effect when the app restarts.

- **Dangerous options** shows Dangerous options under **Settings → Bus**,
  when TigoMonitor runs the bus: each TAP's radio profile, channel, network
  key and report period, and re-sorting the optimizers. A wrong choice there
  can cost reports until it's set back. This switch decides, whatever the
  app's settings.json says.
- **Debug logging** puts more detail in the app's **Log** tab, for tracking
  down a problem.

At each start the **Log** tab says what the switches gave, as
`Configuration: dangerous options on, debug logging off`. While TigoMonitor
only listens to the bus, that line is where to see the first switch took:
the page shows Dangerous options only while it runs the bus.

## More

The full guide, including running the bus in the CCA's place:
https://tigomonitor.overwattlabs.com/guides/raspberry-pi/
