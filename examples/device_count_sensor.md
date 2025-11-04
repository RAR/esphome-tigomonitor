# Device Count Sensor Example

This example shows how to use the new device count sensor in your Tigo Monitor ESPHome component.

## Basic Configuration

Add this to your `sensor:` section in your ESPHome YAML:

```yaml
sensor:
  # Device Count Sensor - Shows number of discovered Tigo devices
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Discovered Devices Count"
    id: discovered_devices_count
```

## Advanced Configuration with Automation

You can use the device count sensor for automation and monitoring:

```yaml
sensor:
  # Device Count Sensor with automation
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Tigo Device Count"
    id: tigo_device_count
    on_value:
      then:
        - logger.log:
            format: "Tigo devices detected: %.0f"
            args: ['x']

# Home Assistant automation example
# You can create automations in Home Assistant based on this sensor
# For example: Alert when fewer devices than expected are detected
```

## Use Cases

1. **System Monitoring**: Track how many Tigo devices are actively communicating
2. **Fault Detection**: Alert when the device count drops below expected levels
3. **Installation Verification**: Confirm all expected devices are discovered
4. **Historical Tracking**: Monitor device availability over time

## What the Sensor Shows

- **Value**: Integer count of currently active Tigo devices
- **Updates**: Every polling interval (default 30 seconds)
- **Range**: 0 to your configured `number_of_devices`

## Important Notes

- The count only includes devices that are actively sending power data
- Devices discovered through Frame 09 or Frame 27 but not sending power data are NOT counted
- This represents "active" devices, not total discovered devices
- The count may fluctuate if devices temporarily stop communicating

## Related Sensors

Combine with other Tigo sensors for comprehensive monitoring:

```yaml
sensor:
  # Total system power
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    
  # Total energy produced  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"
    
  # Number of active devices
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Active Device Count"
```

This gives you complete visibility into your Tigo system performance and device status.