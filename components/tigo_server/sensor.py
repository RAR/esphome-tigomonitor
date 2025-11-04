"""Sensor platform for Tigo Server devices."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    CONF_ADDRESS,
    CONF_NAME,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    UNIT_WATT
)
from . import tigo_server_ns, TigoServerComponent, CONF_TIGO_SERVER_ID

DEPENDENCIES = ['tigo_server']

CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_SERVER_ID): cv.use_id(TigoServerComponent),
    cv.Required(CONF_ADDRESS): cv.string,
})

async def to_code(config):
    hub = await cg.get_variable(config[CONF_TIGO_SERVER_ID])
    device_addr = config[CONF_ADDRESS]
    
    # Create a single power sensor with all other metrics as attributes
    sens = await sensor.new_sensor(config)
    cg.add(hub.add_tigo_sensor(device_addr, sens))