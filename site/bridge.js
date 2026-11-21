// site/bridge.js
// The Config Builder's second kind of build: a network bridge. The board
// decodes nothing; stream_server passes every byte from the RS485 bus to
// whoever connects on port 6638, and TigoMonitor on a Pi, a Linux machine or
// Home Assistant reads the bus from there.
//
// Each board here mirrors an example file in boards/ (test/bridge.test.mjs
// checks every block that touches the bus against it). Only boards whose
// transceiver we know how to hold silent are listed: a bridge's UART has no
// tx_pin, and a switch holds the driver's input (or enable) at the level that
// keeps it off. ESPHome's safe mode never runs the switches, so each board
// marks that pin safeMode and a safe_mode: hook drives it to the same level.
// Add a board only with an example file to check it against.
import { wifiLines } from './yaml.js';

const I = (n) => '  '.repeat(n);
const val = (useSecrets, secretName, raw) => (useSecrets ? `!secret ${secretName}` : raw);

export const BRIDGE_BOARDS = [
  {
    id: 'atoms3-bridge',
    label: 'M5Stack AtomS3 or AtomS3R + RS485 Base',
    example: 'boards/example-atoms3-bridge.yaml',
    board: 'm5stack-atoms3', variant: 'esp32s3', flash_size: '8MB',
    minimumChipRevision: null,
    rxPin: 'GPIO5', rxComment: "the base's RS485_RX; no tx_pin, see the top",
    // The base drives the bus while its TX input (G6) is LOW.
    holdPins: [
      { id: 'rs485_tx_idle', pin: 'GPIO6', comment: "the base's RS485_TX, held HIGH: its driver stays off", safeMode: true },
    ],
    notes: [
      'No PSRAM needed: a plain AtomS3 is enough.',
      'For about a second at power-on, before the firmware holds G6, the base can drive the bus. Power the bridge up before you connect A and B.',
      'Tested on a 40-panel roof bus feeding a Raspberry Pi 5.',
    ],
  },
  {
    id: 't-can485-bridge',
    label: 'LilyGO T-CAN485',
    example: 'boards/example-t-can485-bridge.yaml',
    board: 'esp32dev', variant: null, flash_size: '4MB',
    // See boards.js: 3.0 (ECO3), not 3.1.
    minimumChipRevision: '3.0',
    rxPin: 'GPIO21', rxComment: 'no tx_pin: see the read-only note at the top',
    holdPins: [
      { id: 'rs485_power_5v', pin: 'GPIO16', comment: '5V boost for the transceiver' },
      { id: 'rs485_auto_direction', pin: 'GPIO17', comment: '/RE high => AutoDirection controls the receiver' },
      { id: 'rs485_chip_enable', pin: 'GPIO19', comment: 'SHDN high => normal operation' },
      // The MAX13487E turns its driver on when DI is LOW.
      { id: 'rs485_driver_off', pin: 'GPIO22', comment: 'DI high => the driver stays off (no tx_pin below)', safeMode: true },
    ],
    notes: [
      'No PSRAM needed, so this board can be a bridge even though it is too small for the dashboard.',
      'GPIO22, the transceiver’s transmit input, is held high so its driver stays off.',
      'Built, but not yet run on a bus.',
    ],
  },
  {
    id: 'waveshare-rs485-can-bridge',
    label: 'Waveshare ESP32-S3-RS485-CAN',
    example: 'boards/example-waveshare-rs485-can-bridge.yaml',
    board: 'esp32-s3-devkitc-1', variant: 'esp32s3', flash_size: '16MB',
    minimumChipRevision: null,
    rxPin: 'GPIO18', rxComment: 'no tx_pin: see the read-only note at the top',
    // DE and /RE are one net: LOW is receiver on, driver off.
    holdPins: [
      { id: 'rs485_enable', pin: 'GPIO21', restore: 'ALWAYS_OFF',
        comment: 'DE + /RE, low => receiver on and driver off', safeMode: true },
    ],
    notes: [
      '7-36V input and a DIN-rail case, so it can share the Tigo cabinet\u2019s supply.',
      'GPIO21 is held low, which turns the receiver on and the driver off, in safe mode too.',
      'Built, but not yet run on a bus. The pins are the monitor config\u2019s, which a user ran (issue #22).',
    ],
  },
  {
    id: 't-connect-pro-lite-bridge',
    label: 'LilyGO T-Connect Pro Lite (wired Ethernet)',
    example: 'boards/example-t-connect-pro-lite-bridge.yaml',
    board: 'esp32-s3-devkitc-1', variant: 'esp32s3', flash_size: '8MB',
    minimumChipRevision: null,
    rxPin: 'GPIO18', rxComment: 'no tx_pin: see the read-only note at the top',
    // The transceiver turns around on its TX input; HIGH is the UART's idle
    // level, what the monitor config leaves on the pin between bytes.
    holdPins: [
      { id: 'rs485_tx_idle', pin: 'GPIO17', comment: "the transceiver's TX input, held HIGH: idle, driver off", safeMode: true },
    ],
    // On-board W5500. The bridge then has no wifi: block at all.
    ethernet: {
      type: 'W5500', clk_pin: 'GPIO12', mosi_pin: 'GPIO11', miso_pin: 'GPIO13',
      cs_pin: 'GPIO10', interrupt_pin: 'GPIO9', reset_pin: 'GPIO48', clock_speed: '8MHz',
    },
    notes: [
      'Wired Ethernet, so the stream never waits on Wi-Fi. For a fixed address, reserve one in your router or fill in the commented manual_ip: block.',
      'GPIO17, the transceiver\u2019s TX input, is held high, the line\u2019s idle level.',
      'Built, but not yet run on a bus. The pins are the monitor config\u2019s, from a contributor\u2019s working install (discussion #30).',
    ],
  },
];

