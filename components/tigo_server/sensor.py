"""Sensor platform for Tigo Server devices."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_ADDRESS,
    CONF_NAME,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    STATE_CLASS_MEASUREMENT,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT
)
from . import tigo_server_ns, TigoServerComponent, CONF_TIGO_SERVER_ID

DEPENDENCIES = ['tigo_server']

# Define specific sensor configs
CONF_POWER = "power"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_CURRENT_IN = "current_in"
CONF_TEMPERATURE = "temperature"
CONF_RSSI = "rssi"
CONF_BARCODE = "barcode"

def _tigo_sensor_schema(**kwargs):
    """Create a sensor schema that allows empty configs for auto-templating"""
    base_schema = sensor.sensor_schema(**kwargs)
    # Make id and name optional by removing the validator that requires them
    schema_dict = base_schema.schema.copy()
    # Make ID and NAME optional 
    schema_dict[cv.Optional(CONF_ID)] = cv.declare_id(sensor.Sensor)
    schema_dict[cv.Optional(CONF_NAME)] = cv.string
    return cv.Schema(schema_dict)

def _tigo_text_sensor_schema(**kwargs):
    """Create a text sensor schema that allows empty configs for auto-templating"""
    base_schema = text_sensor.text_sensor_schema(**kwargs)
    # Make id and name optional by removing the validator that requires them
    schema_dict = base_schema.schema.copy()
    # Make ID and NAME optional 
    schema_dict[cv.Optional(CONF_ID)] = cv.declare_id(text_sensor.TextSensor)
    schema_dict[cv.Optional(CONF_NAME)] = cv.string
    return cv.Schema(schema_dict)

def _auto_template_sensor_config(config):
    """Auto-template sensor configs with name and id if not provided"""
    base_name = config[CONF_NAME]
    
    # Sensor configurations with their suffixes
    sensor_configs = [
        (CONF_POWER, "Power"),
        (CONF_VOLTAGE_IN, "Voltage In"),
        (CONF_VOLTAGE_OUT, "Voltage Out"),
        (CONF_CURRENT_IN, "Current"),
        (CONF_TEMPERATURE, "Temperature"),
        (CONF_RSSI, "RSSI"),
        (CONF_BARCODE, "Barcode"),
    ]
    
    for conf_key, suffix in sensor_configs:
        if conf_key in config:
            sensor_config = config[conf_key]
            
            # Auto-generate name if not provided
            if CONF_NAME not in sensor_config:
                sensor_config[CONF_NAME] = f"{base_name} {suffix}"
                
            # Auto-generate ID if not provided
            if CONF_ID not in sensor_config:
                # Create a valid ID by converting to lowercase and replacing spaces/hyphens with underscores
                base_id = base_name.lower().replace(' ', '_').replace('-', '_')
                suffix_id = suffix.lower().replace(' ', '_').replace('-', '_')
                id_string = f"{base_id}_{suffix_id}"
                # Use appropriate sensor type for ID declaration
                if conf_key == CONF_BARCODE:
                    sensor_config[CONF_ID] = cv.declare_id(text_sensor.TextSensor)(id_string)
                else:
                    sensor_config[CONF_ID] = cv.declare_id(sensor.Sensor)(id_string)
            
            # Add default fields for text sensors (skip None values to avoid C++ generation issues)
            if conf_key == CONF_BARCODE:
                if "disabled_by_default" not in sensor_config:
                    sensor_config["disabled_by_default"] = False
    
    return config

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(CONF_TIGO_SERVER_ID): cv.use_id(TigoServerComponent),
        cv.Required(CONF_ADDRESS): cv.string,
        cv.Required(CONF_NAME): cv.string,
        cv.Optional(CONF_POWER): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_IN): _tigo_sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_OUT): _tigo_sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_IN): _tigo_sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMPERATURE): _tigo_sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_RSSI): _tigo_sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BARCODE): _tigo_text_sensor_schema(),
    }).extend(cv.COMPONENT_SCHEMA),
    _auto_template_sensor_config,
)

async def to_code(config):
    hub = await cg.get_variable(config[CONF_TIGO_SERVER_ID])
    address = config[CONF_ADDRESS]
    
    # Define sensor configurations with their methods
    sensor_configs = [
        (CONF_POWER, hub.add_power_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_IN, hub.add_voltage_in_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_OUT, hub.add_voltage_out_sensor, sensor.new_sensor),
        (CONF_CURRENT_IN, hub.add_current_in_sensor, sensor.new_sensor),
        (CONF_TEMPERATURE, hub.add_temperature_sensor, sensor.new_sensor),
        (CONF_RSSI, hub.add_rssi_sensor, sensor.new_sensor),
        (CONF_BARCODE, hub.add_barcode_sensor, text_sensor.new_text_sensor),
    ]
    
    # Process each configured sensor type (auto-templating already done in validation)
    for conf_key, add_method, new_sensor_method in sensor_configs:
        if conf_key in config:
            sensor_config = config[conf_key]
            # Create the sensor object from the config
            sens = await new_sensor_method(sensor_config)
            # Register sensor with the hub using cg.add() to generate C++ code
            cg.add(add_method(address, sens))