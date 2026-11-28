#!/usr/bin/env python3
"""The hub sensors' name keywords (components/tigo_monitor/sensor.py).

A hub sensor has no address, so its kind comes from words in its name. This
checks which kind each name gets, and that every kind has a schema, a
registration, and the C++ method that registration calls.

Needs ESPHome importable: CI's ESP32 job runs it after installing ESPHome;
here, run it with ESPHome's own Python.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'components'))
import tigo_monitor.sensor as s  # noqa: E402

fails = 0


def expect(what, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))


NAMES = {
    # Today's energy, the web page's "Today" figure (#92).
    'Energy Today': 'energy_today',
    "Today's Energy": 'energy_today',
    'Daily Energy': 'energy_today',
    'Solar Energy Today': 'energy_today',
    # The rest as they were.
    'Total Energy Out': 'energy_out',
    'Total Energy In': 'energy_in',
    'Total System Energy': 'energy_in',
    'Total Output Power': 'power_out',
    'Total System Power': 'power_in',
    'Active Device Count': 'count',
    'Missed Frame Count': 'frame',
    'Invalid Checksum Count': 'checksum',
    'Stale Panel Count': 'stale_count',
    'Free Internal RAM': 'internal_ram',
    'Min Free Internal RAM': 'internal_ram_min',
    'Free PSRAM': 'psram',
    'Free Stack': 'stack',
}
for name, want in NAMES.items():
    expect(name, s._classify_aggregate_sensor(name), want)

header = open(os.path.join(ROOT, 'components', 'tigo_monitor', 'tigo_monitor.h')).read()
for kind in [k for k, _ in s._AGGREGATE_RULES] + ['internal_ram_min']:
    expect(f'{kind}: a schema', kind in s._AGGREGATE_SCHEMA_BY_CATEGORY, True)
    method = s._REGISTER_BY_CATEGORY.get(kind)
    expect(f'{kind}: registered by a method the hub has', bool(method and re.search(r'\bvoid ' + method + r'\(', header)), True)

if fails:
    print(f'{fails} failed')
    sys.exit(1)
print('all passed')
