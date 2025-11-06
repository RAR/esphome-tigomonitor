import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import tigo_monitor
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ['tigo_monitor']
CODEOWNERS = ['@yourusername']

tigo_server_ns = cg.esphome_ns.namespace('tigo_server')
TigoWebServer = tigo_server_ns.class_('TigoWebServer', cg.Component)

CONF_TIGO_MONITOR_ID = 'tigo_monitor_id'
CONF_API_TOKEN = 'api_token'

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(TigoWebServer),
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(tigo_monitor.TigoMonitorComponent),
    cv.Optional(CONF_PORT, default=80): cv.port,
    cv.Optional(CONF_API_TOKEN): cv.string,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    # Get the tigo_monitor component
    parent = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])
    cg.add(var.set_tigo_monitor(parent))
    
    # Set the port
    cg.add(var.set_port(config[CONF_PORT]))
    
    # Set the API token if provided
    if CONF_API_TOKEN in config:
        cg.add(var.set_api_token(config[CONF_API_TOKEN]))

