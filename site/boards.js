export const BOARDS = [
  {
    id: 'esp32s3-atoms3r',
    label: 'M5Stack AtomS3R (ESP32-S3, 8MB PSRAM)',
    chip: 'esp32s3', board: 'm5stack-atoms3', variant: 'esp32s3',
    flash_size: '8MB',
    partitions: { default: 'partitions/tigo-8mb.csv', ble: 'partitions/tigo-8mb-ble.csv' },
    psram: { mode: 'octal', speed: '80MHz' },
    // execute_from_psram is the fix for the tsdb flash-write crash: it compiles
    // out ESP-IDF's cross-core cache-disable, which was racing WiFi/BLE-coex
    // ISRs on every history commit (MTTF 13.5-42 h). Costs ~1.7 MiB of PSRAM.
    frameworkAdvanced: { enable_idf_experimental_features: false, execute_from_psram: true },
    // Fork-pinned for the sidecar header: upstream rewrites the db header in
    // place at offset 0 every commit, which LittleFS turns into a rewrite of
    // every block to EOF (~20.4 ms/KB). 21.4 s -> 633 ms median on the rig.
    // Mirrors boards/esp32s3-atoms3r.yaml; SHA, not a branch, so it cannot move
    // under a build.
    frameworkComponents: ['joltwallet/littlefs^1.16'],
    hostedComponent: {
      source: 'https://github.com/RAR/esp_tsdb.git',
      ref: '3fb785ffe0e280e7645a598f1cf82f6babe72143',
    },
    sdkconfig: {
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '2048',
      CONFIG_UART_TX_BUFFER_SIZE: '512',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '32',
      CONFIG_SPIRAM_MODE_OCT: 'y',
      CONFIG_SPIRAM_SPEED_80M: 'y',
      CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: 'y',
      // TLS buffers (SSL in/out content + handshake/X.509 state) to PSRAM. The cloud
      // path re-parses the pinned CA per request, which otherwise craters the internal
      // heap floor during each handshake. Requires PSRAM — do not copy to a board
      // without it.
      CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC: 'y',
      // VFS directory ops (stat/unlink/rename/opendir), off by default in IDF.
      // esp_tsdb reads file sizes via stat(), so without this the History page's
      // per-database size column reads 0 and esp_tsdb's unlink() recovery paths
      // silently no-op.
      CONFIG_VFS_SUPPORT_DIR: 'y',
      // More lwIP sockets for the web server + API + cloud running together
      // (avoids "Failed to create socket" under load, #20). Safe here: the
      // buffers land in PSRAM via SPIRAM_TRY_ALLOCATE_WIFI_LWIP above.
      CONFIG_LWIP_MAX_SOCKETS: '16',
      CONFIG_LWIP_MAX_ACTIVE_TCP: '16',
      CONFIG_LWIP_MAX_LISTENING_TCP: '16',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO1', rx_pin: 'GPIO2', rx_buffer_size: 2048 },
    numberOfDevices: 30,
    supports: { ble: true, display: true },
    displayOverlay: `# --- AtomS3R-Display overlay (from boards/atoms3r-display.yaml) ---
# I2C bus for LP5562 (RGB LED + LCD Backlight control)
i2c:
  sda: GPIO38  # SYS_SDA
  scl: GPIO39  # SYS_SCL
  scan: true

# LP5562 LED driver (controls RGB indicator LED + LCD backlight)
lp5562:
  id: rgb_driver
  address: 0x30

# SPI bus for the LCD
spi:
  clk_pin: GPIO15
  mosi_pin: GPIO21

# Outputs for LP5562 channels
output:
  - platform: lp5562
    id: lp5562_red
    lp5562_id: rgb_driver
    channel: 0  # Red channel
  - platform: lp5562
    id: lp5562_green
    lp5562_id: rgb_driver
    channel: 1  # Green channel
  - platform: lp5562
    id: lp5562_blue
    lp5562_id: rgb_driver
    channel: 2  # Blue channel
  - platform: lp5562
    id: backlight_output
    lp5562_id: rgb_driver
    channel: 3  # White channel (LCD backlight)

# Light entities
light:
  # RGB indicator LED
  - platform: rgb
    name: "RGB LED"
    id: status_led
    red: lp5562_red
    green: lp5562_green
    blue: lp5562_blue
    restore_mode: RESTORE_DEFAULT_OFF
    effects:
      - pulse:
          name: "Pulse"
          transition_length: 1s
          update_interval: 1s
      - strobe:
          name: "Strobe"

  # LCD Backlight (LP5562 White Channel)
  - platform: monochromatic
    name: "LCD Backlight"
    id: lcd_backlight
    output: backlight_output
    restore_mode: RESTORE_DEFAULT_ON
    default_transition_length: 0.5s

# Display configuration
display:
  - platform: st7789v
    id: atoms3r_lcd
    model: CUSTOM
    height: 128
    width: 128
    offset_height: 2
    offset_width: 1
    cs_pin: GPIO14
    dc_pin: GPIO42
    reset_pin: GPIO48
    rotation: 0
    eightbitcolor: true
    data_rate: 40MHz
    update_interval: 5s  # Increased from 2s to reduce CPU load and minimize missed UART packets
    setup_priority: -100  # Initialize display after everything else to avoid WDT timeout
    lambda: |-
      // Use cached values from tigo_monitor (updated during sensor publish)
      // This avoids iterating through 30+ devices on every display update
      int device_count = id(tigo_hub)->get_device_count();
      int online_count = id(tigo_hub)->get_online_device_count();
      float total_power = id(tigo_hub)->get_total_power();

      // Background
      it.fill(COLOR_BLACK);

      // Header - Title
      it.print(64, 5, id(font_title), COLOR_WHITE, TextAlign::TOP_CENTER, "TIGO");
      it.print(64, 20, id(font_small), Color(150, 150, 150), TextAlign::TOP_CENTER, "Solar Monitor");

      // Divider line
      it.line(10, 35, 118, 35, Color(100, 100, 100));

      // Main stats
      if (device_count > 0) {
        // Total Power - Large and prominent
        it.printf(64, 45, id(font_large), COLOR_ORANGE, TextAlign::TOP_CENTER, "%.0fW", total_power);

        // Device status
        it.printf(64, 75, id(font_medium), COLOR_WHITE, TextAlign::TOP_CENTER, "%d/%d", online_count, device_count);
        it.print(64, 92, id(font_tiny), Color(150, 150, 150), TextAlign::TOP_CENTER, "devices online");

        // Status indicator
        if (online_count == device_count) {
          it.filled_circle(10, 110, 4, COLOR_GREEN);
          it.print(20, 107, id(font_small), COLOR_GREEN, TextAlign::CENTER_LEFT, "All OK");
        } else if (online_count > 0) {
          it.filled_circle(10, 110, 4, COLOR_ORANGE);
          it.printf(20, 107, id(font_small), COLOR_ORANGE, TextAlign::CENTER_LEFT, "%d Offline", device_count - online_count);
        } else {
          it.filled_circle(10, 110, 4, COLOR_RED);
          it.print(20, 107, id(font_small), COLOR_RED, TextAlign::CENTER_LEFT, "No Data");
        }
      } else {
        // No devices discovered yet
        it.print(64, 55, id(font_medium), Color(200, 200, 0), TextAlign::TOP_CENTER, "WAITING");
        it.print(64, 80, id(font_small), Color(150, 150, 150), TextAlign::TOP_CENTER, "for devices...");
        it.filled_circle(10, 110, 4, Color(200, 200, 0));
        it.print(20, 107, id(font_small), Color(200, 200, 0), TextAlign::CENTER_LEFT, "Scanning");
      }

      // WiFi status indicator (top right)
      if (id(wifi_status).state) {
        it.filled_circle(118, 10, 3, COLOR_GREEN);
      } else {
        it.filled_circle(118, 10, 3, COLOR_RED);
      }

# Color definitions
color:
  - id: COLOR_BLACK
    red: 0%
    green: 0%
    blue: 0%
  - id: COLOR_WHITE
    red: 100%
    green: 100%
    blue: 100%
  - id: COLOR_GREEN
    red: 0%
    green: 100%
    blue: 0%
  - id: COLOR_RED
    red: 100%
    green: 0%
    blue: 0%
  - id: COLOR_ORANGE
    red: 100%
    green: 65%
    blue: 0%

# Fonts for the display
font:
  - file: "gfonts://Roboto@bold"
    id: font_title
    size: 14
  - file: "gfonts://Roboto@bold"
    id: font_large
    size: 28
  - file: "gfonts://Roboto"
    id: font_medium
    size: 18
  - file: "gfonts://Roboto"
    id: font_small
    size: 12
  - file: "gfonts://Roboto"
    id: font_tiny
    size: 9`,
    supportsWebServer: true,
    notes: ['Built-in tail485 RS485 transceiver on GPIO1/GPIO2.'],
  },
  {
    id: 'esp32p4-evboard',
    label: 'ESP32-P4 Function EV Board (32MB PSRAM, C6 Wi-Fi)',
    chip: 'esp32p4', board: 'esp32-p4-function-ev-board', variant: 'esp32p4',
    flash_size: '16MB',
    // Same table for both: unlike the 8 MB boards (1.75 -> 2.0 MB slots for BLE), the
    // P4's 3 MB slots already fit the BLE build with room to spare (measured 1.80 MB,
    // 49%), so enabling BLE here needs no repartition.
    partitions: { default: 'partitions/tigo-16mb.csv', ble: 'partitions/tigo-16mb.csv' },
    psram: { mode: 'hex', speed: '200MHz' },
    // execute_from_psram (XIP) is required on the P4 so PSRAM-resident task
    // stacks stay reachable during flash cache-disable windows — without it some
    // boards crash-loop at boot with esp_task_stack_is_sane_cache_disabled (#31).
    frameworkAdvanced: { enable_idf_experimental_features: true, execute_from_psram: true },
    frameworkComponents: ['joltwallet/littlefs^1.16'],
    hostedComponent: { source: 'https://github.com/RAR/esp_tsdb.git', ref: 'tigomonitor' },
    sdkconfig: {
      CONFIG_ESP32P4_DEFAULT_CPU_FREQ_400: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '16384',
      CONFIG_UART_TX_BUFFER_SIZE: '4096',
      CONFIG_FREERTOS_HZ: '1000',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '128',
      CONFIG_FREERTOS_USE_TICKLESS_IDLE: 'n',
      // See the AtomS3R entry — esp_tsdb needs stat() for its size stats, and this
      // board runs the same History/tsdb stack.
      CONFIG_VFS_SUPPORT_DIR: 'y',
      CONFIG_LWIP_MAX_SOCKETS: '16',
      CONFIG_LWIP_MAX_ACTIVE_TCP: '16',
      CONFIG_LWIP_MAX_LISTENING_TCP: '16',
    },
    // Emitted only when BLE is selected. Required for repeatable CCA-over-BLE here:
    // BLE is hosted (Bluedroid host on the P4, controller on the C6 over VHCI), and
    // once a session completes Bluedroid switches from LE_Create_Connection (0x200D)
    // to LE_Extended_Create_Connection (0x2043). The C6 accepts the first and refuses
    // the second with HCI Cmd Disallowed, so every connect after the first failed with
    // GATT 133. Dropping BLE 5.0 removes the extended initiator from l2cble_create_conn
    // and keeps the legacy path the C6 honours. Verified on an EV board: 3/3 refreshes.
    // Cost is extended advertising/scanning, which the CCA does not use.
    sdkconfigBle: {
      CONFIG_BT_BLE_50_FEATURES_SUPPORTED: 'n',
    },
    hosted: {
      variant: 'ESP32C6', slot: 1, active_high: true,
      clk_pin: 'GPIO18', cmd_pin: 'GPIO19',
      d0_pin: 'GPIO14', d1_pin: 'GPIO15', d2_pin: 'GPIO16', d3_pin: 'GPIO17',
      reset_pin: 'GPIO54',
    },
    uartDefault: { tx_pin: 'GPIO20', rx_pin: 'GPIO21', rx_buffer_size: 16384 },
    numberOfDevices: 100,
    // BLE works even though the P4 has no radio: ESPHome's esp32_ble detects an
    // esp32_hosted config and switches to CONFIG_BT_CONTROLLER_DISABLED +
    // CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID, running the Bluedroid host on the P4
    // against the C6's controller over VHCI. Requires C6 slave firmware built with BT.
    supports: { ble: true, display: false },
    displayOverlay: null,
    notes: [
      'No native Wi-Fi — uses an ESP32-C6 companion over SDIO (esp32_hosted).',
      'BLE runs over the C6 companion (Bluedroid host on the P4, controller on the C6 via VHCI). Verified on an EV board: repeated CCA sessions work and Wi-Fi is unaffected. Selecting BLE adds CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n, which is required — without it only the first connect per boot succeeds, because Bluedroid switches to LE_Extended_Create_Connection after a session and the C6 refuses that command. This trades away extended advertising/scanning, which the CCA does not use.',
      'PSRAM is hex mode only; valid speeds are 20/100/200 MHz (200 default). If a specific board crash-loops at boot, drop to 100 or 20 MHz — a per-board PSRAM quirk, not a universal limit.',
    ],
    supportsWebServer: true,
  },
  {
    // Mirrors boards/esp32-lilygo-t-can485.yaml (PR #44). Deliberately the same
    // narrow tier that file defines: sensors to Home Assistant, nothing else.
    //
    // This board cannot have PSRAM — LilyGO wired GPIO16/17 to the RS485 front
    // end, which are the pins a WROVER uses for its PSRAM die. tigo_server
    // builds whole HTML pages and JSON responses in memory from the httpd task,
    // so it needs PSRAM; tigo_monitor's tables are a few KB and do not. Hence
    // supportsWebServer: false, and with the web server goes the dashboard, the
    // REST API, CCA/cloud import and the BLE bridge.
    //
    // History is out of reach regardless: the smallest tsdb layout wants ~7MB
    // (dual OTA slots + a 3MB LittleFS partition) and this board has 4MB. So no
    // partitions, and no esp_tsdb/littlefs in frameworkComponents.
    id: 'esp32-lilygo-t-can485',
    label: 'LilyGO T-CAN485 (classic ESP32, no PSRAM — Home Assistant only)',
    chip: 'esp32', board: 'esp32dev', variant: null,
    flash_size: '4MB',
    // Stock ESPHome 4MB table (two 1.75MB OTA slots). The repo's tigo-*.csv
    // tables carve out a tsdb partition and assume 8/16MB.
    partitions: null,
    psram: null,
    frameworkAdvanced: {
      enable_idf_experimental_features: false,
      execute_from_psram: false,
      // 3.0 (ECO3), not 3.1: every revision gate in IDF 5.5.5 tests
      // CONFIG_ESP32_REV_MIN_FULL >= 300, nothing tests >= 301, and ESPHome
      // turns this into a hard boot-time floor. 3.1 would buy no flash and
      // refuse to boot on the common ECO3 parts.
      minimum_chip_revision: '3.0',
      // Hands SRAM1 to IRAM (+40KB), which is where CONFIG_UART_ISR_IN_IRAM's
      // budget comes from. Needs an ESP-IDF v5.1+ bootloader — see notes.
      sram1_as_iram: true,
    },
    frameworkComponents: [],
    hostedComponent: null,
    sdkconfig: {
      // The single most important setting for frame loss: without it the UART
      // ISR stalls whenever flash is busy and Tigo frames drop mid-burst.
      CONFIG_UART_ISR_IN_IRAM: 'y',
      // The classic ESP32 defaults to 160MHz under ESP-IDF; the headroom goes
      // to keeping up with the bus while WiFi is active.
      CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240: 'y',
      CONFIG_LOG_DEFAULT_LEVEL_INFO: 'y',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO22', rx_pin: 'GPIO21', rx_buffer_size: 2048 },
    numberOfDevices: 20,
    supports: { ble: false, display: false },
    supportsWebServer: false,
    // The Tigo bus is on GPIO21/22, a different hardware UART from the USB
    // console on GPIO1/3 — so unlike the AtomS3R this board keeps its serial
    // console and must NOT get `logger: baud_rate: 0`.
    keepSerialConsole: true,
    // MAX13487E front end. At reset the ESP32 leaves these floating, so the
    // transceiver comes up undefined and you get no bus data. All three must be
    // driven HIGH, each for a different reason (see the board file). `internal`
    // keeps them out of Home Assistant — board wiring, not user controls.
    gpioSwitches: [
      { id: 'rs485_power_5v', name: 'RS485 5V Enable', pin: 'GPIO16',
        comment: '5V_EN — ME2107 boost that feeds the transceiver' },
      { id: 'rs485_auto_direction', name: 'RS485 AutoDirection Enable', pin: 'GPIO17',
        comment: '/RE high => the AutoDirection state machine owns the receiver' },
      { id: 'rs485_chip_enable', name: 'RS485 Chip Enable', pin: 'GPIO19',
        comment: 'SHDN high => normal operation (low shuts the whole chip down)' },
    ],
    displayOverlay: null,
    notes: [
      'Sensors only, published to Home Assistant. No web dashboard, REST API, on-flash history, CCA import or BLE bridge — all of those live in tigo_server, which needs PSRAM this board cannot have.',
      'Built-in isolated RS485 transceiver on GPIO21/GPIO22; the USB serial console stays available on UART0.',
      '⚠ Flash this board over USB the first time. sram1_as_iram needs an ESP-IDF v5.1+ bootloader — a USB flash updates the bootloader, an OTA does not, so OTA-ing an existing device onto this config leaves it unable to boot.',
      'Tested at 18 devices; 40 should fit. Each optimizer costs roughly 600 bytes across the device and node tables, against the ~130KB free once WiFi is up — the practical ceiling is heap fragmentation, not total bytes.',
    ],
  },
];

export function getBoard(id) {
  return BOARDS.find((b) => b.id === id);
}
