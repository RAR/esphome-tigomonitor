#!/usr/bin/env python3
"""A TAP for testing the bus master, on a pseudo-terminal.

    mock_tap.py PTY_LINK [--lease S] [--renew-to S] [--nodes N] [--panels N]
                         [--foreign-after S] [--echo] [--drop-every N]
                         [--start-cursor HEX] [--bare] [--stats FILE]

Answers requests to gateway 0x1201 the way the rig's TAP did in the capture:
  - 0x0148 polls with 0x0149 power frames: up to two synthetic readings per
    reply, and it checks the poll's cursor against how many it has handed out;
  - tunnel 0x2E with the run lease (a countdown from --lease seconds; state
    and the first count drop to 0 when it runs out);
  - tunnel 0x2D, the renewal, by resetting the lease to --renew-to seconds.
A Frame 27 node table goes out after the first poll, so panels get barcodes.
--foreign-after puts a request of its own on the bus after S seconds, as a
second master would. --echo sends every byte the master writes straight back,
as an adapter that hears itself does. --drop-every N leaves every Nth poll
unanswered. --start-cursor gives the TAP a cursor of its own: a poll with any
other cursor gets no data and a reply whose long header states the TAP's
cursor (a guess at what a real TAP does; the capture never shows it).
--bare is a gateway with no mesh behind it, like a spare TAP on the bench: it
answers VERSION and refuses polls and the tunnel with 0x0006, as the analysis
repo's standalone gateways did. Writes what it saw to --stats as JSON every second.
Barcodes are made up.
"""
import argparse
import json
import os
import pty
import random
import select
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from synth_bus import crc16, power_packet  # noqa: E402

GATEWAY = 0x1201
ESCAPED = [0x7E, 0x24, 0x23, 0x25, 0xA4, 0xA3, 0xA5]


