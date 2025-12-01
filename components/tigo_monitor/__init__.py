"""ESPHome external component for Tigo Server communication."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, time as time_
from esphome.const import CONF_ID, CONF_UART_ID, CONF_TIME_ID, CONF_NAME
from esphome.core import coroutine
from esphome import pins

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
CONF_FLOW_CONTROL_PIN = 'flow_control_pin'

# Inverter configuration schema
INVERTER_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME): cv.string,
    cv.Required(CONF_MPPIS): cv.ensure_list(cv.string),
})

CONFIG_SCHEMA = cv.Schema({
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
    cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
}).extend(cv.polling_component_schema('30s')).extend(uart.UART_DEVICE_SCHEMA)

@coroutine
def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield uart.register_uart_device(var, config)
    
    cg.add(var.set_number_of_devices(config[CONF_NUMBER_OF_DEVICES]))
    
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
    
    # Configure inverters if provided
    if CONF_INVERTERS in config:
        for inverter_config in config[CONF_INVERTERS]:
            inverter_name = inverter_config[CONF_NAME]
            mppts = inverter_config[CONF_MPPIS]
            # Pass the Python list directly - ESPHome will convert it
            cg.add(var.add_inverter(inverter_name, mppts))
    
    # Configure flow control pin (RS485 DE/RE control)
    if CONF_FLOW_CONTROL_PIN in config:
        pin = yield cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    
    # Add ESP-IDF HTTP client component dependency
    cg.add_library("ESP32 HTTP Client", None)

