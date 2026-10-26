#!/usr/bin/env python3
"""A network bridge for tests: serves a bus capture over TCP, as stream_server
does on an ESP32. Each connection gets the whole file and is then closed, so a
second connection shows the daemon reconnecting. Counts every byte a client
sends, which must stay 0: the daemon only ever reads.

usage: mock_bridge.py CAPTURE PORT CONNECTIONS STATSFILE [BIND_ADDRESS]
"""
import json
import socket
import sys

capture, port, connections, stats = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = open(capture, 'rb').read()
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((sys.argv[5] if len(sys.argv) > 5 else '127.0.0.1', port))
srv.listen(1)
received = 0
for _ in range(connections):
    conn, _ = srv.accept()
    conn.sendall(data)
    conn.settimeout(0.5)
    try:
        while chunk := conn.recv(4096):
            received += len(chunk)
    except socket.timeout:
        pass
    conn.close()
json.dump({'served': connections, 'received': received}, open(stats, 'w'))
