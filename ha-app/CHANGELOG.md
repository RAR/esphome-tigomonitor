# Changelog

What changed in the TigoMonitor app at each release, and anything you need
to do. Each release's full notes are on
[GitHub](https://github.com/RAR/tigomonitor/releases).

## 2.5.1

Nothing to do, unless you turned on dangerous options in the app's
settings.json: turn them on in the new Configuration tab as well, or this
update turns them off.

- **A Configuration tab.** Turn on dangerous options there (each TAP's radio
  settings and report period, and re-sorting the optimizers), and debug
  logging. Each takes effect when the app restarts.
- **Changes to the bus no longer ask you to set a web password first.** When
  the app runs the bus, System setup's changes, turning the array's output
  off and a TAP's report period said "set a web password first", though Home
  Assistant had already signed you in. They now count that sign-in, as
  everything else already did.

[All of 2.5.1's changes](https://github.com/RAR/tigomonitor/releases/tag/v2.5.1)

## 2.5.0

Nothing to do for most installs. Two cases take a step, below: building a
network bridge again, and a TAP you added with TigoMonitor before this
release.

- **The app no longer sits at "starting".** Home Assistant now counts it as
  started once its page answers, within a few seconds.
- **A new TAP can take optimizers (experimental, when TigoMonitor runs the
  bus).** Your TAPs share one network key and are told each other's
  networks, and **Re-sort the optimizers** lets each optimizer join the TAP
  it hears. On our roof the TAP that had none took 22 of the 40, and the
  next day's missed reports fell from about 2.3% to 0.7%.
- **A bus cut with several TAPs no longer leaves 30 minutes without
  readings.**
- **The Mesh map no longer draws relay lines across TAPs.**
- **Uptime is TigoMonitor's own**, not the Home Assistant machine's.

**A network bridge** that's running keeps working. To build one again, take
the new config from the
[Config Builder](https://tigomonitor.overwattlabs.com/config-builder/?build=bridge)
and build it on ESPHome 2026.11 or later: the old config doesn't build on
current ESPHome. It keeps port 6638, so the app finds the bridge as before.
It serves one client at a time, and its "Tigo Bridge Clients" sensor gives
way to "Tigo Bridge Disconnects".

**A TAP you added with TigoMonitor before 2.5.0** got a network key of its
own, so no optimizer can join it. The
[release notes](https://github.com/RAR/tigomonitor/releases/tag/v2.5.0)
say how to give it the shared key. This only applies when TigoMonitor runs
the bus; over a bridge it only listens.

[All of 2.5.0's changes](https://github.com/RAR/tigomonitor/releases/tag/v2.5.0)

## 2.4.0

The first release of the app. It reads the bus from a network bridge at the
Tigo cabinet, or from a USB RS485 adapter on the Home Assistant machine. On
first start it uses the Mosquitto broker app, so the panels appear in Home
Assistant without typing a broker in. Home Assistant signs you in, and its
backups include the app's settings and history.

[All of 2.4.0's changes](https://github.com/RAR/tigomonitor/releases/tag/v2.4.0)
