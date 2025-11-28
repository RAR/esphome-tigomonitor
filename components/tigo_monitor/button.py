"""Button platform for Tigo Monitor buttons."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID
from . import tigo_monitor_ns, TigoMonitorComponent, CONF_TIGO_MONITOR_ID

DEPENDENCIES = ['tigo_monitor']

CONF_BUTTON_TYPE = "button_type"

TigoYamlGeneratorButton = tigo_monitor_ns.class_('TigoYamlGeneratorButton', button.Button, cg.Component)
TigoDeviceMappingsButton = tigo_monitor_ns.class_('TigoDeviceMappingsButton', button.Button, cg.Component)
TigoResetNodeTableButton = tigo_monitor_ns.class_('TigoResetNodeTableButton', button.Button, cg.Component)
TigoSyncFromCCAButton = tigo_monitor_ns.class_('TigoSyncFromCCAButton', button.Button, cg.Component)
TigoRequestGatewayVersionButton = tigo_monitor_ns.class_('TigoRequestGatewayVersionButton', button.Button, cg.Component)
TigoRequestDeviceDiscoveryButton = tigo_monitor_ns.class_('TigoRequestDeviceDiscoveryButton', button.Button, cg.Component)

# Create a configuration function that returns the appropriate schema
def get_config_schema():
    return cv.Any(
        # YAML Generator Button Schema
        button.button_schema(TigoYamlGeneratorButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Optional(CONF_BUTTON_TYPE, default="yaml_generator"): cv.one_of("yaml_generator"),
        }),
        # Device Mappings Button Schema  
        button.button_schema(TigoDeviceMappingsButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("device_mappings"),
        }),
        # Reset Node Table Button Schema
        button.button_schema(TigoResetNodeTableButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("reset_node_table"),
        }),
        # Sync from CCA Button Schema
        button.button_schema(TigoSyncFromCCAButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("sync_from_cca"),
        }),
        # Request Gateway Version Button Schema (Phase 1 UART TX)
        button.button_schema(TigoRequestGatewayVersionButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("request_gateway_version"),
        }),
        # Request Device Discovery Button Schema (Phase 1 UART TX)
        button.button_schema(TigoRequestDeviceDiscoveryButton).extend({
            cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
            cv.Required(CONF_BUTTON_TYPE): cv.one_of("request_device_discovery"),
        })
    )

CONFIG_SCHEMA = get_config_schema()

async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    
    server = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])
    cg.add(var.set_tigo_monitor(server))
