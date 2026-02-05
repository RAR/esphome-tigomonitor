# Troubleshooting Guide

Common issues and solutions for ESPHome Tigo Monitor.

## Quick Fixes

| Symptom | Solution |
|---------|----------|
| No devices discovered | Check UART wiring, verify 38400 baud |
| High packet loss | Add `CONFIG_UART_ISR_IN_IRAM: "y"` |
| Memory exhaustion | Use ESP32-S3 with PSRAM |
| CCA sync fails | Verify CCA IP, check network connectivity |
| Web UI not loading | Confirm `tigo_server` configured, check ESP32 IP |

---

## Device Discovery Issues

### No Devices Found

**Symptoms:** No "Frame received" or "New device discovered" log messages.

**Solutions:**
1. Verify UART wiring (TX→RX, RX→TX)
2. Confirm baud rate is 38400
3. Check Tigo system is powered and communicating
4. Look for any "Frame" messages in ESPHome logs

### Devices Found But No Barcodes

**Symptoms:** Devices show as "Module XXXX" instead of barcode.

**Explanation:** Frame 27 (16-char barcode) may not transmit immediately. This is normal.

**Solutions:**
- Wait longer (barcodes come from periodic Frame 27)
- Devices work without barcodes
- Use CCA sync to get panel names

---

## UART Communication

### "Packet missed!" Errors

**Cause:** UART interrupt not in IRAM, causing missed frames.

**Solution:** Add to ESP-IDF config:
```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"
```

**Expected miss rate:** 0.02-0.04% is excellent for multi-drop RS485.

### High Frame Loss Rate

**Solutions:**
1. Increase RX buffer: `rx_buffer_size: 8192`
2. Ensure `CONFIG_UART_ISR_IN_IRAM: "y"` is set
3. Check for electrical interference on RS485 line
4. Verify proper termination on RS485 bus

---

## Memory Issues

### Socket Creation Failures

**Symptoms:** 
- `Failed to create socket` in logs
- Web interface crashes
- System instability

**Cause:** Internal RAM exhaustion without PSRAM.

**Solutions:**
1. **Immediate:** Reboot ESP32
2. **Permanent:** Upgrade to ESP32-S3 with PSRAM (e.g., M5Stack AtomS3R)

### Memory Limits

| Configuration | Device Limit | Notes |
|---------------|--------------|-------|
| Without PSRAM | ~10 devices | Not recommended, unstable with web UI |
| With PSRAM | 36+ devices | Tested stable |

---

## CCA Integration

### Sync Not Working

**Checklist:**
1. CCA IP address correct?
2. ESP32 can reach CCA on network?
3. CCA responding? (Test: `http://cca-ip/` in browser)
4. Check logs for "CCA Sync complete" or error messages

### Barcode Mismatch

**Cause:** CCA barcodes may differ slightly from UART Frame 27 barcodes.

**Solution:** Component uses fuzzy matching (last 6 chars, 1-char tolerance). If still not matching, check logs for discovered barcodes and CCA barcodes.

---

## Web Interface

### Not Accessible

**Checklist:**
1. `tigo_server` component configured?
2. Correct ESP32 IP? (Check ESPHome logs)
3. Port 80 not blocked?
4. Try direct IP: `http://x.x.x.x/`

### Slow or Unresponsive

**Solutions:**
1. Use ESP32-S3 with PSRAM
2. Reduce `number_of_devices` if set too high
3. Check heap memory on `/status` page

---

## Energy Data

### Data Resets on Reboot

**Normal behavior:** Component saves energy hourly to reduce flash wear.

**Maximum data loss:** 1 hour of energy accumulation on unexpected reboot.

**Logs to verify:**
- `Restored total energy: X.XX kWh`
- `Energy data saved at hour boundary`

### Energy Not Accumulating

**Checklist:**
1. Sensors publishing? (Check HA entities)
2. Night mode active? (Check binary sensor)
3. Power readings valid? (Non-zero during daylight)

---

## Night Mode

**Normal behavior:** After 60 minutes without data:
- Publishes zeros every 10 minutes
- Prevents stale data in Home Assistant
- Temperatures show as unavailable

**Customize timeout:**
```yaml
tigo_monitor:
  night_mode_timeout: 90  # Minutes (1-1440)
```

---

## Compilation Errors

### "has no member" Error

**Cause:** Method renamed during refactoring.

**Solution:** Update to latest component version, or check for stale method names.

### ESP-IDF Errors

**Solutions:**
1. Use ESPHome 2025.10.3+
2. Set `framework: type: esp-idf, version: recommended`
3. Try clean compile: `esphome clean <yaml> && esphome compile <yaml>`

---

## Reset Commands

### Reset Node Table

Clears all device mappings:

```yaml
button:
  - platform: tigo_monitor
    name: "Reset Node Table"
    tigo_monitor_id: tigo_hub
    button_type: reset_node_table
```

### Force CCA Re-sync

```yaml
button:
  - platform: tigo_monitor
    name: "Sync from CCA"
    tigo_monitor_id: tigo_hub
    button_type: sync_from_cca
```

---

## Log Messages Reference

| Message | Meaning |
|---------|---------|
| `Frame 27 received` | Long address discovery (16-char barcode) |
| `New device discovered` | First power data from device |
| `Assigned sensor index X` | Device mapped to sensor slot |
| `Energy data saved at hour boundary` | Hourly persistence complete |
| `No Frame 27 long address available` | Device found, waiting for barcode |
| `node table is full` | Increase `number_of_devices` |
| `Maximum number of devices reached` | At configured limit |
| `CCA Sync complete` | Successfully synced with CCA |
| `Packet missed!` | UART frame loss (add ISR to IRAM) |

---

## Getting Help

1. Check ESPHome logs for error messages
2. Review `/status` page for memory info
3. Search existing GitHub issues
4. Open new issue with:
   - ESPHome version
   - Hardware (board, PSRAM)
   - Relevant log output
   - Configuration (sanitized)
