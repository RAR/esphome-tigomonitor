# Home Assistant Integration

Guide for integrating Tigo Monitor with Home Assistant.

## Energy Dashboard

The component provides sensors compatible with Home Assistant's Energy Dashboard.

![Home Assistant Energy Dashboard](images/HA.png)

### Setup

1. Go to **Settings → Dashboards → Energy**
2. Under **Solar Panels**, click **Add Solar Production**
3. Select `sensor.total_system_energy` (or your named energy sensor)

### Required Sensor

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"  # Must include "energy" in name
```

---

## Ready-to-Use Dashboards

The repository includes complete dashboard configurations in `/examples/`:

| File | Description |
|------|-------------|
| `home-assistant-dashboard.yaml` | Full desktop dashboard |
| `home-assistant-mobile-dashboard.yaml` | Mobile-optimized layout |
| `home-assistant-automations.yaml` | Alert and reporting automations |

### Installation

1. Copy content from desired YAML file
2. Paste into Home Assistant Dashboard editor (raw mode)
3. Update entity names to match your configuration

### Entity Name Mapping

Dashboard files use default names. Update to match your setup:

```yaml
# Dashboard uses:
sensor.tigo_device_1_power

# Your setup might be:
sensor.solar_panel_east_power
```

Find and replace throughout the dashboard file.

---

## Dashboard Features

### Main Dashboard

- **System Overview**: Power, energy, device count gauges
- **Panel Grid**: Individual panel power output
- **Efficiency Trends**: Conversion efficiency over time
- **Temperature Monitoring**: Panel temperature graphs
- **Device Management**: Control buttons

### Mobile Dashboard

- **Compact Layout**: Fold-entity-row cards
- **Quick Status**: Glance cards for instant overview
- **Touch-Friendly**: Mini-graph cards
- **Smart Summaries**: Best/worst performer identification

---

## Automation Examples

### Low Efficiency Alert

```yaml
automation:
  - alias: "Panel Low Efficiency Alert"
    trigger:
      platform: numeric_state
      entity_id: sensor.panel_1_efficiency
      below: 85
      for: "00:30:00"
    action:
      service: notify.mobile_app
      data:
        message: "Panel 1 efficiency dropped below 85%"
```

### Device Count Alert

```yaml
automation:
  - alias: "Missing Devices Alert"
    trigger:
      platform: numeric_state
      entity_id: sensor.active_device_count
      below: 15  # Your expected count
    action:
      service: notify.mobile_app
      data:
        message: "Tigo system has fewer active devices than expected"
```

### High Temperature Warning

```yaml
automation:
  - alias: "Panel High Temperature"
    trigger:
      platform: numeric_state
      entity_id: sensor.panel_1_temperature
      above: 70
    action:
      service: notify.mobile_app
      data:
        message: "Panel 1 temperature exceeds 70°C"
```

### Daily Energy Report

```yaml
automation:
  - alias: "Daily Solar Report"
    trigger:
      platform: time
      at: "20:00:00"
    action:
      service: notify.mobile_app
      data:
        message: >
          Today's solar: {{ states('sensor.total_system_energy') }} kWh
          Peak: {{ states('sensor.total_system_peak_power') }} W
```

---

## Useful Sensors

### System Health

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Active Device Count"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Missed Frame Count"

binary_sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    night_mode:
      name: "Solar Night Mode"
```

### Per-Panel Monitoring

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "East Panel 1"
    power: {}
    efficiency: {}
    temperature: {}
```

---

## Template Sensors

### Best Performing Panel

```yaml
template:
  - sensor:
      - name: "Best Panel Power"
        unit_of_measurement: "W"
        state: >
          {{ [
            states('sensor.panel_1_power') | float(0),
            states('sensor.panel_2_power') | float(0),
            states('sensor.panel_3_power') | float(0)
          ] | max }}
```

### System Efficiency

```yaml
template:
  - sensor:
      - name: "System Average Efficiency"
        unit_of_measurement: "%"
        state: >
          {% set panels = [
            states('sensor.panel_1_efficiency') | float(0),
            states('sensor.panel_2_efficiency') | float(0),
            states('sensor.panel_3_efficiency') | float(0)
          ] %}
          {{ (panels | sum / panels | length) | round(1) }}
```

---

## Lovelace Cards

### Power Gauge

```yaml
type: gauge
entity: sensor.total_system_power
name: Solar Power
unit: W
min: 0
max: 10000
severity:
  green: 5000
  yellow: 2000
  red: 0
```

### Energy Today

```yaml
type: entity
entity: sensor.total_system_energy
name: Energy Today
icon: mdi:solar-power
```

### Panel Grid

```yaml
type: grid
columns: 4
cards:
  - type: entity
    entity: sensor.panel_1_power
    name: Panel 1
  - type: entity
    entity: sensor.panel_2_power
    name: Panel 2
  # ... more panels
```

---

## API Access

Access Tigo Monitor data via REST API:

```yaml
rest:
  - resource: http://192.168.1.100/api/overview
    scan_interval: 60
    sensor:
      - name: "Tigo API Power"
        value_template: "{{ value_json.total_power }}"
        unit_of_measurement: "W"
```

With authentication:

```yaml
rest:
  - resource: http://192.168.1.100/api/overview
    headers:
      Authorization: Bearer your-api-token
    scan_interval: 60
```

---

## Tips

1. **Use meaningful names** in ESPHome config for easier HA entity identification
2. **Enable energy dashboard** for long-term production tracking
3. **Set up alerts** for efficiency drops to catch issues early
4. **Use device count sensor** to detect communication problems
5. **Night mode binary sensor** useful for conditional automations
