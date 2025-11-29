# Taptap Protocol Research Notes

## Overview
This document captures research findings about the Taptap proprietary protocol used by Tigo gateways for communication with solar optimizers and the Cloud Connect Advanced (CCA) device.

## Frame Structure

### Controller vs Gateway Frames
**Critical Discovery:** Controllers (CCA, ESP32) and gateways use different preambles:

- **Controller Preamble:** `00 FF FF` (must precede all frames sent TO gateway)
- **Gateway Preamble:** `FF` (used for frames FROM gateway)

Complete frame format:
```
[Preamble] [0x7E 0x07] [Address] [Type] [Prefix 00 00 00] [PV_Type] [Seq] [Payload] [CRC16] [0x7E 0x08]
```

**Example controller frame:**
```
00 FF FF 7E 07 12 01 0B 0F 00 00 00 0D 01 00 00 E7 9B 7E 08
└──┬───┘ └──┬──┘ └──┬──┘ └──┬──┘ └────┬────┘ └┬┘ └┬┘ └─┬──┘ └─┬──┘ └──┬──┘
   │        │       │       │         │       │   │    │      │      │
Controller  Start   Dest    Type    Prefix   Type Seq Payload CRC    End
Preamble   Delim   Addr   (0B0F)  (000000)  (0D) (1) (0000) (CCITT) Delim
```

## CCA Startup Sequence

When the CCA powers up or reconnects, it performs a complete authentication handshake with the gateway:

### 1. Broadcast Poll Phase (Frame 0x14)
- CCA sends repeated broadcast frames with `000000` prefix
- Frame format: `00 00 00 14 37 24 92 66 12 35`
- Typically 5 broadcasts at ~750ms intervals
- Purpose: Announce presence on bus, check for gateway response

### 2. Authentication Step 1 (Frames 0x3A/0x3B)
**Direct-addressed frames (not 0B0F command frames)**

Request (0x3A):
```
Dest: 1201 (or 1235, 1202 depending on gateway)
Type: 003A
Payload: (minimal or none)
```

Response (0x3B) from gateway:
```
Source: 9201 (gateway response address)
Type: 003B
Payload: 04C05B3000037DFB1201
         └────────┬────────┘
           Challenge data
```

**Key Discovery:** Gateway sends challenge data that must be echoed back in next step.

### 3. Authentication Step 2 (Frames 0x3C/0x3D)
Request (0x3C):
```
Dest: 1201
Type: 003C
Payload: 3724926604C05B3000037DFB1202
         └──┬───┘└────────┬────────┘
           Unknown  Challenge echo from 0x3B
```

Response (0x3D): Gateway acknowledges (typically short/empty payload)

### 4. Gateway Info Request (Frames 0x0A/0x0B)
Request (0x0A):
```
Dest: 1201
Type: 000A
Payload: (none or minimal)
```

Response (0x0B) - Gateway Firmware Version:
```
Source: 9201
Type: 000B
Payload: 4D676174652056657273696F6E2047382E35390D4A756C20203620323032300D...
         └────────────────────────┬──────────────────────────────┘
                          ASCII firmware version string
```

**Decoded example:** 
```
Mgate Version G8.59 | Jul  6 2020 | 16:51:51 | GW-H158.4.3S0.12
```

Carriage returns (0x0D) separate fields in the version string.

### 5. Unknown Handshake (Frames 0x0B00/0x0B01)
```
Request:  12010B0000
Response: 92010B0100
```
Purpose unknown, but appears consistently after gateway info exchange.

### 6. Final Authentication (Frames 0x0D/0x0E)
**Command frames (0B0F/0B10 segment)**

First 0x0D request:
```
Segment: 0B0F (command request)
Type: 0x0D
Seq: (varies)
Payload: 0000
```

Second 0x0D request (600ms later):
```
Segment: 0B0F
Type: 0x0D
Seq: (seq+1)
Payload: 0001
```

Gateway Response (0x0E):
```
Segment: 0B10 (command response)
Type: 0x0E
Seq: (matching request)
Payload: 0012B94A0C020201000000001C015AF24745B3D8B23EE7926645FBCB6B2BF2000002003C00
         └┬┘└──┬──┘└──────────────────┬──────────────────┘└────┬────┘
         Ch PAN_ID        Encryption Key (16 bytes)      Unknown (4 bytes)
```

