"""Button platform for Tigo Server buttons."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID
from . import tigo_server_ns, TigoServerComponent, CONF_TIGO_SERVER_ID

DEPENDENCIES = ['tigo_server']

CONF_BUTTON_TYPE = "button_type"

TigoYamlGeneratorButton = tigo_server_ns.class_('TigoYamlGeneratorButton', button.Button, cg.Component)
TigoDeviceMappingsButton = tigo_server_ns.class_('TigoDeviceMappingsButton', button.Button, cg.Component)
TigoResetNodeTableButton = tigo_server_ns.class_('TigoResetNodeTableButton', button.Button, cg.Component)

# Create a configuration function that returns the appropriate schema
def get_config_schema():
    return cv.Any(
        # YAML Generator Button Schema
        button.button_schema(TigoYamlGeneratorButton).extend({
            cv.GenerateID(CONF_TIGO_SERVER_ID): cv.use_id(TigoServerComponent),
            cv.Optional(CONF_BUTTON_TYPE, default="yaml_generator"): cv.one_of("yaml_generator"),
        }),
        # Device Mappings Button Schema  
        button.button_schema(TigoDeviceMappingsButton).extend({
            cv.GenerateID(CONF_TIGO_SERVER_ID): cv.use_id(TigoServerComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("device_mappings"),
        }),
        # Reset Node Table Button Schema
        button.button_schema(TigoResetNodeTableButton).extend({
            cv.GenerateID(CONF_TIGO_SERVER_ID): cv.use_id(TigoServerComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("reset_node_table"),
        })
    )

CONFIG_SCHEMA = get_config_schema()

async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    
    server = await cg.get_variable(config[CONF_TIGO_SERVER_ID])
    cg.add(var.set_tigo_server(server))