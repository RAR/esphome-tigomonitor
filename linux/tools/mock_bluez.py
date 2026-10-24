#!/usr/bin/env python3
"""A stand-in for BlueZ on a private D-Bus bus, with one made-up Tigo CCA.

    dbus-daemon --session --fork --print-address   # then, with that address:
    DBUS_SESSION_BUS_ADDRESS=... python3 mock_bluez.py

The CCA advertises once discovery starts, accepts a connection, and serves
the Tigo GATT service: requests are [reqNum] "cmd=X[ params]" '\\0' written to
one characteristic, replies come back as notifications on another, each with
a 3-byte header and the body split into 20-byte pieces ending with '\\0'.
Protected commands need sid = SHA-512 hex of (uts + 159260), as on a real CCA.
Everything here is invented; nothing belongs to a real installation.
"""
import hashlib
import json
import sys
from urllib.parse import unquote

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

UUID = 'd3dadcba-e4fa-4016-ba33-8bcc671999a7'
ADAPTER = '/org/bluez/hci0'
CCA_MAC = '04:C0:5B:12:34:56'
OTHER_MAC = 'AA:BB:CC:00:11:22'
UTS = 1700000123
SID = hashlib.sha512(str(UTS + 159260).encode()).hexdigest()
PROPS = 'org.freedesktop.DBus.Properties'

log = []  # requests the CCA received, for the test to check


def replies(cmd, params):
    if cmd == 'DEVICE_INFO':
        return {'uts': UTS, 'serial': '00000000TEST', 'sw_version': '4.0.5-test',
                'status': [{'name': 'Cloud Connection', 'status': 0}, {'name': 'Discovery: 6/6', 'status': 0}]}
    if params.get('sid') != SID:
        return {'msg': 'Unauthorized request'}
    if cmd == 'NETWORK_INFO':
        return {'eth': {'state': 'up', 'ip': '192.0.2.10'}, 'wlan': {'state': 'down'}}
    if cmd in ('START_DISCOVERY', 'DEVICE_DATA_EXPORT', 'NETWORK_WIFI_CLEAR'):
        return {'code': 0}
    if cmd == 'DISCOVERY_STATUS':
        return {'running': False, 'found': 6, 'expected': 6}
    if cmd == 'NETWORK_WIFI_SCAN':
        return [{'ssid': 'ExampleNet', 'rssi': -55}]
    if cmd == 'NETWORK_WIFI_CONNECT':
        return {'code': 0, 'nid': params.get('nid'), 'pwd_len': len(params.get('pwd', ''))}
    return {'msg': 'Unknown command'}


class Obj(dbus.service.Object):
    def __init__(self, bus, path, iface, props):
        super().__init__(bus, path)
        self.path, self.iface, self.props = path, iface, props

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        return self.props[name]

    @dbus.service.method(PROPS, in_signature='ssv')
    def Set(self, iface, name, value):
        self.props[name] = value

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Root(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, '/')
        self.objs = {}

    def add(self, obj):
        self.objs[obj.path] = obj

    @dbus.service.method('org.freedesktop.DBus.ObjectManager', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {dbus.ObjectPath(p): {o.iface: o.props} for p, o in self.objs.items()}


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName('org.bluez', bus)  # noqa: F841 (held for the bus name)
    root = Root(bus)

    class Adapter(Obj):
        @dbus.service.method('org.bluez.Adapter1', in_signature='a{sv}')
        def SetDiscoveryFilter(self, f):
            log.append('filter:' + str(f.get('Transport')))

        @dbus.service.method('org.bluez.Adapter1')
        def StartDiscovery(self):
            for mac, nm, rssi in ((CCA_MAC, 'Tigo CCA', -61), (OTHER_MAC, 'Speaker', -40)):
                path = ADAPTER + '/dev_' + mac.replace(':', '_')
                if path in root.objs:
                    root.objs[path].props['RSSI'] = dbus.Int16(rssi)
                    continue
                root.add(Device(bus, path, 'org.bluez.Device1', {
                    'Address': mac, 'Name': nm, 'RSSI': dbus.Int16(rssi),
                    'Connected': False, 'ServicesResolved': False}))

        @dbus.service.method('org.bluez.Adapter1')
        def StopDiscovery(self):
            pass

    class Device(Obj):
        @dbus.service.method('org.bluez.Device1')
        def Connect(self):
            if self.props['Address'] != CCA_MAC:
                raise dbus.exceptions.DBusException('org.bluez.Error.Failed', 'not a CCA')
            log.append('connect')
            svc = self.path + '/service0010'
            if svc in root.objs:  # BlueZ keeps the GATT tree between connections
                self.props['Connected'] = True
                GLib.timeout_add(300, lambda: self.props.__setitem__('ServicesResolved', True))
                return
            root.add(Obj(bus, svc, 'org.bluez.GattService1', {'UUID': UUID, 'Device': dbus.ObjectPath(self.path)}))
            notify = Char(bus, svc + '/char0011', 'org.bluez.GattCharacteristic1',
                          {'UUID': '0000aaaa-0000-1000-8000-00805f9b34fb', 'Service': dbus.ObjectPath(svc),
                           'Flags': dbus.Array(['notify'], signature='s')})
            root.add(notify)
            root.add(Char(bus, svc + '/char0013', 'org.bluez.GattCharacteristic1',
                          {'UUID': '0000bbbb-0000-1000-8000-00805f9b34fb', 'Service': dbus.ObjectPath(svc),
                           'Flags': dbus.Array(['write'], signature='s')}, notify))
            self.props['Connected'] = True
            GLib.timeout_add(300, lambda: self.props.__setitem__('ServicesResolved', True))

        @dbus.service.method('org.bluez.Device1')
        def Disconnect(self):
            log.append('disconnect')
            self.props['Connected'] = False
            self.props['ServicesResolved'] = False

    class Char(Obj):
        def __init__(self, bus, path, iface, props, notify=None):
            super().__init__(bus, path, iface, props)
            self.notify = notify

        @dbus.service.method('org.bluez.GattCharacteristic1')
        def StartNotify(self):
            log.append('notify-on')

        @dbus.service.method('org.bluez.GattCharacteristic1')
        def StopNotify(self):
            log.append('notify-off')

        @dbus.service.method('org.bluez.GattCharacteristic1', in_signature='aya{sv}')
        def WriteValue(self, value, opts):
            raw = bytes(value)
            assert opts.get('type') == 'request', opts
            assert raw[-1] == 0, raw
            req, text = raw[0], raw[1:-1].decode()
            fields = dict(f.split('=', 1) for f in text.split(' '))
            cmd = fields.pop('cmd')
            params = {k: unquote(v) for k, v in fields.items()}
            log.append(cmd)
            body = json.dumps(replies(cmd, params)).encode() + b'\0'

            def send(pieces=[body[i:i + 20] for i in range(0, len(body), 20)]):
                if pieces:
                    self.notify.PropertiesChanged('org.bluez.GattCharacteristic1', {
                        'Value': dbus.Array(bytes([req, 0, 0]) + pieces.pop(0), signature='y')}, [])
                    return True
                return False
            GLib.timeout_add(20, send)

    root.add(Adapter(bus, ADAPTER, 'org.bluez.Adapter1', {'Address': '00:00:5E:00:53:01', 'Powered': False}))

    class Control(dbus.service.Object):
        @dbus.service.method('test.Mock', out_signature='as')
        def Log(self):
            return log

        @dbus.service.method('test.Mock', out_signature='b')
        def Powered(self):
            return bool(root.objs[ADAPTER].props['Powered'])
    Control(bus, '/test')
    print('ready', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    sys.exit(main())
