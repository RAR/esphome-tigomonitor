export function assembleConfig(board, form) {
  if (form.cca === 'ble' && !board.supports.ble) {
    throw new Error(`BLE is not supported on ${board.id}`);
  }
  const useSecrets = Boolean(form.useSecrets);
  const partitions =
    form.cca === 'ble' && board.partitions?.ble
      ? board.partitions.ble
      : board.partitions?.default ?? null;

  // cca: 'ble' needs the full BLE stack + a ble_client pointing at the CCA's MAC.
  // tigo_server's own validation rejects cca_source: ble without a ble_client_id.
  const ble =
    form.cca === 'ble'
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
    name: form.name || 'tigo-server',
    friendlyName: 'Tigo Server',
    minVersion: '2026.5.0',
    esp32: {
      board: board.board,
      variant: board.variant ?? null,
      flash_size: board.flash_size ?? null,
      frameworkAdvanced: board.frameworkAdvanced,
      frameworkComponents: board.frameworkComponents,
      hostedComponent: board.hostedComponent,
      sdkconfig: board.sdkconfig,
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
      ccaIp: form.cca === 'http' ? form.ccaIp || '192.168.1.100' : null,
    },
    tigoServer: {
      ccaSource: form.cca === 'none' ? null : form.cca,
      cloudImport: Boolean(form.cloudImport),
      bleClientId: ble ? ble.clientId : null,
    },
    ble,
    displayOverlay: form.display && board.supports.display ? board.displayOverlay : null,
    secrets,
  };
}