def encode(dest, cmd, payload):
    body = bytes([dest >> 8, dest & 0xFF, cmd >> 8, cmd & 0xFF]) + payload
    crc = crc16(body)
    body += bytes([crc >> 8, crc & 0xFF])
    out = bytearray(b'\x00\xff\xff\x7e\x07')
    for b in body:
        out += bytes([0x7E, ESCAPED.index(b)]) if b in ESCAPED else bytes([b])
    return bytes(out + b'\x7e\x08')


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('link')
    ap.add_argument('--lease', type=float, default=900)
    ap.add_argument('--renew-to', type=float, default=900)
    ap.add_argument('--nodes', type=int, default=44)
    ap.add_argument('--panels', type=int, default=6)
    ap.add_argument('--foreign-after', type=float, default=None)
    ap.add_argument('--echo', action='store_true')
    ap.add_argument('--drop-every', type=int, default=0)
    ap.add_argument('--start-cursor', type=lambda v: int(v, 16), default=0)
    ap.add_argument('--bare', action='store_true')
    ap.add_argument('--stats', default=None)
    a = ap.parse_args()

    master_fd, slave_fd = pty.openpty()
    os.system(f'stty -F {os.ttyname(slave_fd)} raw -echo')
    if os.path.lexists(a.link):
        os.unlink(a.link)
    os.symlink(os.ttyname(slave_fd), a.link)

    rng = random.Random(1)
    panels = [(f"04C05B40{0x0B0000 + i * 7:06X}{rng.randint(0, 255):02X}", 0x0002 + i) for i in range(a.panels)]
    start = time.monotonic()
    lease_end = start + a.lease
    handed_out = a.start_cursor
    sent_table = False
    foreign_sent = False
    stats = {'polls': 0, 'retries': 0, 'cursor_mismatches': 0, 'checks': 0, 'renewals': [], 'other': [],
             'first_lease_frame': None, 'first_frame': None, 'frames_after_foreign': 0, 'foreign_at': None}
    buf = bytearray()
    last_stats = 0.0

    def lease_payload(seq):
        left = max(0, int(lease_end - time.monotonic()))
        state, count_a = (1, a.nodes) if left > 0 else (0, 0)
        return bytes([0x00, 0x0E, 0x00, 0x2F, seq, state, left >> 8, left & 0xFF, count_a >> 8, count_a & 0xFF,
                      0x00, 0x28, a.nodes >> 8, a.nodes & 0xFF, 0x00])

    while True:
        now = time.monotonic()
        if a.foreign_after is not None and not foreign_sent and now - start >= a.foreign_after:
            os.write(master_fd, encode(GATEWAY, 0x0148, bytes.fromhex('0001000004')))
            foreign_sent = True
            stats['foreign_at'] = now - start
        if a.stats and now - last_stats >= 1.0:
            with open(a.stats + '.tmp', 'w') as f:
                json.dump(stats, f)
            os.replace(a.stats + '.tmp', a.stats)
            last_stats = now
        r, _, _ = select.select([master_fd], [], [], 0.05)
        if not r:
            continue
        try:
            chunk = os.read(master_fd, 4096)
        except OSError:
            continue
        if a.echo:
            os.write(master_fd, chunk)
        buf += chunk
        while True:
            s = buf.find(b'\x7e\x07')
            e = buf.find(b'\x7e\x08', s + 2) if s >= 0 else -1
            if s < 0 or e < 0:
                break
            body = unescape(bytes(buf[s + 2:e]))
            del buf[:e + 2]
            if len(body) < 6 or crc16(body[:-2]) != (body[-2] << 8 | body[-1]):
                continue
            dest, cmd, p = body[0] << 8 | body[1], body[2] << 8 | body[3], body[4:-2]
            if dest != GATEWAY:
                continue
            t = time.monotonic() - start
            stats['first_frame'] = stats['first_frame'] or f'{cmd:04X}'
            if cmd == 0x000A:
                os.write(master_fd, encode(GATEWAY | 0x8000, 0x000B, b'Mgate G1.0000'))
                continue
            if a.bare and cmd in (0x0B0F, 0x0148):
                os.write(master_fd, encode(GATEWAY | 0x8000, 0x0006, b''))
                continue
            if foreign_sent:
                stats['frames_after_foreign'] += 1
            if cmd == 0x0148 and len(p) == 5:
                stats['polls'] += 1
                if p[1] == 0:
                    stats['retries'] += 1
                cursor = p[2] << 8 | p[3]
                if a.drop_every and stats['polls'] % a.drop_every == 0:
                    stats['dropped'] = stats.get('dropped', 0) + 1
                    continue
                if cursor != handed_out & 0xFFFF:
                    stats['cursor_mismatches'] += 1
                    # 0x41E0: bits 0-4 clear, so the header carries the TAP's whole cursor.
                    reply = bytes.fromhex(f'41E0000E00010200{handed_out & 0xFFFF:04X}4C00')
                    os.write(master_fd, encode(GATEWAY | 0x8000, 0x0149, reply))
                    continue
                n = rng.choice([0, 0, 0, 1, 2])
                packets = ''.join(power_packet(panels[(handed_out + i) % len(panels)][1], rng, 0.8) for i in range(n))
                handed_out += n
                reply = bytes.fromhex(f'41FF{cursor & 0xFF:02X}4B9A' + packets)
                os.write(master_fd, encode(GATEWAY | 0x8000, 0x0149, reply))
                if not sent_table:  # the node table, as a reply from the gateway
                    table = '0000002701' + f'{2:04X}{len(panels):04X}' + ''.join(
                        barcode + f'{addr:04X}' for barcode, addr in panels)
                    os.write(master_fd, encode(GATEWAY | 0x8000, 0x0B10, bytes.fromhex(table)))
                    sent_table = True
            elif cmd == 0x0B0F and len(p) >= 5 and p[3] == 0x2E:
                stats['checks'] += 1
                stats['first_lease_frame'] = stats['first_lease_frame'] or 'check'
                os.write(master_fd, encode(GATEWAY | 0x8000, 0x0B10, lease_payload(p[4])))
            elif cmd == 0x0B0F and len(p) == 14 and p[3] == 0x2D:
                stats['first_lease_frame'] = stats['first_lease_frame'] or 'renew'
                stats['renewals'].append({'t': round(t, 2), 'nodes': p[10] << 8 | p[11]})
                lease_end = time.monotonic() + a.renew_to
                os.write(master_fd, encode(GATEWAY | 0x8000, 0x0B10, lease_payload(p[4])))
            else:
                stats['other'].append(f'{cmd:04X} {p.hex()}')


if __name__ == '__main__':
    main()
