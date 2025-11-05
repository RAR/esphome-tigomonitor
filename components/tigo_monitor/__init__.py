"""ESPHome external component for Tigo Server communication."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_UART_ID
from esphome.core import coroutine

DEPENDENCIES = ['uart']

tigo_monitor_ns = cg.esphome_ns.namespace('tigo_monitor')
TigoMonitorComponent = tigo_monitor_ns.class_('TigoMonitorComponent', cg.PollingComponent, uart.UARTDevice)

CONF_TIGO_MONITOR_ID = 'tigo_monitor_id'
CONF_NUMBER_OF_DEVICES = 'number_of_devices'

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(TigoMonitorComponent),
    cv.GenerateID(CONF_UART_ID): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_NUMBER_OF_DEVICES, default=20): cv.int_range(min=1, max=100),
}).extend(cv.polling_component_schema('30s')).extend(uart.UART_DEVICE_SCHEMA)

@coroutine
def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield uart.register_uart_device(var, config)
    
    cg.add(var.set_number_of_devices(config[CONF_NUMBER_OF_DEVICES]))

