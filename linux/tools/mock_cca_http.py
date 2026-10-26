#!/usr/bin/env python3
"""A stand-in for a CCA's local HTTP API (firmware before 4.0.4), with made-up data.

    mock_cca_http.py PORT

POST a JSON list of barcodes to / and summary_config returns them as panels
under two strings on two inverters. Requests need the CCA's Basic auth.
"""
import base64, json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
barcodes = []
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        global barcodes
        barcodes = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.send(200, {})
    def do_GET(self):
        if self.headers.get('Authorization') != 'Basic ' + base64.b64encode(b'Tigo:$olar').decode():
            return self.send(401, {})
        if self.path == '/cgi-bin/mobile_api?cmd=DEVICE_INFO':
            return self.send(200, {'serial': 'HTTPCCA0001', 'sw_version': '3.9.0'})
        if self.path == '/cgi-bin/summary_config':
            objs = [{'id': '10', 'type': 4, 'label': 'Inverter 1'}, {'id': '11', 'type': 4, 'label': 'Inverter 2'},
                    {'id': '20', 'type': 3, 'label': 'East', 'parent': '10'}, {'id': '21', 'type': 3, 'label': 'West', 'parent': '11'}]
            for i, b in enumerate(barcodes):
                objs.append({'id': str(100 + i), 'type': 2, 'label': f'P{i}', 'serial': b, 'channel': f'ch{i}', 'parent': '20' if i < 2 else '21'})
            return self.send(200, objs)
        self.send(404, {})
HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
