#!/usr/bin/env python3
"""Several TAPs on one bus, for testing the bus master with more than one,
on a pseudo-terminal (docs/superpowers/specs/2026-11-10-multi-tap-design.md).

    mock_taps.py PTY_LINK --tap ID,MAC[,PANELS[,new]] [--tap ...]
                 [--down N,AT,FOR] [--down-file N,PATH] [--reset-quiet S] [--join-after S]
                 [--stats FILE]

Each --tap is a TAP at RS485 id ID (hex; 1235 is the factory address), with
a MAC ending in the hex byte MAC and PANELS optimizers behind it (3). "new"
means it keeps no id through a RESET, as a TAP never stored has none: it
goes back to the factory address. --down takes TAP number N (from 1) off the
bus AT seconds in, for FOR seconds (0: for good); --down-file, while PATH
exists.

Every TAP answers at its own id, as mock_tap.py's one does: polls with
readings for its own optimizers and, once, a Frame 27 naming them; its
discovery window (2E, renewed by 2D); VERSION, 000E/0E02, the GatewayCfg
read, 0B00, 22, 41 and table pages (26), writes (29) and erases (2B). The
bring-up goes to all of them, as on RS485: SET_485_ID moves every TAP to the
factory address, GET_MAC there is answered by each, SET_ID_BY_MAC moves the
one with that MAC, STORE keeps a TAP's id, and RESET puts each back at its
kept id (a new one at the factory address), deaf for --reset-quiet seconds.
Optimizer ids run on across the TAPs, as a system's do. MACs and barcodes
are made up.
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
from mock_tap import encode, unescape  # noqa: E402
from synth_bus import crc16, power_packet  # noqa: E402

FACTORY = 0x1235


class Tap:
    def __init__(self, n, tid, mac_last, panels, new, first_node, rng):
        self.n = n
        self.id = tid
        self.kept = None if new else tid  # where a RESET puts it
        self.mac = bytes.fromhex('04c05b0000a1b2') + bytes([mac_last])
        self.panels = [(f"04C05B40{0x0C0000 + (first_node + i) * 7:06X}{rng.randint(0, 255):02X}", first_node + i)
                       for i in range(panels)]
        self.handed_out = 0
        self.sent_table = False
        self.window_end = time.monotonic() + 600
        self.written = []  # [mac hex, id, joins at]
        self.erased = False
        self.down = False
        self.st = {'n': n, 'mac': self.mac.hex(), 'polls': 0, 'checks': 0, 'renewals': 0, 'writes': [], 'erases': 0,
                   'stores': 0, 'housekeeping': [], 'sessions': [], 'versions': 0, 'config_reads': 0, 'frames': 0}

    def window(self, seq, nodes):
        left = max(0, int(self.window_end - time.monotonic()))
        state, count = (1, nodes) if left > 0 else (0, 0)
        return bytes([0x00, 0x0E, 0x00, 0x2F, seq, state, left >> 8, left & 0xFF, count >> 8, count & 0xFF,
                      0x00, 0x28, nodes >> 8, nodes & 0xFF, 0x00])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('link')
    ap.add_argument('--tap', action='append', default=[])
    ap.add_argument('--down', action='append', default=[])
    ap.add_argument('--down-file', action='append', default=[])
    ap.add_argument('--reset-quiet', type=float, default=4.5)
    ap.add_argument('--join-after', type=float, default=5)
    ap.add_argument('--stats', default=None)
    a = ap.parse_args()

    rng = random.Random(1)
    taps, node = [], 2
    for i, spec in enumerate(a.tap):
        f = spec.split(',')
        panels = int(f[2]) if len(f) > 2 else 3
        taps.append(Tap(i + 1, int(f[0], 16), int(f[1], 16), panels, len(f) > 3 and f[3] == 'new', node, rng))
        node += panels
    downs = []
    for spec in a.down:
        n, at, dur = spec.split(',')
        downs.append((int(n), float(at), float(dur)))
    down_files = [(int(spec.split(',', 1)[0]), spec.split(',', 1)[1]) for spec in a.down_file]

    m_fd, s_fd = pty.openpty()
    os.system(f'stty -F {os.ttyname(s_fd)} raw -echo')
    os.set_blocking(m_fd, False)
    if os.path.lexists(a.link):
        os.unlink(a.link)
    os.symlink(os.ttyname(s_fd), a.link)

    def send(data):
        try:
            os.write(m_fd, data)
        except OSError:
            pass

    stats = {'claims': 0, 'resets': 0, 'other': [], 'log': [], 'taps': [t.st for t in taps]}
    start = time.monotonic()
    quiet_until = 0.0
    last_stats = 0.0
    buf = bytearray()
    while True:
        now = time.monotonic()
        t_rel = now - start
        for n, at, dur in downs:
            taps[n - 1].down = at <= t_rel and (dur == 0 or t_rel < at + dur)
        for n, path in down_files:
            taps[n - 1].down = os.path.exists(path)
        for t in taps:
            t.st['id'] = f'{t.id:04X}'
            t.st['down'] = t.down
        if a.stats and now - last_stats >= 1.0:
            with open(a.stats + '.tmp', 'w') as f:
                json.dump(stats, f)
            os.replace(a.stats + '.tmp', a.stats)
            last_stats = now
        r, _, _ = select.select([m_fd], [], [], 0.05)
        if not r:
            continue
        try:
            chunk = os.read(m_fd, 4096)
        except OSError:
            continue
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
            t_rel = time.monotonic() - start
            if cmd != 0x0148 and not (cmd == 0x0B0F and len(p) > 3 and p[3] in (0x2E, 0x41)) and len(stats['log']) < 800:
                sub = f' {p[3]:02x}' if cmd == 0x0B0F and len(p) > 3 else ''
                stats['log'].append({'t': round(t_rel, 2), 'f': f'{dest:04X}/{cmd:04X}{sub}'})
            if time.monotonic() < quiet_until:
                continue
            live = [t for t in taps if not t.down]
            if cmd == 0x0014 and dest == 0x0000:  # SET_485_ID: every TAP to the factory address
                stats['claims'] += 1
                for t in live:
                    t.id = FACTORY
                send(encode(0x8000, 0x0015, b''))
                continue
            if cmd == 0x0010 and dest == 0x0000:  # RESET: each back at its kept id
                stats['resets'] += 1
                for t in live:
                    t.id = t.kept if t.kept is not None else FACTORY
                quiet_until = time.monotonic() + a.reset_quiet
                continue
            here = [t for t in live if t.id == dest]
            if cmd in (0x0038, 0x003A):  # GET_MAC: each TAP there answers
                for t in here:
                    send(encode(t.id | 0x8000, cmd + 1, t.mac + bytes([t.id >> 8, t.id & 0xFF])))
                continue
            if cmd == 0x003C and len(p) == 14:  # SET_ID_BY_MAC: the one with that MAC
                for t in here:
                    if p[4:12] == t.mac:
                        send(encode(dest | 0x8000, 0x003D, b''))
                        t.id = p[12] << 8 | p[13]
                        break
                continue
            if len(here) != 1:
                continue  # none there, or two answering over each other
            t = here[0]
            t.st['frames'] += 1
            addr = t.id
            if cmd == 0x0012 and p == bytes.fromhex('37249266'):  # STORE, key only
                t.kept = t.id
                t.st['stores'] += 1
                send(encode(addr | 0x8000, 0x0013, b''))
            elif cmd == 0x000A:
                t.st['versions'] += 1
                send(encode(addr | 0x8000, 0x000B, f'Mgate Version G8.{58 + t.n}\rJan  1 2026\r'.encode()))
            elif cmd in (0x000E, 0x0E02) and not p:
                send(encode(addr | 0x8000, cmd + 1, bytes(11) + b'\x01\x03'))
            elif cmd == 0x0148 and len(p) == 5:
                t.st['polls'] += 1
                cursor = p[2] << 8 | p[3]
                n = rng.choice([0, 0, 1, 2]) if t.panels else 0
                packets = ''.join(power_packet(t.panels[(t.handed_out + i) % len(t.panels)][1], rng, 0.8)
                                  for i in range(n))
                t.handed_out += n
                send(encode(addr | 0x8000, 0x0149, bytes.fromhex(f'41FF{cursor & 0xFF:02X}4B9A' + packets)))
                if not t.sent_table and t.panels:
                    first = t.panels[0][1]
                    table = '0000002701' + f'{first:04X}{len(t.panels):04X}' + ''.join(
                        b + f'{n:04X}' for b, n in t.panels)
                    send(encode(addr | 0x8000, 0x0B10, bytes.fromhex(table)))
                    t.sent_table = True
            elif cmd == 0x0B0F and len(p) >= 5 and p[3] == 0x2E:
                t.st['checks'] += 1
                send(encode(addr | 0x8000, 0x0B10, t.window(p[4], len(t.panels))))
            elif cmd == 0x0B0F and len(p) == 14 and p[3] == 0x2D:
                t.st['renewals'] += 1
                t.window_end = time.monotonic() + 600
                send(encode(addr | 0x8000, 0x0B10, t.window(p[4], len(t.panels))))
            elif cmd == 0x0B00 and len(p) == 1:
                t.st['sessions'].append(f'{p[0]:02x}')
                send(encode(addr | 0x8000, 0x0B01, p))
            elif cmd == 0x0B0F and len(p) == 7 and p[3] == 0x0D:
                t.st['config_reads'] += 1
                send(encode(addr | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x0E, p[4]]) + bytes(range(37))))
            elif cmd == 0x0B0F and len(p) == 7 and p[3] == 0x22:
                send(encode(addr | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x23, p[4], 0x03])))
            elif cmd == 0x0B0F and len(p) >= 5 and p[3] == 0x41:
                t.st['housekeeping'].append(p[5:7].hex())
                send(encode(addr | 0x8000, 0x0B10, bytes([0x00, 0x0D, 0x00, 0x41, p[4]])))
            elif cmd == 0x0B0F and len(p) == 7 and p[3] == 0x26:
                idx = p[5] << 8 | p[6]
                rows = ([] if t.erased else list(t.panels)) + \
                    [(m, n | (0 if t_rel >= at else 0x8000)) for m, n, at in t.written]
                page = sorted([(b, n) for b, n in rows if (n & 0x7FFF) >= idx], key=lambda r: r[1] & 0x7FFF)[:12]
                first = page[0][1] & 0x7FFF if page else idx
                out = bytes([0x00, 0x0E, 0x00, 0x27, p[4], first >> 8, first & 0xFF, 0x00, len(page)])
                for barcode, paddr in page:
                    out += bytes.fromhex(barcode) + bytes([paddr >> 8, paddr & 0xFF])
                send(encode(addr | 0x8000, 0x0B10, out))
            elif cmd == 0x0B0F and len(p) >= 16 and p[3] == 0x29 and len(p) == 6 + 10 * p[5]:
                t.st['writes'].append(p[5:].hex())
                for k in range(p[5]):
                    rec = p[6 + 10 * k:16 + 10 * k]
                    tuid = rec[8] << 8 | rec[9]
                    t.written.append((rec[:8].hex(), tuid & 0x7FFF, t_rel + a.join_after if tuid & 0x8000 else t_rel))
                send(encode(addr | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x2A, p[4], 0x00])))
            elif cmd == 0x0B0F and len(p) == 7 and p[3] == 0x2B and p[5:7] == b'\xba\xbe':
                t.st['erases'] += 1
                t.erased = True
                t.written.clear()
                send(encode(addr | 0x8000, 0x0B10, bytes([0x00, 0x0E, 0x00, 0x2C, p[4], 0x00])))
            else:
                stats['other'].append(f'{dest:04X} {cmd:04X} {p.hex()}')


if __name__ == '__main__':
    main()
