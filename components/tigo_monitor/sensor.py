"""Sensor platform for Tigo Monitor devices."""
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
    DEVICE_CLASS_ENERGY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_KILOWATT_HOURS,
    UNIT_PERCENT
)
from . import tigo_monitor_ns, TigoMonitorComponent, CONF_TIGO_MONITOR_ID

DEPENDENCIES = ['tigo_monitor']

# Define specific sensor configs
CONF_POWER = "power"
CONF_PEAK_POWER = "peak_power"
CONF_POWER_SUM = "power_sum"
CONF_ENERGY_SUM = "energy_sum"
CONF_DEVICE_COUNT = "device_count"
CONF_INVALID_CHECKSUM = "invalid_checksum"
CONF_MISSED_FRAME = "missed_frame"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_CURRENT_IN = "current_in"
CONF_DUTY_CYCLE = "duty_cycle"
CONF_TEMPERATURE = "temperature"
CONF_RSSI = "rssi"
CONF_BARCODE = "barcode"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_DEVICE_INFO = "device_info"
CONF_EFFICIENCY = "efficiency"
CONF_POWER_FACTOR = "power_factor"
CONF_LOAD_FACTOR = "load_factor"
# Memory monitoring (ESP32 only)
CONF_INTERNAL_RAM_FREE = "internal_ram_free"
CONF_INTERNAL_RAM_MIN = "internal_ram_min"
CONF_PSRAM_FREE = "psram_free"
CONF_STACK_FREE = "stack_free"

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
        (CONF_PEAK_POWER, "Peak Power"),
        (CONF_VOLTAGE_IN, "Voltage In"),
        (CONF_VOLTAGE_OUT, "Voltage Out"),
        (CONF_CURRENT_IN, "Current"),
        (CONF_DUTY_CYCLE, "Duty Cycle"),
        (CONF_TEMPERATURE, "Temperature"),
        (CONF_RSSI, "RSSI"),
        (CONF_BARCODE, "Barcode"),
        (CONF_FIRMWARE_VERSION, "Firmware Version"),
        (CONF_DEVICE_INFO, "Device Info"),
        (CONF_EFFICIENCY, "Efficiency"),
        (CONF_POWER_FACTOR, "Power Factor"),
        (CONF_LOAD_FACTOR, "Load Factor"),
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
                
                # Special handling for power sum sensor (no device-specific suffix)
                if conf_key == CONF_POWER_SUM:
                    id_string = f"{base_id}_power_sum"
                else:
                    id_string = f"{base_id}_{suffix_id}"
                    
                # Use appropriate sensor type for ID declaration
                if conf_key in [CONF_BARCODE, CONF_FIRMWARE_VERSION, CONF_DEVICE_INFO]:
                    sensor_config[CONF_ID] = cv.declare_id(text_sensor.TextSensor)(id_string)
                else:
                    sensor_config[CONF_ID] = cv.declare_id(sensor.Sensor)(id_string)
            
            # Add default fields for text sensors (skip None values to avoid C++ generation issues)
            if conf_key in [CONF_BARCODE, CONF_FIRMWARE_VERSION, CONF_DEVICE_INFO]:
                if "disabled_by_default" not in sensor_config:
                    sensor_config["disabled_by_default"] = False
    
    return config

# Schema for individual device sensors
DEVICE_CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
        cv.Required(CONF_ADDRESS): cv.string,
        cv.Required(CONF_NAME): cv.string,
        cv.Optional(CONF_POWER): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PEAK_POWER): _tigo_sensor_schema(
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
        cv.Optional(CONF_DUTY_CYCLE): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
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
        cv.Optional(CONF_FIRMWARE_VERSION): _tigo_text_sensor_schema(),
        cv.Optional(CONF_DEVICE_INFO): _tigo_text_sensor_schema(),
        cv.Optional(CONF_EFFICIENCY): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_FACTOR): _tigo_sensor_schema(
            accuracy_decimals=3,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_LOAD_FACTOR): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }).extend(cv.COMPONENT_SCHEMA),
    _auto_template_sensor_config,
)

# Schema for power sum sensor (no address required)
POWER_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for energy sum sensor (no address required)
ENERGY_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for device count sensor (no address required)
DEVICE_COUNT_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for invalid checksum counter (no address required)
INVALID_CHECKSUM_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:alert-circle-outline",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for missed frame counter (no address required)
MISSED_FRAME_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:alert-circle-outline",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Memory monitoring sensors (ESP32 only)
INTERNAL_RAM_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

INTERNAL_RAM_MIN_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

PSRAM_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

