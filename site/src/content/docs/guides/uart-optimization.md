---
title: Reducing RS485 Frame Loss
---

**What this is:** a how-to for cutting down missed Tigo telemetry frames on the RS485/UART bus. **Who it's for:** solar owners running this component who see the "Missed Packets" counter climbing in the web UI or Home Assistant.

Frames arrive continuously at 38400 baud. If the ESP32 can't drain the UART receive buffer fast enough — usually because the display, LEDs, or WiFi are stealing CPU time — the buffer overflows and you lose frames. The fixes below free up that time and give the buffer more headroom. Apply them in order; most people only need the first two.

## 1. Keep the UART ISR in fast RAM

This is the single most important setting. It keeps the UART interrupt handler in internal IRAM so it still runs while flash is busy, which dramatically reduces dropped bytes. Every shipping board YAML sets it:

```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"
```

If you change nothing else, keep this.

## 2. Size the RX buffer (one consistent setting)

Set the receive buffer in **two** places to the **same** value: the SDK-level `CONFIG_UART_RX_BUFFER_SIZE` and the `rx_buffer_size:` on the `uart:` component. They must match so the driver and the component agree on capacity (the P4 board YAML comment puts it simply: "Match SDK buffer for consistency").

```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"
      CONFIG_UART_RX_BUFFER_SIZE: "8192"   # match rx_buffer_size below

uart:
  id: tigo_uart
  rx_pin: GPIO5          # your board's RX pin
  baud_rate: 38400
  rx_buffer_size: 8192   # match CONFIG_UART_RX_BUFFER_SIZE above
```

**Pick a value for your board:**

| Board | Recommended RX buffer |
|-------|-----------------------|
| Memory-tight ESP32-S3 (e.g. AtomS3R with display) | 1–2 KB |
| ESP32-S3 with headroom | up to 8 KB |
| ESP32-P4 | 16 KB |

**Why a bigger buffer helps:** at 38400 baud you receive roughly 4800 bytes/second, about 4.8 bytes/ms. An 8 KB buffer holds about 1.7 seconds of data, so a brief 3 ms display refresh or a FreeRTOS task switch can't overflow it. That timing headroom is the whole point.

**The trade-off — don't oversize:** these buffers must live in internal, DMA-capable RAM. They **never** go in PSRAM. Internal RAM is scarce (under ~200 KB total, shared with everything else), so an oversized RX buffer wastes heap you need for the rest of the firmware. Use the smallest value that stops the loss. On a display-equipped AtomS3R, start at 1–2 KB and only go higher if the counter is still climbing.

The TX buffer stays tiny — the component only **listens** to the RS485 bus and never transmits, so the default is plenty.

## 3. Give the CPU back to the UART

On single-core boards like the AtomS3R, the display, I2C LEDs, and UART all share one CPU. Reducing how often the display and LEDs work frees time to drain the buffer.

**Slow the display refresh:**

```yaml
display:
  - platform: st7789v
    update_interval: 5s   # fewer SPI transfers than 1–2s
```

**Slow the display SPI clock** if loss persists:

```yaml
display:
  - platform: st7789v
    data_rate: 20MHz   # down from 40MHz; each refresh takes ~6.4ms vs ~3.2ms
```

**Drop animated LED effects,** which trigger frequent I2C traffic:

```yaml
light:
  - platform: rgb
    name: "RGB LED"
    id: status_led
    # Avoid pulse/strobe effects if you see frame loss —
    # they cause constant I2C updates that compete with UART.
```

## 4. Check whether you need PSRAM

PSRAM is recommended once you track **15 or more devices** — the device list and node table grow with your array and belong in PSRAM, not internal RAM. Enable it in your board config if you're at or above that count.

Higher figures are board-specific: an ESP32-P4 (dual-core, large PSRAM) has been run with 50+ devices while driving a display. That's a ceiling on capable hardware, not a change to the 15+ rule — if you're near or past 15 devices, turn PSRAM on regardless of board.

## Verify it worked

Watch the "Missed Packets" counter in the web UI or Home Assistant after flashing. It should stop climbing (or climb only rarely). In the logs you'll see the RX buffer staying well under its capacity and few or no "Packet missed!" warnings:

```text
[D][tigo_monitor:xxx]: Heap: Internal 150 KB free, PSRAM 7800 KB free, Buffer: 234 bytes
```

**Good signs:** buffer usage stays low, no steady stream of "Packet missed!" warnings, counter is flat.

**Still losing frames?** Work down this list:
- Raise the display `update_interval` (e.g. to 10s).
- Remove animated LED effects.
- Lower the display `data_rate` to 20 MHz.
- Bump the RX buffer one step (and keep both values matched) — but stop as soon as the loss stops, to preserve internal RAM.

## Background

This guidance came out of a real case: an AtomS3R with its display enabled was dropping frames because 40 MHz SPI refreshes and I2C LED updates kept starving the UART buffer on the single core. Enabling the IRAM ISR, matching the RX buffer, and easing off the display fixed it. The component also drains more bytes per processing pass than it used to; that improvement ships built in and isn't a setting you configure.

---

**See also:** [Troubleshooting](./troubleshooting.md) · [Configuration](./configuration.md) · [← Back to README](https://github.com/RAR/esphome-tigomonitor)