**Decoded:**
- Channel: 0x12 (18)
- PAN ID: 0xB94A
- Encryption Key: 1C015AF24745B3D8B23EE7926645FBCB6B2BF2 (128-bit key for ZigBee/RF layer)

## Command Frame Types

### Device Query Frames (0x06/0x07)
Used to query individual devices:

Request (0x06):
```
Segment: 0B0F
Type: 0x06
Seq: (varies)
Payload: [device_addr] [query_string]

Examples:
- 00065E303056657273696F6E0D  → Query device 0x0006 for "^00Version\r"
- 00195E3030496E666F0D        → Query device 0x0019 for "^00Info\r"
- 00175E3030536D72740D        → Query device 0x0017 for "^00Smrt\r"
- 00135E303054657374730D      → Query device 0x0013 for "^00Tests\r"
```

Response (0x07):
```
Segment: 0B10
Type: 0x07
Seq: (matching request)
Payload: (varies - often empty for non-version queries)
```

**Note:** CCA cycles through devices requesting version, info, tests, etc. Most return empty responses except version queries.

### Device Discovery (Frames 0x26/0x27)
Request (0x26):
```
Segment: 0B0F
Type: 0x26
Seq: (varies)
Payload: 0000 (offset, typically starts at 0)
```

Response (0x27):
```
Segment: 0B10
Type: 0x27
Seq: (matching)
Payload: [device_count (2 bytes)] [device_entries (12 bytes each)]
```

Device entry format (12 bytes):
- Unknown field (varies)
- Device address
- Status/flags

### Network Status (Frames 0x2E/0x2F)
Request (0x2E):
```
Segment: 0B0F
Type: 0x2E
Seq: (varies)
Payload: (none or minimal)
```

Response (0x2F):
```
Segment: 0B10
Type: 0x2F
Seq: (matching)
Payload: 010308002400250024
         └┬┘└┬┘└┬┘└──┬──┘
         Status and network metrics (meaning TBD)
```

### Broadcast Command (Frames 0x22/0x23)
Request (0x22):
```
Segment: 0B0F
Type: 0x22
Seq: (varies)
Payload: 0000 (typical)
```

Response (0x23):
```
Segment: 0B10
Type: 0x23
Seq: (matching)
Payload: 02 (or other status bytes)
```

Purpose appears to be periodic keepalive or sync.

### Unknown Commands
- **Frame 0x17:** Seen in both request and response, short payload
- **Frame 0x41:** Seen with 25-byte payload (structure unknown)

## Sequence Number Behavior

**Key Discovery:** CCA maintains a continuously incrementing sequence counter (0x00-0xFF, wraps to 0x00).

**Collision Avoidance Observed:**
When ESP32 transmits frames on the bus, the CCA detects them and increments its own sequence counter to avoid collisions:

```
Time    Device   Frame        Seq
------  ------   ----------   ---
T+0     ESP32    0x0D req     0x67
T+443ms CCA      0x2E req     0x67  ← CCA used same seq (natural increment)
T+600ms ESP32    0x0D req     0x68
T+600ms ESP32    0x26 req     0x69
T+2500ms CCA     0x06 req     0x68  ← CCA skipped to 0x68 (saw our 0x68 traffic)
```

The CCA appears to monitor bus traffic and adjust its sequence to avoid reusing recently-seen sequence numbers.

## Gateway Response Behavior

### Critical Finding: Exclusive Controller Binding
**The gateway appears to maintain an exclusive session with whichever controller completes the authentication handshake first.**

Evidence:
1. **With CCA authenticated:** Gateway responds immediately to CCA's Frame 0x06, 0x2E, etc.
2. **ESP32 attempts with correct frames:** No responses to Frame 0x0D, 0x26, or any command frames
3. **Protocol-perfect frames:** ESP32 transmissions are structurally identical to CCA's (verified byte-by-byte)
4. **No distinguishing features:** RS-485 has no MAC addresses, session tokens not visible in frames

**Hypothesis:** Gateway maintains stateful session after authentication handshake completion. Session is exclusive - only one authenticated controller at a time. Gateway silently ignores command frames from non-authenticated sources.

**Session timeout:** Unknown duration. After CCA disconnection, gateway still doesn't respond to ESP32 frames even with correct auth sequence numbers.

## Hardware/Electrical Discoveries

