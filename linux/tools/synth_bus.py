#!/usr/bin/env python3
"""Write a synthetic Tigo RS485 byte stream, for testing without a bus.

    synth_bus.py OUT [--panels N] [--rounds R] [--seed S]

The stream is what a listener on the bus would see: a Frame 27 node table,
then R rounds of power frames, each carrying one reading per panel, with some
noise (a corrupted frame, a stray end delimiter) thrown in. Barcodes are made
up — none belong to a real installation.
"""
import argparse
import random

ESCAPES = {0x7E: 0x00, 0x24: 0x01, 0x23: 0x02, 0x25: 0x03, 0xA4: 0x04, 0xA3: 0x05, 0xA5: 0x06}


def crc16(data: bytes) -> int:
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
        table.append(crc)
    crc = 0x8408
    for b in data:
        crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
    return ((crc >> 8) | (crc << 8)) & 0xFFFF


def wire(hex_body: str) -> bytes:
    raw = bytes.fromhex(hex_body)
    crc = crc16(raw)
    raw += bytes([crc >> 8, crc & 0xFF])
    out = bytearray(b"\x7E\x07")
    for b in raw:
        if b in ESCAPES:
            out += bytes([0x7E, ESCAPES[b]])
        else:
            out.append(b)
    return bytes(out + b"\x7E\x08")


def frame27(start: int, entries) -> bytes:
    body = "1234" "0B10" "0000" "0027" "01" + f"{start:04X}{len(entries):04X}"
    for barcode, addr in entries:
        body += barcode + f"{addr:04X}"
    return wire(body)


def power_packet(addr: int, rng: random.Random, sun: float) -> str:
    vin = int((30 + rng.uniform(-1, 1)) / 0.05)
    iin = int(max(0.0, 9.0 * sun + rng.uniform(-0.1, 0.1)) / 0.005)
    vout = int((vin * 0.05 * rng.uniform(0.97, 1.0)) / 0.10)
    temp = int((20 + 20 * sun + rng.uniform(-1, 1)) / 0.1) & 0xFFF
    rssi = rng.randint(120, 220)
    return (f"31{addr:04X}{addr:04X}000F{vin & 0xFFF:03X}{vout & 0xFFF:03X}FF"
            f"{iin & 0xFFF:03X}{temp:03X}830064{rng.randint(0, 0xFFFF):04X}{rssi:02X}0000")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--panels", type=int, default=12)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    panels = [(f"04C05B40{0x0A0000 + i * 7:06X}{rng.randint(0, 255):02X}", 0x0002 + i) for i in range(a.panels)]
    out = bytearray(b"noise before sync")
    for start in range(0, len(panels), 12):  # the gateway sends 12 entries per chunk
        out += frame27(start + 2, panels[start:start + 12])
    for rnd in range(a.rounds):
        sun = 0.5 + 0.4 * (rnd / max(1, a.rounds - 1))
        for start in range(0, len(panels), 5):
            body = "1234" "0149" "FFFF" "000000" + "".join(
                power_packet(addr, rng, sun) for _, addr in panels[start:start + 5])
            out += wire(body)
        if rnd == 3:
            bad = bytearray(wire("1234" "0149" "FFFF" "000000" + power_packet(panels[0][1], rng, sun)))
            bad[10] ^= 0x01  # corrupted in transit: must fail its checksum
            out += bad
        if rnd == 5:
            out += b"\x00\x7E\x08"  # an end whose start was lost
    with open(a.out, "wb") as f:
        f.write(out)
    print(f"wrote {len(out)} bytes: {a.panels} panels, {a.rounds} rounds")


if __name__ == "__main__":
    main()
