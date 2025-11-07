"""Binary sensor platform for Tigo Monitor."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ID,
)
from . import tigo_monitor_ns, TigoMonitorComponent, CONF_TIGO_MONITOR_ID

DEPENDENCIES = ['tigo_monitor']

CONF_NIGHT_MODE = "night_mode"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
    cv.Optional(CONF_NIGHT_MODE): binary_sensor.binary_sensor_schema(
        icon="mdi:weather-night"
    ),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    parent = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])
    
    if CONF_NIGHT_MODE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_NIGHT_MODE])
        cg.add(parent.add_night_mode_sensor(sens))