export function getBridgeBoard(id) {
  return BRIDGE_BOARDS.find((b) => b.id === id);
}

export function assembleBridge(board, form) {
  const useSecrets = Boolean(form.useSecrets);
  const wired = Boolean(board.ethernet);
  return {
    board,
    name: form.name || 'tigo-bridge',
    // null on a wired board: no wifi: block, and no Wi-Fi in secrets.yaml.
    wifi: wired ? null : {
      ssid: form.wifi.ssid, password: form.wifi.password,
      staticIp: form.wifi.staticIp || null, useSecrets,
    },
    api: { useSecrets, key: form.apiKey },
    ota: { useSecrets, password: form.otaPassword },
    secrets: useSecrets
      ? {
          ...(wired ? {} : { wifi_ssid: form.wifi.ssid, wifi_password: form.wifi.password }),
          api_encryption_key: form.apiKey,
          ota_password: form.otaPassword,
        }
      : null,
  };
}

export function toBridgeYaml(cfg) {
  const b = cfg.board;
  const L = [];
  L.push('# Generated by the TigoMonitor config builder: a network bridge');
  L.push(`# Board: ${b.label}`);
  L.push('# https://github.com/RAR/esphome-tigomonitor');
  L.push('#');
  L.push('# This board decodes nothing. It passes the raw RS485 bus over TCP to');
  L.push('# TigoMonitor on a Raspberry Pi, a Linux machine or Home Assistant, which');
  L.push('# decodes it and serves the dashboard. In TigoMonitor, under Settings -> Bus,');
  L.push(`# choose "Network bridge (TCP)" and enter  ${cfg.name}.local:6638  (or this`);
  L.push("# board's IP: mDNS doesn't reach every network, and the IP always works).");
  L.push('#');
  L.push('# READ-ONLY, IN HARDWARE. stream_server writes whatever a client sends to the');
  L.push('# UART, so the UART below has no tx_pin, and the switch block holds the');
  L.push("# transceiver's driver off. Keep both as they are.");
  L.push('');

  L.push('esphome:');
  L.push(`${I(1)}name: ${cfg.name}`);
  L.push(`${I(1)}friendly_name: "Tigo Bridge"`);
  L.push('');

  L.push('esp32:');
  L.push(`${I(1)}board: ${b.board}`);
  if (b.variant) L.push(`${I(1)}variant: ${b.variant}`);
  L.push(`${I(1)}flash_size: ${b.flash_size}`);
  L.push(`${I(1)}framework:`);
  L.push(`${I(2)}type: esp-idf`);
  L.push(`${I(2)}version: recommended`);
  if (b.minimumChipRevision) {
    L.push(`${I(2)}advanced:`);
    L.push(`${I(3)}minimum_chip_revision: '${b.minimumChipRevision}'`);
  }
  L.push(`${I(2)}sdkconfig_options:`);
  L.push(`${I(3)}# The UART ISR from IRAM, so bytes are not dropped while flash is busy.`);
  L.push(`${I(3)}CONFIG_UART_ISR_IN_IRAM: "y"`);
  L.push(`${I(3)}CONFIG_LOG_DEFAULT_LEVEL_INFO: "y"`);
  L.push('');

  L.push('external_components:');
  L.push(`${I(1)}- source: github://oxan/esphome-stream-server`);
  L.push(`${I(2)}components: [stream_server]`);
  L.push('');

  if (b.ethernet) {
    const e = b.ethernet;
    L.push('ethernet:');
    for (const k of ['type', 'clk_pin', 'mosi_pin', 'miso_pin', 'cs_pin', 'interrupt_pin', 'reset_pin', 'clock_speed']) {
      L.push(`${I(1)}${k}: ${e[k]}`);
    }
    L.push(`${I(1)}# For a fixed address, uncomment and fill in:`);
    L.push(`${I(1)}# manual_ip:`);
    L.push(`${I(1)}#   static_ip: 10.0.0.50`);
    L.push(`${I(1)}#   gateway: 10.0.0.1`);
    L.push(`${I(1)}#   subnet: 255.255.255.0`);
    L.push('');
  } else {
    L.push(...wifiLines(cfg.wifi));
    L.push(`${I(1)}# Latency costs nothing here, but dropped Wi-Fi beacons stall the stream.`);
    L.push(`${I(1)}power_save_mode: none`);
    L.push(`${I(1)}ap:`);
    L.push(`${I(2)}ssid: "Tigo-Bridge Fallback Hotspot"`);
    L.push(`${I(2)}password: "12345678"`);
    L.push('');
    L.push('captive_portal:');
    L.push('');
  }

  L.push('logger:');
  L.push(`${I(1)}level: INFO`);
  L.push('');

  L.push('# For OTA updates and the sensors below; nothing on the bus goes through it.');
  L.push('api:');
  L.push(`${I(1)}encryption:`);
  L.push(`${I(2)}key: ${val(cfg.api.useSecrets, 'api_encryption_key', cfg.api.key)}`);
  L.push('');

  L.push('ota:');
  L.push(`${I(1)}- platform: esphome`);
  L.push(`${I(2)}password: ${val(cfg.ota.useSecrets, 'ota_password', cfg.ota.password)}`);
  L.push('');

  L.push('switch:');
  for (const p of b.holdPins) {
    L.push(`${I(1)}- platform: gpio`);
    L.push(`${I(2)}id: ${p.id}`);
    L.push(`${I(2)}internal: true`);
    L.push(`${I(2)}pin: ${p.pin}  # ${p.comment}`);
    L.push(`${I(2)}restore_mode: ${p.restore ?? 'ALWAYS_ON'}`);
  }
  L.push(`${I(1)}- platform: restart`);
  L.push(`${I(2)}name: "Tigo Bridge Restart"`);
  L.push('');

  // Safe mode (after 10 quick reboots) sets up only the network, OTA and the
  // logger, so the switches never run and the driver-off pin floats. Drive it
  // to the level its switch holds.
  const holds = b.holdPins.filter((p) => p.safeMode);
  if (holds.length) {
    L.push('# Safe mode never runs the switches above; this holds the driver off there.');
    L.push('safe_mode:');
    L.push(`${I(1)}on_safe_mode:`);
    L.push(`${I(2)}- lambda: |-`);
    for (const p of holds) {
      const n = p.pin.replace('GPIO', 'GPIO_NUM_');
      const high = (p.restore ?? 'ALWAYS_ON') === 'ALWAYS_ON';
      L.push(`${I(4)}gpio_reset_pin(${n});`);
      L.push(`${I(4)}gpio_set_direction(${n}, GPIO_MODE_OUTPUT);`);
      L.push(`${I(4)}gpio_set_level(${n}, ${high ? 1 : 0});`);
      L.push(`${I(4)}ESP_LOGW("tigo_bridge", "Safe mode: ${p.pin} held ${high ? 'high' : 'low'}, the RS485 driver is off");`);
    }
    L.push('');
  }

  L.push('uart:');
  L.push(`${I(1)}id: tigo_uart`);
  L.push(`${I(1)}rx_pin: ${b.rxPin}  # ${b.rxComment}`);
  L.push(`${I(1)}baud_rate: 38400`);
  L.push(`${I(1)}rx_buffer_size: 2048`);
  L.push('');

  L.push('stream_server:');
  L.push(`${I(1)}uart_id: tigo_uart`);
  L.push(`${I(1)}port: 6638`);
  L.push(`${I(1)}# Bytes sit here while the network is slow; 38400 baud is ~3.8 kB/s, so`);
  L.push(`${I(1)}# this covers about a second.`);
  L.push(`${I(1)}buffer_size: 4096`);
  L.push('');

  L.push('binary_sensor:');
  L.push(`${I(1)}- platform: status`);
  L.push(`${I(2)}name: "Tigo Bridge Status"`);
  L.push(`${I(1)}- platform: stream_server`);
  L.push(`${I(2)}connected:`);
  L.push(`${I(3)}name: "Tigo Bridge Client Connected"`);
  L.push('');

  L.push('sensor:');
  if (!b.ethernet) {
    L.push(`${I(1)}- platform: wifi_signal`);
    L.push(`${I(2)}name: "Tigo Bridge WiFi Signal"`);
    L.push(`${I(2)}update_interval: 60s`);
  }
  L.push(`${I(1)}- platform: stream_server`);
  L.push(`${I(2)}connection_count:`);
  L.push(`${I(3)}name: "Tigo Bridge Clients"`);
  L.push('');
  return L.join('\n');
}
