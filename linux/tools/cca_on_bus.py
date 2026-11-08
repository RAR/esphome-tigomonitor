#!/usr/bin/env python3
"""A CCA and its TAP on a pseudo-terminal, as a listening monitor hears them.

usage: cca_on_bus.py PTY_LINK [--reconfigure-after S] [--config-read]

Plays the CCA's steady state: polls and their replies, a discovery-window
read, a text query with its ack, and VERSION with its reply. With
--reconfigure-after, S seconds in it also sends what the CCA's local UI sends
to reconfigure a TAP: a GatewayCfg write (01 00 and a 36-byte block, all
made-up zeros here) and SET_CHANNEL 20 00. With --config-read it starts with
the bring-up's GatewayCfg read: channel 18, SLT 90, SLm 60, and a made-up PAN
(12 34) and key (EE x 16). Then steady state again, until killed. Nothing
here is a real network's config.
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
    ap.add_argument('--config-read', action='store_true')
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

    if a.config_read:
        time.sleep(1)  # let the monitor open the port
        block = (bytes([0x00, 0x12, 0x12, 0x34, 0x0C, 0x02, 0x02, 0x01, 0x00, 0xAA, 0xBB, 0xCC, 0x1C, 0x03, 0x5A])
                 + bytes([0xEE] * 16) + bytes([0x00, 0x00, 0x02, 0x00, 0x3C, 0x00]))
        say([encode(TAP, 0x0B0F, bytes([0, 0, 0, 0x0D, 0xF0, 0x00, 0x00])),
             encode(TAP | 0x8000, 0x0B10, bytes([0x00, 0x2A, 0x00, 0x0E, 0xF0]) + block)])

    while True:
        seq = (seq + 1) & 0xFF
        if seq % 40 == 1:
            say([encode(TAP, 0x000A, b''), encode(TAP | 0x8000, 0x000B, b'Mgate Version G8.59\rJan  1 2026\r')])
        # Now and then a poll brings back optimizer 0003's answer to ^00Version.
        answer = b''
        if seq % 40 == 2:
            text = b'Mnode Version K8.0120 (2D)\r'
            answer = bytes([0x07, 0x00, 0x03, 0x00, 0x00, 0x00, len(text)]) + text
        # And its settings, the roof's usual answer to ^00Smrt.
        if seq % 40 == 3:
            text = b'!Smrt 0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01\r'
            answer = bytes([0x07, 0x00, 0x03, 0x00, 0x00, 0x00, len(text)]) + text
        say([encode(TAP, 0x0148, bytes([0x00, 0x01, cursor >> 8, cursor & 0xFF, 0x04])),
             encode(TAP | 0x8000, 0x0149, bytes([0x00, 0x1F, 0x00, 0x00, 0x00]) + answer)])
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
