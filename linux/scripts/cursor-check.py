#!/usr/bin/env python3
"""Checks the 0x0148 poll rule against a capture of the CCA running the bus.

The rule (tigo_master.h, PollCursor): the next poll's cursor is the last one
plus the number of power packets in the 0x0149 reply between them, and its
second byte is 01 after an answered poll, 00 after an unanswered one. This
replays every poll in the capture through that rule and passes at 99.9 %.

usage: cursor-check.py [CAPTURE]   (default build/roof/steady-20261026.bin;
                                    skipped when the capture isn't there)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
from bus_frames import frames  # noqa: E402


def packets(reply):
    """Power packets in a 0x0149 payload: header fields by status bit, then
    packets of 7 bytes plus the length at offset 6."""
    status = reply[0] << 8 | reply[1]
    i = 2
    for bit, size in ((0, 1), (1, 1), (2, 2), (3, 2), (4, 1)):
        if not status & (1 << bit):
            i += size
    i += 3  # bit 5 (1 byte) and bit 6 (2 bytes) are always there
    n = 0
    while i + 7 <= len(reply):
        i += 7 + reply[i + 6]
        if i <= len(reply):
            n += 1
    return n


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/roof/steady-20261026.bin'
    if not os.path.exists(path):
        print(f'skipped: no capture at {path}')
        return 0
    good, _ = frames(open(path, 'rb').read())
    polls = [(f[2], f[3]) for f in good if f[2] in (0x0148, 0x0149)]
    cursor = retry = None
    answered = True
    agree = total = 0
    misses = []
    for cmd, p in polls:
        if cmd == 0x0149:
            if cursor is not None:
                cursor = (cursor + packets(p)) & 0xFFFF
            answered = True
            continue
        actual_cursor, actual_retry = p[2] << 8 | p[3], p[1] == 0
        if cursor is not None:
            total += 1
            if (actual_cursor, actual_retry) == (cursor, not answered):
                agree += 1
            elif len(misses) < 10:
                misses.append((cursor, not answered, actual_cursor, actual_retry))
        cursor = actual_cursor  # follow the CCA so one miss doesn't cascade
        answered = False
    print(f'agree {agree} of {total}')
    for m in misses:
        print('  predicted %04X retry=%s, CCA sent %04X retry=%s' % m)
    return 0 if total and agree >= 0.999 * total else 1


if __name__ == '__main__':
    sys.exit(main())
