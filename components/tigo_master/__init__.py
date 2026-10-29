"""The ESP32 as the Tigo TAP's RS485 bus master, or a standby beside one.

The only component in this project that transmits on the bus. It owns the
RS485 uart, and tigo_monitor reads the bus through it:

    uart:
      id: rs485
      tx_pin: GPIO7
      rx_pin: GPIO8
      flow_control_pin: GPIO21   # DE/RE, for a transceiver without auto-direction
      baud_rate: 38400

    tigo_master:
      id: bus
      uart_id: rs485
      monitor_id: tigo_hub
      role: standby

    tigo_monitor:
      id: tigo_hub
      uart_id: bus

See docs/superpowers/specs/2026-10-29-esp32-bus-master-design.md.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, sensor, text_sensor, uart
from esphome.components.uart import CONF_DATA_BITS, CONF_PARITY, CONF_STOP_BITS
from esphome.const import (
    CONF_BAUD_RATE,
    CONF_ID,
    CONF_STATUS,
    CONF_UART_ID,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MILLISECOND,
    UNIT_SECOND,
)

CODEOWNERS = ["@RAR"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "tigo_monitor"]

tigo_master_ns = cg.esphome_ns.namespace("tigo_master")
TigoMasterComponent = tigo_master_ns.class_("TigoMasterComponent", cg.Component, uart.UARTComponent)
tigo_monitor_ns = cg.esphome_ns.namespace("tigo_monitor")
TigoMonitorComponent = tigo_monitor_ns.class_("TigoMonitorComponent")

CONF_MONITOR_ID = "monitor_id"
CONF_ROLE = "role"
CONF_TAKEOVER_AFTER = "takeover_after"
CONF_GATEWAY_ID = "gateway_id"
CONF_STARTUP = "startup"
CONF_HOUSEKEEPING = "housekeeping"
CONF_QUERIES = "queries"
CONF_OPTIMIZER_WRITES = "optimizer_writes"
CONF_PROBLEM = "problem"
CONF_LEASE_REMAINING = "lease_remaining"
CONF_TAKEOVERS = "takeovers"
CONF_LONGEST_PAUSE = "longest_pause"


def _gateway_id(value):
    # As gateway_id_ok() on the Pi: the bring-up hops the TAP to the id one up,
    # which must be neither the factory address nor past the top.
    value = cv.hex_int(value)
    if not 1 <= value <= 0x7FFE or value in (0x1234, 0x1235):
        raise cv.Invalid("gateway_id must be 0x0001-0x7FFE, and not 0x1234 or 0x1235")
    return value


def _writes_need_queries(config):
    if config[CONF_OPTIMIZER_WRITES] and not config[CONF_QUERIES]:
        raise cv.Invalid(f"{CONF_OPTIMIZER_WRITES} needs {CONF_QUERIES}: the writes follow the queries' answers")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TigoMasterComponent),
            cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            # No "listen": leave this component out, and nothing can transmit.
            cv.Optional(CONF_ROLE, default="standby"): cv.one_of("master", "standby", lower=True),
            cv.Optional(CONF_TAKEOVER_AFTER, default="8s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=2), max=cv.TimePeriod(seconds=60)),
            ),
            cv.Optional(CONF_GATEWAY_ID, default=0x1201): _gateway_id,
            cv.Optional(CONF_STARTUP, default=False): cv.boolean,
            cv.Optional(CONF_HOUSEKEEPING, default=False): cv.boolean,
            cv.Optional(CONF_QUERIES, default=False): cv.boolean,
            cv.Optional(CONF_OPTIMIZER_WRITES, default=False): cv.boolean,
            # What tigo_monitor's check reads off the uart it is given: the bus's.
            cv.Optional(CONF_BAUD_RATE, default=38400): cv.one_of(38400, int=True),
            cv.Optional(CONF_DATA_BITS, default=8): cv.one_of(8, int=True),
            cv.Optional(CONF_PARITY, default="NONE"): cv.one_of("NONE", upper=True),
            cv.Optional(CONF_STOP_BITS, default=1): cv.one_of(1, int=True),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:transit-connection-variant"
            ),
            cv.Optional(CONF_PROBLEM): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
            cv.Optional(CONF_LEASE_REMAINING): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_TAKEOVERS): sensor.sensor_schema(
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_LONGEST_PAUSE): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                accuracy_decimals=0,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    _writes_need_queries,
)


def _no_flash_writes(config):
    """A flash write stops both cores' cache, the master's task with them, for
    up to a second (the history commits measured 0.63 s median, 1.02 s worst).
    So the device that runs the bus keeps no history and serves no web UI."""
    full = fv.full_config.get()
    if "tigo_server" in full:
        raise cv.Invalid(
            "tigo_master runs on a thin config: leave tigo_server out. Its history writes to flash, and "
            "a flash write freezes the bus master for up to a second."
        )
    components = full.get("esp32", {}).get("framework", {}).get("components", [])
    if any("esp_tsdb" in str(c) for c in components):
        raise cv.Invalid(
            "tigo_master runs on a thin config: leave esp_tsdb out of the framework components. "
            "tigo_monitor keeps its on-flash history when esp_tsdb is there, and a flash write freezes "
            "the bus master for up to a second."
        )
    return config


FINAL_VALIDATE_SCHEMA = cv.All(
    uart.final_validate_device_schema(
        "tigo_master",
        baud_rate=38400,
        require_tx=True,
        require_rx=True,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    ),
    _no_flash_writes,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_bus(bus))
    cg.add(var.set_baud_rate(38400))
    cg.add(var.set_data_bits(8))
    cg.add(var.set_stop_bits(1))
    if CONF_MONITOR_ID in config:
        monitor = await cg.get_variable(config[CONF_MONITOR_ID])
        cg.add(var.set_monitor(monitor))
    if config[CONF_ROLE] == "standby":
        cg.add(var.set_standby(config[CONF_TAKEOVER_AFTER].total_seconds))
    cg.add(var.set_gateway_id(config[CONF_GATEWAY_ID]))
    cg.add(var.set_startup(config[CONF_STARTUP]))
    cg.add(var.set_housekeeping(config[CONF_HOUSEKEEPING]))
    cg.add(var.set_queries(config[CONF_QUERIES], config[CONF_OPTIMIZER_WRITES]))
    if CONF_STATUS in config:
        cg.add(var.set_status_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
    if CONF_PROBLEM in config:
        cg.add(var.set_problem_sensor(await binary_sensor.new_binary_sensor(config[CONF_PROBLEM])))
    if CONF_LEASE_REMAINING in config:
        cg.add(var.set_lease_sensor(await sensor.new_sensor(config[CONF_LEASE_REMAINING])))
    if CONF_TAKEOVERS in config:
        cg.add(var.set_takeovers_sensor(await sensor.new_sensor(config[CONF_TAKEOVERS])))
    if CONF_LONGEST_PAUSE in config:
        cg.add(var.set_pause_sensor(await sensor.new_sensor(config[CONF_LONGEST_PAUSE])))
