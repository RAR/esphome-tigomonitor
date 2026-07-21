export const BOARDS = [
  {
    id: 'esp32s3-atoms3r',
    label: 'M5Stack AtomS3R (ESP32-S3, 8MB PSRAM)',
    chip: 'esp32s3', board: 'm5stack-atoms3', variant: 'esp32s3',
    flash_size: '8MB',
    partitions: { default: 'partitions/tigo-8mb.csv', ble: 'partitions/tigo-8mb-ble.csv' },
    psram: { mode: 'octal', speed: '80MHz' },
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: ['zakery292/esp_tsdb^2.1.0', 'joltwallet/littlefs^1.16'],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '2048',
      CONFIG_UART_TX_BUFFER_SIZE: '512',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '32',
      CONFIG_SPIRAM_MODE_OCT: 'y',
      CONFIG_SPIRAM_SPEED_80M: 'y',
      CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: 'y',
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
    notes: ['Built-in tail485 RS485 transceiver on GPIO1/GPIO2.'],
  },
  {
    id: 'esp32s3-atoms3',
    label: 'M5Stack AtomS3 / AtomS3 Lite (ESP32-S3, no PSRAM)',
    chip: 'esp32s3', board: 'm5stack-atoms3', variant: 'esp32s3',
    psram: null,
    partitions: null,
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: [],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '1024',
      CONFIG_UART_TX_BUFFER_SIZE: '512',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '16',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO6', rx_pin: 'GPIO5', rx_buffer_size: 1024 },
    numberOfDevices: 15,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: ['No PSRAM: History/tsdb persistence is not configured on this board.'],
  },
  {
    id: 'esp32p4-evboard',
    label: 'ESP32-P4 Function EV Board (32MB PSRAM, C6 Wi-Fi)',
    chip: 'esp32p4', board: 'esp32-p4-function-ev-board', variant: 'esp32p4',
    flash_size: '16MB',
    partitions: { default: 'partitions/tigo-16mb.csv' },
    psram: { mode: 'hex', speed: '80MHz' },
    frameworkAdvanced: { enable_idf_experimental_features: false },
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
      CONFIG_LWIP_MAX_SOCKETS: '16',
      CONFIG_LWIP_MAX_ACTIVE_TCP: '16',
      CONFIG_LWIP_MAX_LISTENING_TCP: '16',
    },
    hosted: {
      variant: 'ESP32C6', slot: 1, active_high: true,
      clk_pin: 'GPIO18', cmd_pin: 'GPIO19',
      d0_pin: 'GPIO14', d1_pin: 'GPIO15', d2_pin: 'GPIO16', d3_pin: 'GPIO17',
      reset_pin: 'GPIO54',
    },
    uartDefault: { tx_pin: 'GPIO20', rx_pin: 'GPIO21', rx_buffer_size: 16384 },
    numberOfDevices: 100,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: [
      'No native Wi-Fi — uses an ESP32-C6 companion over SDIO (esp32_hosted).',
      'PSRAM defaults to 80MHz (safe on all P4 boards incl. the P4-nano). Only the Function-EV-Board can run 200MHz — for that, raise the speed and enable idf experimental features.',
    ],
  },
  {
    id: 'esp32-dev',
    label: 'Generic ESP32 DevKit (classic ESP32)',
    chip: 'esp32', board: 'esp32dev',
    psram: null,
    partitions: null,
    frameworkAdvanced: { enable_idf_experimental_features: false },
    frameworkComponents: [],
    hostedComponent: null,
    sdkconfig: {
      CONFIG_ESP32_DEFAULT_CPU_FREQ_240: 'y',
      CONFIG_UART_ISR_IN_IRAM: 'y',
      CONFIG_UART_RX_BUFFER_SIZE: '1024',
      CONFIG_UART_TX_BUFFER_SIZE: '256',
      CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE: '16',
    },
    hosted: null,
    uartDefault: { tx_pin: 'GPIO1', rx_pin: 'GPIO3', rx_buffer_size: 1024 },
    numberOfDevices: 12,
    supports: { ble: false, display: false },
    displayOverlay: null,
    notes: ['Limited RAM: keep number_of_devices low; History/tsdb not configured.'],
  },
];

export function getBoard(id) {
  return BOARDS.find((b) => b.id === id);
}
