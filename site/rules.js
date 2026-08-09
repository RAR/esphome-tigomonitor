export function assembleConfig(board, form) {
  if (form.cca === 'ble' && !board.supports.ble) {
    throw new Error(`BLE is not supported on ${board.id}`);
  }
  // A board without PSRAM cannot run tigo_server: the web layer builds whole
  // HTML pages and JSON responses in memory from the httpd task. Everything the
  // wizard offers on top of the monitor — CCA import, cloud layout import, the
  // BLE bridge — is a tigo_server feature, so it all drops with it. The form
  // hides those controls, but enforce it here too: rules.js is the contract, and
  // a stale form value must not silently produce a config that OOMs on device.
  const webServer = board.supportsWebServer !== false;
  if (!webServer && form.cca === 'ble') {
    throw new Error(`${board.id} has no web server, so it cannot import from a CCA over BLE`);
  }
  const cca = webServer ? form.cca : 'none';
  const cloudImport = webServer && Boolean(form.cloudImport);
  const useSecrets = Boolean(form.useSecrets);
  const partitions =
    cca === 'ble' && board.partitions?.ble
      ? board.partitions.ble
      : board.partitions?.default ?? null;

  // cca: 'ble' needs the full BLE stack + a ble_client pointing at the CCA's MAC.
  // tigo_server's own validation rejects cca_source: ble without a ble_client_id.
  const ble =
    cca === 'ble'
      ? {
          mac: form.ccaMac || '04:C0:5B:00:00:00',
          clientId: 'tigo_cca_ble',
          usePsram: Boolean(board.psram),
        }
      : null;

  const secrets = useSecrets
    ? {
        wifi_ssid: form.wifi.ssid,
        wifi_password: form.wifi.password,
        api_encryption_key: form.apiKey,
        ota_password: form.otaPassword,
      }
    : null;

  return {
    // "Server" is a misnomer on the monitor-only tier — match the board file.
    name: form.name || (webServer ? 'tigo-server' : 'tigo-monitor'),
    friendlyName: webServer ? 'Tigo Server' : 'Tigo Monitor',
    minVersion: '2026.5.0',
    esp32: {
      board: board.board,
      variant: board.variant ?? null,
      flash_size: board.flash_size ?? null,
      frameworkAdvanced: board.frameworkAdvanced,
      frameworkComponents: board.frameworkComponents,
      hostedComponent: board.hostedComponent,
      sdkconfig: board.sdkconfig,
      // Merged in by toYaml only when the config actually selects BLE.
      sdkconfigBle: board.sdkconfigBle ?? null,
    },
    partitions,
    psram: board.psram,
    hosted: board.hosted,
    wifi: {
      ssid: form.wifi.ssid,
      password: form.wifi.password,
      staticIp: form.wifi.staticIp || null,
      useSecrets,
    },
    api: { useSecrets, key: form.apiKey },
    ota: { useSecrets, password: form.otaPassword },
    uart: {
      tx_pin: form.uart.tx_pin || board.uartDefault.tx_pin,
      rx_pin: form.uart.rx_pin || board.uartDefault.rx_pin,
      rx_buffer_size: board.uartDefault.rx_buffer_size,
    },
    tigoMonitor: {
      numberOfDevices: form.numberOfDevices || board.numberOfDevices,
      updateInterval: form.updateInterval || '30s',
      ccaIp: cca === 'http' ? form.ccaIp || '192.168.1.100' : null,
    },
    // null when the board cannot host it — yaml.js omits the whole block.
    tigoServer: webServer
      ? {
          ccaSource: cca === 'none' ? null : cca,
          cloudImport,
          bleClientId: ble ? ble.clientId : null,
        }
      : null,
    ble,
    displayOverlay: form.display && board.supports.display ? board.displayOverlay : null,
    keepSerialConsole: Boolean(board.keepSerialConsole),
    gpioSwitches: board.gpioSwitches ?? null,
    secrets,
  };
}