STACK_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="B",
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Main config schema that handles both types
def _validate_config(config):
    """Validate configuration and determine sensor type"""
    # Check sensor type by name keywords
    sensor_name = config.get(CONF_NAME, "").lower()
    has_energy_keywords = any(keyword in sensor_name for keyword in ["energy", "kwh", "kilowatt", "wh"])
    has_power_keywords = any(keyword in sensor_name for keyword in ["power", "watt", "total", "sum", "combined", "system"])
    has_count_keywords = any(keyword in sensor_name for keyword in ["count", "devices", "discovered", "active", "number"])
    has_checksum_keywords = any(keyword in sensor_name for keyword in ["checksum", "invalid", "crc", "error"])
    has_frame_keywords = any(keyword in sensor_name for keyword in ["frame", "missed", "lost", "dropped"])
    has_internal_ram_keywords = any(keyword in sensor_name for keyword in ["internal", "ram", "heap"])
    has_psram_keywords = "psram" in sensor_name
    has_stack_keywords = "stack" in sensor_name
    has_min_keywords = any(keyword in sensor_name for keyword in ["min", "minimum", "watermark"])
    
    # If it has keywords and no address, treat as aggregate sensor
    if CONF_ADDRESS not in config:
        if has_energy_keywords:
            return ENERGY_SUM_CONFIG_SCHEMA(config)
        elif has_power_keywords:
            return POWER_SUM_CONFIG_SCHEMA(config)
        elif has_count_keywords:
            return DEVICE_COUNT_CONFIG_SCHEMA(config)
        elif has_checksum_keywords:
            return INVALID_CHECKSUM_CONFIG_SCHEMA(config)
        elif has_frame_keywords:
            return MISSED_FRAME_CONFIG_SCHEMA(config)
        elif has_psram_keywords:
            return PSRAM_FREE_CONFIG_SCHEMA(config)
        elif has_stack_keywords:
            return STACK_FREE_CONFIG_SCHEMA(config)
        elif has_internal_ram_keywords and has_min_keywords:
            return INTERNAL_RAM_MIN_CONFIG_SCHEMA(config)
        elif has_internal_ram_keywords:
            return INTERNAL_RAM_FREE_CONFIG_SCHEMA(config)
        else:
            raise cv.Invalid("For sensors without address, use names containing 'energy'/'kwh' for energy sensors, 'power'/'total'/'sum' for power sensors, 'count'/'devices' for device count, 'checksum'/'invalid' for checksum errors, 'frame'/'missed' for frame errors, 'psram' for PSRAM free, 'stack' for stack free, 'internal ram min' for internal RAM minimum, or 'internal ram' for internal RAM free")
    elif CONF_ADDRESS in config:
        # This is a device sensor configuration
        return DEVICE_CONFIG_SCHEMA(config)
    else:
        # Default to requiring address for safety
        raise cv.Invalid("Either 'address' is required for device sensors, or use appropriate keywords for aggregate sensors")

CONFIG_SCHEMA = _validate_config

async def to_code(config):
    hub = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])
    
    # Check if this is an aggregate sensor (no address) or device sensor (has address)
    if CONF_ADDRESS not in config:
        # Check if this is energy, power, device count, or diagnostic sensor by name keywords
        sensor_name = config.get(CONF_NAME, "").lower()
        has_energy_keywords = any(keyword in sensor_name for keyword in ["energy", "kwh", "kilowatt", "wh"])
        has_count_keywords = any(keyword in sensor_name for keyword in ["count", "devices", "discovered", "active", "number"])
        has_checksum_keywords = any(keyword in sensor_name for keyword in ["checksum", "invalid", "crc", "error"])
        has_frame_keywords = any(keyword in sensor_name for keyword in ["frame", "missed", "lost", "dropped"])
        has_internal_ram_keywords = any(keyword in sensor_name for keyword in ["internal", "ram", "heap"])
        has_psram_keywords = "psram" in sensor_name
        has_stack_keywords = "stack" in sensor_name
        has_min_keywords = any(keyword in sensor_name for keyword in ["min", "minimum", "watermark"])
        
        sens = await sensor.new_sensor(config)
        if has_energy_keywords:
            cg.add(hub.add_energy_sum_sensor(sens))
        elif has_count_keywords:
            cg.add(hub.add_device_count_sensor(sens))
        elif has_checksum_keywords:
            cg.add(hub.add_invalid_checksum_sensor(sens))
        elif has_frame_keywords:
            cg.add(hub.add_missed_frame_sensor(sens))
        elif has_psram_keywords:
            cg.add(hub.add_psram_free_sensor(sens))
        elif has_stack_keywords:
            cg.add(hub.add_stack_free_sensor(sens))
        elif has_internal_ram_keywords and has_min_keywords:
            cg.add(hub.add_internal_ram_min_sensor(sens))
        elif has_internal_ram_keywords:
            cg.add(hub.add_internal_ram_free_sensor(sens))
        else:
            cg.add(hub.add_power_sum_sensor(sens))
        return
    
    # This is a device sensor configuration
    address = config[CONF_ADDRESS]
    
    # Define sensor configurations with their methods
    sensor_configs = [
        (CONF_POWER, hub.add_power_sensor, sensor.new_sensor),
        (CONF_PEAK_POWER, hub.add_peak_power_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_IN, hub.add_voltage_in_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_OUT, hub.add_voltage_out_sensor, sensor.new_sensor),
        (CONF_CURRENT_IN, hub.add_current_in_sensor, sensor.new_sensor),
        (CONF_DUTY_CYCLE, hub.add_duty_cycle_sensor, sensor.new_sensor),
        (CONF_TEMPERATURE, hub.add_temperature_sensor, sensor.new_sensor),
        (CONF_RSSI, hub.add_rssi_sensor, sensor.new_sensor),
        (CONF_BARCODE, hub.add_barcode_sensor, text_sensor.new_text_sensor),
        (CONF_FIRMWARE_VERSION, hub.add_firmware_version_sensor, text_sensor.new_text_sensor),
        (CONF_DEVICE_INFO, hub.add_device_info_sensor, text_sensor.new_text_sensor),
        (CONF_EFFICIENCY, hub.add_efficiency_sensor, sensor.new_sensor),
        (CONF_POWER_FACTOR, hub.add_power_factor_sensor, sensor.new_sensor),
        (CONF_LOAD_FACTOR, hub.add_load_factor_sensor, sensor.new_sensor),
    ]
    
    # Process each configured sensor type (auto-templating already done in validation)
    for conf_key, add_method, new_sensor_method in sensor_configs:
        if conf_key in config:
            sensor_config = config[conf_key]
            # Create the sensor object from the config
            sens = await new_sensor_method(sensor_config)
            # Register sensor with the hub using cg.add() to generate C++ code
            cg.add(add_method(address, sens))