### RS-485 Module Requirements
**Critical:** Isolated RS-485 modules (like SH-U12) can cause issues:

**Problem with isolation:**
- No common ground reference between controller and gateway
- With CCA connected: CCA provides common-mode voltage reference → works
- With CCA disconnected: Bus floats, no ground reference → 0 bytes received

**Solution:** Use non-isolated RS-485 module (MAX485/MAX3485) with:
- Direct ground connection between controller and gateway
- DE/RE flow control pins for reliable TX/RX switching
- Proper termination (120Ω at both ends of bus)

**Observed:** With isolated module, RX receives 0 bytes when CCA is disconnected, even though TX is functioning (verified by CCA re-broadcasting frames when connected).

### Echo Behavior
**Discovery:** "Echo" observed in testing was NOT from ESP32's own transceiver, but from **CCA re-broadcasting** received frames.

Evidence:
- With CCA connected: See "echo" (20/20 bytes)
- With CCA disconnected: 0 echo bytes
- RX definitely works: Receives CCA and gateway traffic perfectly

**Implication:** Many RS-485 transceivers do NOT provide loopback of own transmissions. The SH-U12 and similar modules suppress self-echo to avoid RX buffer pollution.

## Escape Sequences

Taptap protocol uses escape sequences for special bytes within frame data:

```
Escaped Byte    Escape Sequence
------------    ---------------
0x7E            7E 00
0x24            7E 01
0x23            7E 02
0x25            7E 03
0xA4            7E 04
0xA3            7E 05
0xA5            7E 06
```

**Note:** Escape sequences only apply to payload data between frame delimiters, NOT to the delimiters themselves (0x7E 0x07 / 0x7E 0x08) or controller preamble.

## CRC Calculation

CRC-16-CCITT with polynomial 0x8408 (reversed 0x1021):
- Initial value: 0xFFFF
- Calculated over entire frame from start delimiter through payload
- 2-byte CRC appended before end delimiter
- Little-endian byte order

## Next Steps for Active Querying

To successfully query the gateway without CCA:

1. **Hardware:** Use non-isolated MAX485/MAX3485 module with ground connection
2. **Implement full auth sequence:**
   - Broadcast polls (Frame 0x14)
   - Auth step 1 (Frame 0x3A, receive 0x3B challenge)
   - Auth step 2 (Frame 0x3C with challenge echo)
   - Gateway info (Frame 0x0A)
   - Unknown handshake (Frame 0x0B00)
   - Final auth (Frame 0x0D twice with payloads 0000, 0001)
3. **Session management:** Maintain authenticated state, handle timeouts
4. **Challenge-response:** Store gateway's 0x3B response data, echo in 0x3C request

Alternatively, **passive monitoring** works reliably: Capture CCA's Frame 0x27 device discovery responses when CCA polls the gateway.

## Reference CCA Startup Capture

Complete CCA startup sequence (from logs):

```
[Broadcast Poll x5]
000000140000...   (repeated ~5 times at 750ms intervals)

[Auth Step 1]
REQ: 1201003A...
RSP: 9201003B04C05B3000037DFB1201

[Auth Step 2]  
REQ: 1201003C3724926604C05B3000037DFB1202
RSP: 9201003D...

[Gateway Info]
REQ: 1201000A...
RSP: 9201000B4D676174652056657273696F6E2047382E35390D...
     (Decoded: "Mgate Version G8.59 | Jul  6 2020 | 16:51:51 | GW-H158.4.3S0.12")

[Unknown Handshake]
REQ: 12010B0000
RSP: 92010B0100

[Final Auth]
REQ: 12010B0F0000000D010000     (Frame 0x0D, seq=01, payload=0000)
RSP: 92010B10000E000E010012B94A0C020201000000001C015AF24745B3D8B23EE7926645FBCB6B2BF2000002003C00

REQ: 12010B0F0000000D020001     (Frame 0x0D, seq=02, payload=0001)
(Response to second 0x0D not always logged, assumed similar to first)

[Session Active - Normal Operations Begin]
```

---

**Document Version:** 1.0  
**Last Updated:** November 28, 2024  
**Hardware Tested:** Tigo GW-H158.4.3S0.12, CCA (Cloud Connect Advanced)  
**Controller Tested:** ESP32-S3 AtomS3R with RS-485 modules SH-U12 (isolated), Waveshare MAX485 (non-isolated)
