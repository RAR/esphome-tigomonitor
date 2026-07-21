# Wiring Guide

Wire an ESP32 (recommended: M5Stack AtomS3R) inline with your Tigo CCA/TAP RS485 bus so it can passively monitor optimizer telemetry — written for solar owners doing the install themselves.

> ⚠️ **Warning — this bus lives inside a high-voltage PV system.**
> The RS485 *signal* is low voltage, but you tap it inside or next to a Tigo CCA/TAP and inverter that are part of a **high-voltage PV DC system**. That DC can be **lethal**, and it is **not** switched off just because you turn the inverter off — sunlit panels keep producing voltage.
>
> Before opening any enclosure:
> - **De-energize and isolate the PV array and inverter** using the DC disconnect / isolator, per your inverter's shutdown procedure. Turning off the inverter alone does **not** make the array safe.
> - **Treat the inverter side as live high-voltage DC** until you have verified it is isolated.
> - **If you are not comfortable or qualified, stop** and involve a licensed solar installer or electrician. There is no shame in it and it may be a legal requirement where you live.

## Overview

The Tigo system uses RS485 for communication between the CCA (Cloud Connect Advanced) or TAP (Tigo Access Point) and the solar optimizers. The ESP32 connects to the **GATEWAY** port to passively monitor this communication.

```text
┌─────────────────────────────────────┐      ┌────────────────────────────┐
│                 CCA                 │      │            TAP             │
│                                     │      │                            │
│ AUX  RS485-1  GATEWAY  RS485-2 POWER│      │                    ┌~┐     │
│┌─┬─┐ ┌─┬─┬─┐ ┌─┬─┬─┬─┐ ┌─┬─┬─┐ ┌─┬─┐│      │   ┌─┬─┬─┬─┐   ┌─┬─┬│┬│┐    │
││/│_│ │-│B│A│ │-│+│B│A│ │-│B│A│ │-│+││      │   │-│+│B│A│   │-│+│B│A│    │
│└─┴─┘ └─┴─┴─┘ └│┴│┴│┴│┘ └─┴─┴─┘ └─┴─┘│      │   └│┴│┴│┴│┘   └─┴─┴─┴─┘    │
└───────────────│─│─│─│───────────────┘      └────│─│─│─│─────────────────┘
                │ │ │ │                           │ │ │ │
                │ │ │ ┃───────────────────────────│─│─│─┘
                │ │ ┃─┃───────────────────────────│─│─┘
                │ └─┃─┃───────────────────────────│─┘
                ┃───┃─┃───────────────────────────┘
                ┗━┓ ┃ ┃
              ┌───┃─┃─┃───┐
              │  ┌┃┬┃┬┃┐  │
              │  │-│B│A│  │
              │  └─┴─┴─┘  │
              │  Monitor  │
              └───────────┘
```

