"""ESPHome external component for Tigo Server communication."""
import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, time as time_, esp32
from esphome.components.esp32.const import VARIANT_ESP32S3
from esphome.components.psram import DOMAIN as PSRAM_DOMAIN
from esphome.const import CONF_ID, CONF_UART_ID, CONF_TIME_ID, CONF_NAME
from esphome.core import coroutine, CORE

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ['uart']

tigo_monitor_ns = cg.esphome_ns.namespace('tigo_monitor')
TigoMonitorComponent = tigo_monitor_ns.class_('TigoMonitorComponent', cg.PollingComponent, uart.UARTDevice)

CONF_TIGO_MONITOR_ID = 'tigo_monitor_id'
CONF_NUMBER_OF_DEVICES = 'number_of_devices'
CONF_CCA_IP = 'cca_ip'
CONF_SYNC_CCA_ON_STARTUP = 'sync_cca_on_startup'
CONF_RESET_AT_MIDNIGHT = 'reset_at_midnight'
CONF_INVERTERS = 'inverters'
CONF_MPPIS = 'mppts'
CONF_POWER_CALIBRATION = 'power_calibration'
CONF_NIGHT_MODE_TIMEOUT = 'night_mode_timeout'
CONF_STALE_TIMEOUT = 'stale_timeout'
CONF_HISTORY_INTERVAL = 'history_interval'

# Inverter configuration schema
INVERTER_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME): cv.string,
    cv.Required(CONF_MPPIS): cv.ensure_list(cv.string),
})

def _warn_history_wear(config):
    """Flag intervals that buy resolution with flash life.

    Each snapshot commits four databases, and every commit stalls the writer for
    seconds while holding the filesystem lock — measured at ~21 s. That cost is per
    commit, so a shorter interval spends proportionally more of the device's
    time in it, and flash wear rises the same way. Runs as a validator rather
    than in to_code so `esphome config` surfaces it too.
    """
    minutes = config[CONF_HISTORY_INTERVAL]
    if minutes < 15:
        _LOGGER.warning(
            "history_interval is %d min, %.1fx more often than the 30 min "
            "default. Each snapshot takes ~21 s to commit and holds the "
            "filesystem for all of it, so history pages queue behind it that "
            "much more often, and flash wear rises by the same factor. "
            "Per-panel history also shrinks to ~%d days.",
            minutes,
            30 / minutes,
            round(5404 * minutes / 1440),
        )
    return config


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(TigoMonitorComponent),
    cv.GenerateID(CONF_UART_ID): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_NUMBER_OF_DEVICES, default=20): cv.int_range(min=1, max=100),
    cv.Optional(CONF_CCA_IP): cv.string,
    cv.Optional(CONF_SYNC_CCA_ON_STARTUP, default=True): cv.boolean,
    cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
    cv.Optional(CONF_RESET_AT_MIDNIGHT, default=False): cv.boolean,
    cv.Optional(CONF_INVERTERS): cv.ensure_list(INVERTER_SCHEMA),
    cv.Optional(CONF_POWER_CALIBRATION, default=1.0): cv.float_range(min=0.5, max=2.0),
    cv.Optional(CONF_NIGHT_MODE_TIMEOUT, default=60): cv.int_range(min=1, max=1440),  # 1 minute to 24 hours
    cv.Optional(CONF_STALE_TIMEOUT, default=10): cv.int_range(min=0, max=1440),  # minutes; 0 disables staleness zeroing
    # How often on-flash history is written. Bounds mirror kMin/kMaxSnapshotIntervalMin
    # in tigo_history.h. The floor is not arbitrary: a commit holds the flash lock
    # ~21 s with the panel rings full, so 5 min is already ~7% duty cycle.
    cv.Optional(CONF_HISTORY_INTERVAL, default=30): cv.int_range(min=5, max=1440),
}).extend(cv.polling_component_schema('30s')).extend(uart.UART_DEVICE_SCHEMA), _warn_history_wear)

@coroutine
def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield uart.register_uart_device(var, config)
    
    cg.add(var.set_number_of_devices(config[CONF_NUMBER_OF_DEVICES]))
    cg.add(var.set_snapshot_interval_min(config[CONF_HISTORY_INTERVAL]))

    
    if CONF_CCA_IP in config:
        cg.add(var.set_cca_ip(config[CONF_CCA_IP]))
        cg.add(var.set_sync_cca_on_startup(config[CONF_SYNC_CCA_ON_STARTUP]))
    
    if CONF_TIME_ID in config:
        time_id = yield cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(time_id))
        cg.add_define("USE_TIME")
    
    if config[CONF_RESET_AT_MIDNIGHT]:
        if CONF_TIME_ID not in config:
            raise cv.Invalid("reset_at_midnight requires a time_id to be configured")
        cg.add(var.set_reset_at_midnight(True))
    
    # Set power calibration multiplier
    cg.add(var.set_power_calibration(config[CONF_POWER_CALIBRATION]))
    
    # Set night mode timeout (convert minutes to milliseconds)
    cg.add(var.set_night_mode_timeout(config[CONF_NIGHT_MODE_TIMEOUT] * 60000))
    
    # Per-device staleness cutoff (minutes -> ms; 0 disables)
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT] * 60000))
    
    # Configure inverters if provided
    if CONF_INVERTERS in config:
        for inverter_config in config[CONF_INVERTERS]:
            inverter_name = inverter_config[CONF_NAME]
            mppts = inverter_config[CONF_MPPIS]
            # Pass the Python list directly - ESPHome will convert it
            cg.add(var.add_inverter(inverter_name, mppts))
    
    # Add ESP-IDF HTTP client component dependency
    esp32.include_builtin_idf_component("esp_http_client")

    # ESP-IDF 6.0 removed the built-in `json` component that bundled cJSON.
    # Our C++ includes "cJSON.h" (tigo_monitor.cpp, tigo_web_server.cpp), so on
    # IDF >= 6 we must pull cJSON in as a managed component from the registry.
    # On IDF 5.x it is still built-in; adding it there would collide, so guard
    # on the version. Both components compile into the same `src` target, so
    # declaring the dependency once here covers tigo_server as well.
    if esp32.idf_version() >= cv.Version(6, 0, 0):
        esp32.add_idf_component(name="espressif/cjson", ref="^1.7.19")

    # XIP-from-PSRAM. This is not a performance tuning knob — it is what keeps
    # the history writer from bricking the device.
    #
    # Every tsdb commit takes ESP-IDF's cross-core cache-disable
    # (spi_flash_disable_interrupts_caches_and_other_cpu). With code executing
    # from flash, that stall raced WiFi/BLE-coex radio ISRs and produced the
    # "Fault - Unknown" crashes in docs/tsdb-flash-crash-issue.md — mean
    # time-to-failure measured at 13.5-42 h on an AtomS3R.
    #
    # IDF 5.5.5 spi_flash_os_func_app.c:29 compiles cache_disable out entirely
    # when SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA are both set, which is
    # exactly the pair ESPHome's `execute_from_psram: true` sets. Setting them
    # here means users get the fix without having to know it exists; a config
    # that already sets the flag lands on the same two values, so there is no
    # conflict.
    #
    # Gated the same way ESPHome gates its own option (esp32/__init__.py:544):
    # S3-only, and PSRAM must be configured. It costs ~1.7 MiB of PSRAM, which
    # moves out of the heap to hold relocated instructions and rodata.
    if esp32.get_esp32_variant() == VARIANT_ESP32S3 and PSRAM_DOMAIN in CORE.config:
        esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_FETCH_INSTRUCTIONS", True)
        esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_RODATA", True)

