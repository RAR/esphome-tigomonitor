# Wiring Guide

How to connect the ESP32 inline with your Tigo CCA/TAP system.

## Overview

The Tigo system uses RS485 for communication between the CCA (Cloud Connect Advanced) or TAP (Tigo Access Point) and the solar optimizers. The ESP32 connects to the **GATEWAY** port to passively monitor this communication.

```
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

The ESP32 acts as a **passive listener** – it does not transmit, only receives.

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

```
Tigo CCA/TAP                     ESP32 + RS485 Base                    Tigo Optimizers
┌──────────┐                    ┌──────────────────┐                   ┌──────────────┐
│          │                    │                  │                   │              │
│   RS485  │───── A ───────────►│ A (Terminal)     │◄────── A ─────────│   RS485      │
│   Port   │───── B ───────────►│ B (Terminal)     │◄────── B ─────────│   Bus        │
│          │                    │                  │                   │              │
│   GND    │─────────(optional)─│ GND              │                   │              │
└──────────┘                    └──────────────────┘                   └──────────────┘
                                        │
                                        │ Internal
                                        ▼
                                ┌──────────────────┐
                                │  AtomS3R ESP32   │
                                │  TX: GPIO6       │
                                │  RX: GPIO5       │
                                └──────────────────┘
```

### Wiring Table

| Connection | From | To |
|------------|------|-----|
| RS485 A | CCA/TAP A terminal | RS485 Base A terminal |
| RS485 A | RS485 Base A terminal | Optimizer bus A |
| RS485 B | CCA/TAP B terminal | RS485 Base B terminal |
| RS485 B | RS485 Base B terminal | Optimizer bus B |
| GND | CCA/TAP GND (optional) | RS485 Base GND |

**Note:** RS485 A and B are daisy-chained through the ESP32 module. The ESP32 monitors traffic in both directions.

## Safety Notes

⚠️ **Important:**
- Disconnect power before wiring
- RS485 is low voltage (typically 5V differential) but verify your system
- The ESP32 is passive – it cannot interfere with Tigo operation
- Do not connect ESP32 TX to the bus if you want read-only operation

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
5. Tie MAX485 DE and RE to GND
6. Connect A/B to Tigo RS485 bus