*Diagram credit: [willglynn/taptap](https://github.com/willglynn/taptap/blob/main/README.md)*

The ESP32 acts as a **passive listener** – it only receives, and it is wired so that it *physically cannot* transmit onto the bus. See [Passive-listener wiring](#why-this-is-read-only) below for the electrical reason why.

---

## Hardware Required

| Component | Recommended | Notes |
|-----------|-------------|-------|
| ESP32 Board | M5Stack AtomS3R | 8MB PSRAM for 15+ devices |
| RS485 Adapter | M5Stack Atomic RS485 Base | Built-in level shifter |
| Wiring | 22-24 AWG twisted pair | For RS485 A/B connections |

**Alternative RS485 adapters:**
- MAX485 module
- SP3485 module
- Any TTL-to-RS485 converter

---

## Wiring Diagram

### M5Stack AtomS3R + Atomic RS485 Base

The Atomic RS485 Base plugs directly onto the AtomS3R. Connect RS485 terminals:

```text
Tigo CCA/TAP                     ESP32 + RS485 Base                    Tigo Optimizers
┌──────────┐                    ┌──────────────────┐                   ┌──────────────┐
│          │                    │                  │                   │              │
│   RS485  │───── A ───────────►│ A (Terminal)     │◄────── A ─────────│   RS485      │
│   Port   │───── B ───────────►│ B (Terminal)     │◄────── B ─────────│   Bus        │
│          │                    │                  │                   │              │
│   GND    │────── GND ─────────│ GND              │                   │              │
└──────────┘                    └──────────────────┘                   └──────────────┘
                                        │
                                        │ Internal
                                        ▼
                                ┌──────────────────┐
                                │  AtomS3R ESP32   │
                                │  TX: GPIO6       │
                                │  RX: GPIO5       │
                                │  38400 baud 8N1  │
                                └──────────────────┘
```

TX is wired but inert — the transceiver's driver is held disabled, so the ESP32 only listens. See [Why this is read-only](#why-this-is-read-only).

### Wiring Table

| Connection | From | To |
|------------|------|-----|
| RS485 A | CCA/TAP A terminal | RS485 Base A terminal |
| RS485 A | RS485 Base A terminal | Optimizer bus A |
| RS485 B | CCA/TAP B terminal | RS485 Base B terminal |
| RS485 B | RS485 Base B terminal | Optimizer bus B |
| GND | CCA/TAP GND | RS485 Base GND |

**Note:** RS485 A and B are daisy-chained through the ESP32 module. The ESP32 monitors traffic in both directions.

**Ground reference:** RS485 needs a **common ground reference** between the ESP32 and the Tigo bus for reliable signalling — connect GND. It is not optional. (Skip it only if the ESP32 and the Tigo equipment already share a ground through another path, and even then a dedicated GND wire is the safer default.)

### Why this is read-only

The AtomS3R UART wires TX to GPIO6, and the MAX485 example below wires the driver input (DI) to that same TX pin — yet the ESP32 still **cannot** transmit onto the bus. Here's why:

- A MAX485-style transceiver only drives the A/B lines when its **driver-enable (DE)** pin is HIGH and its **receiver-enable (RE)** pin is LOW.
- In every wiring here, **DE and RE are tied LOW**. That holds the driver **permanently disabled** — its A/B outputs are high-impedance no matter what TX/DI does.
- So wiring TX/DI is harmless: with the driver disabled, whatever the ESP32 sends on TX simply goes nowhere. The receiver stays enabled and you listen only.

This is what guarantees passive, read-only operation — it's the disabled driver, not an unwired TX pin.

## Safety Notes

> ⚠️ **Warning — high-voltage PV DC.**
> - **De-energize and isolate the PV array and inverter before opening any enclosure or touching wiring.** PV DC can be **lethal** and is **not** switched off by turning the inverter off — panels in daylight keep producing voltage.
> - **Treat the inverter side as live high-voltage DC** until you have confirmed isolation with the proper disconnect.
> - **If you are not comfortable or qualified, involve a licensed solar installer or electrician.**

Once the high-voltage side is safely isolated:

- The RS485 **signal** is low voltage (typically 5V differential), but verify your system before assuming anything is safe to touch.
- Connect a **common GND** between the ESP32 and the Tigo bus so RS485 has a shared reference.
- The ESP32 is passive – with the transceiver's driver disabled (DE/RE tied LOW) it physically cannot transmit, so it cannot interfere with Tigo operation.

---

## Example Setups

### M5Stack AtomS3R + Atomic RS485 Base

Simplest setup – just plug together and connect RS485 terminals:

1. Attach AtomS3R to Atomic RS485 Base
2. Connect A terminal to Tigo RS485 A (daisy-chain)
3. Connect B terminal to Tigo RS485 B (daisy-chain)
4. Power via USB-C
5. Flash ESPHome configuration

### ESP32-DevKit + MAX485 Module

Budget option:

1. Wire MAX485 VCC to ESP32 3.3V
2. Wire MAX485 GND to ESP32 GND
3. Wire MAX485 RO to ESP32 GPIO5 (RX)
4. Wire MAX485 DI to ESP32 GPIO6 (TX)
5. **Tie MAX485 DE and RE to GND** — this disables the driver and is what makes the ESP32 read-only (step 4 is then harmless; see [Why this is read-only](#why-this-is-read-only))
6. Connect A/B to Tigo RS485 bus, and share a common GND with the bus

The UART runs at **38400 baud, 8N1** on TX=GPIO6 / RX=GPIO5.

---

**See also:** [Troubleshooting](TROUBLESHOOTING.md) · [Configuration](CONFIGURATION.md) · [← Back to README](../README.md)
