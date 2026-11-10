#!/usr/bin/env python3
"""Lists the frames in a bus capture: one line per frame with a good CRC.

Two kinds of file: the raw bytes tigomonitord --capture or `cat
/dev/serial0` writes, and the 2026-11-06 bench recorder's, where each read
is a record behind a 10-byte header (ms since the epoch as a u16 low part
and a u32 high part, a u16, and a u16 length, all little-endian). Read raw,
a frame that straddles two records fails its CRC. The reading that gives
fewer bad frames is used, and a recorded file's times come from its
headers.

usage: bus_frames.py CAPTURE [--seconds S] [--requests] [--sub HEX] [--pairs]

  --seconds S   how long a raw capture ran (default 1200), to turn offsets into time
  --requests    only the master's requests (DEST without the 0x8000 bit)
  --sub HEX     only tunnel requests with this sub, and replies with sub + 1
  --pairs       each request followed by the next reply whose CMD is CMD+1

Standalone, with no import from tigomonitord or the analysis repo, so it runs in
the dev container and in CI.
"""
import sys

ESCAPED = [0x7E, 0x24, 0x23, 0x25, 0xA4, 0xA3, 0xA5]


def crc16(data):
    crc = 0x8408
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def unescape(b):
    out, i = bytearray(), 0
    while i < len(b):
        if b[i] == 0x7E and i + 1 < len(b) and b[i + 1] < 7:
            out.append(ESCAPED[b[i + 1]])
            i += 2
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def frames(data):
    """(offset, dest, cmd, payload) for each frame with a good CRC; also the bad count."""
    good, bad, pos = [], 0, 0
    while True:
        s = data.find(b'\x7e\x07', pos)
        if s < 0:
            break
        e = data.find(b'\x7e\x08', s + 2)
        if e < 0:
            break
        body = unescape(data[s + 2:e])
        pos = e + 2
        # The CRC goes on the wire low byte first.
        if len(body) < 6 or crc16(body[:-2]) != (body[-2] | body[-1] << 8):
            bad += 1
            continue
        good.append((s, body[0] << 8 | body[1], body[2] << 8 | body[3], body[4:-2]))
    return good, bad


def records(raw):
    """A recorded capture's bytes with the headers stripped, and (offset, unix s)
    for each record; None if raw doesn't parse as records end to end."""
    out, marks, i = bytearray(), [], 0
    while i + 10 <= len(raw):
        lo, hi, n = int.from_bytes(raw[i:i + 2], 'little'), int.from_bytes(raw[i + 2:i + 6], 'little'), \
            int.from_bytes(raw[i + 8:i + 10], 'little')
        if i + 10 + n > len(raw):
            return None
        marks.append((len(out), (hi << 16 | lo) / 1000.0))
        out += raw[i + 10:i + 10 + n]
        i += 10 + n
    return (bytes(out), marks) if i == len(raw) and marks else None


def load(raw):
    """(bytes, good frames, bad count, time of an offset): raw or recorded, whichever reads better."""
    good, bad = frames(raw)
    rec = records(raw)
    if rec:
        data, marks = rec
        g2, b2 = frames(data)
        if b2 < bad:
            offs = [m[0] for m in marks]

            def at(off):
                import bisect
                return marks[max(0, bisect.bisect_right(offs, off) - 1)][1] - marks[0][1]
            return data, g2, b2, at
    return raw, good, bad, None


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith('-'):
        print(__doc__)
        return 2
    data, good, bad, at = load(open(args[0], 'rb').read())
    seconds = float(args[args.index('--seconds') + 1]) if '--seconds' in args else 1200.0
    sub = int(args[args.index('--sub') + 1], 16) if '--sub' in args else None
    rate = len(data) / seconds
    print(f'{len(data)} bytes, {len(good)} good frames, {bad} bad' + (' (recorded: headers stripped)' if at else ''))

    def wanted(f):
        _, dest, cmd, p = f
        if '--requests' in args and dest & 0x8000:
            return False
        # A request matches its sub exactly; a reply, sub + 1 (pair them with --pairs).
        want = sub if cmd == 0x0B0F else (sub + 1 if sub is not None else None)
        if sub is not None and not (cmd in (0x0B0F, 0x0B10) and len(p) > 3 and p[3] == want):
            return False
        return True

    def line(f, tag=''):
        s, dest, cmd, p = f
        t = at(s) if at else s / rate
        return f'{tag}@{s:>9} {t:8.1f}s {dest:04X} {cmd:04X} [{len(p)}] {p.hex(" ")}'

    for i, f in enumerate(good):
        if not wanted(f) or (('--pairs' in args) and f[1] & 0x8000):
            continue
        print(line(f))
        if '--pairs' in args:
            reply = next((g for g in good[i + 1:i + 50] if g[2] == f[2] + 1 and g[1] & 0x8000), None)
            if reply:
                print(line(reply, '   -> '))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
