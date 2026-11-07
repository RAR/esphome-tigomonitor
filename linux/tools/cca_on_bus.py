#!/usr/bin/env python3
"""A CCA and its TAP on a pseudo-terminal, as a listening monitor hears them.

usage: cca_on_bus.py PTY_LINK [--reconfigure-after S]

Plays the CCA's steady state: polls and their replies, a discovery-window
read, a text query with its ack, and VERSION with its reply. With
--reconfigure-after, S seconds in it also sends what the CCA's local UI sends
to reconfigure a TAP: a GatewayCfg write (01 00 and a 36-byte block, all
made-up zeros here) and SET_CHANNEL 20 00. Then steady state again, until
killed. Nothing here is a real network's config.
"""
import argparse
import os
import pty
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from mock_tap import encode  # noqa: E402

TAP = 0x1201


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('link')
    ap.add_argument('--reconfigure-after', type=float, default=None)
    a = ap.parse_args()
    master, slave = pty.openpty()
    if os.path.lexists(a.link):
        os.unlink(a.link)
    os.symlink(os.ttyname(slave), a.link)
    start = time.monotonic()
    reconfigured = False
    cursor = 0x0100
    seq = 0

    def say(frames):
        for f in frames:
            os.write(master, f)
            time.sleep(0.02)

    while True:
        seq = (seq + 1) & 0xFF
        if seq % 40 == 1:
            say([encode(TAP, 0x000A, b''), encode(TAP | 0x8000, 0x000B, b'Mgate Version G8.59\rJan  1 2026\r')])
        say([encode(TAP, 0x0148, bytes([0x00, 0x01, cursor >> 8, cursor & 0xFF, 0x04])),
             encode(TAP | 0x8000, 0x0149, bytes([0x00, 0x1F, 0x00, 0x00, 0x00]))])
        cursor += 1
        if seq % 10 == 0:
            say([encode(TAP, 0x0B0F, bytes([0, 0, 0, 0x2E, seq])),
                 encode(TAP | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x2F, seq, 0x01, 0x03, 0x84, 0x00, 0x02,
                                                     0x00, 0x02, 0x00, 0x02, 0x00]))])
        if seq % 25 == 0:
            say([encode(TAP, 0x0B0F, bytes([0, 0, 0, 0x06, seq, 0x00, 0x02]) + b'^00Info\r'),
                 encode(TAP | 0x8000, 0x0B10, bytes([0x00, 0x0D, 0x00, 0x07, seq]))])
        if a.reconfigure_after is not None and not reconfigured and time.monotonic() - start >= a.reconfigure_after:
            reconfigured = True
            say([encode(TAP, 0x0B0F, bytes([0, 0, 0, 0x0D, seq, 0x01, 0x00]) + bytes(36)),
                 encode(TAP | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x0E, seq, 0x01])),
                 encode(TAP, 0x000C, bytes([0x20, 0x00])),
                 encode(TAP | 0x8000, 0x000D, b'')])
        time.sleep(0.05)


if __name__ == '__main__':
    main()
