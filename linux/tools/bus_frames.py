#!/usr/bin/env python3
"""Lists the frames in a raw bus capture (the bytes tigomonitord --capture or
`cat /dev/serial0` writes): one line per frame with a good CRC.

usage: bus_frames.py CAPTURE [--seconds S] [--requests] [--sub HEX] [--pairs]

  --seconds S   how long the capture ran (default 1200), to turn offsets into time
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


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith('-'):
        print(__doc__)
        return 2
    data = open(args[0], 'rb').read()
    seconds = float(args[args.index('--seconds') + 1]) if '--seconds' in args else 1200.0
    sub = int(args[args.index('--sub') + 1], 16) if '--sub' in args else None
    rate = len(data) / seconds
    good, bad = frames(data)
    print(f'{len(data)} bytes, {len(good)} good frames, {bad} bad')

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
        return f'{tag}@{s:>9} {s / rate:8.1f}s {dest:04X} {cmd:04X} [{len(p)}] {p.hex(" ")}'

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
