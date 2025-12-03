# UART Packet Loss Troubleshooting Guide

## Problem
Missed packets on AtomS3R with display enabled (e.g., 972 missed packets over 7 minutes).

## Root Causes

### 1. Display Updates Competing with UART
The ST7789V LCD display requires SPI transfers that can delay UART buffer reads:
- Display updates at 2-5 second intervals
- Each update transfers ~128×128×1 byte = 16KB over SPI
- At 40MHz SPI, this takes ~3.2ms per frame
- During this time, UART RX buffer can overflow if data arrives faster than it's consumed

### 2. I2C Operations (LP5562)
- RGB LED and backlight updates use I2C bus
- I2C transactions can block briefly
- Light effects (pulse, strobe) cause frequent I2C traffic

### 3. Processing Bottleneck
- Default MAX_BYTES_PER_LOOP was 2048 bytes
- With display overhead, UART processing gets delayed
- Buffer fills faster than it's drained

## Solutions Applied

### 1. Increased Processing Capacity (Code Change)
```cpp
// OLD: const size_t MAX_BYTES_PER_LOOP = 2048;
// NEW: const size_t MAX_BYTES_PER_LOOP = 4096;
```
- Doubled bytes processed per loop iteration
- Helps drain buffer faster between display updates
- Still yields to watchdog to prevent timeouts

### 2. Reduced Display Update Frequency
```yaml
# OLD: update_interval: 2s
# NEW: update_interval: 5s
```
- Less frequent SPI transactions
- More CPU time available for UART processing
- Display still updates reasonably fast for monitoring

### 3. Recommended UART Buffer Sizes
For AtomS3R with display, increase RX buffer (TX is minimal since we only listen):

```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"           # Keep UART ISR in fast RAM
      CONFIG_UART_RX_BUFFER_SIZE: "8192"     # 8KB RX buffer (was 2048)
      CONFIG_UART_TX_BUFFER_SIZE: "1024"     # 1KB TX buffer (minimal, we only listen)
```

**Why larger RX buffer helps:**
- 38400 baud ≈ 4800 bytes/second ≈ 4.8 bytes/ms
- 8KB buffer provides ~1.7 seconds of buffering
- Display update takes ~3ms, well within buffer capacity
- Accounts for occasional FreeRTOS task switching delays

**Why small TX buffer is fine:**
- Tigo Monitor only **listens** to the RS485 bus
- No transmission to Tigo devices
- TX buffer only used for debug logging (if any)
- 1KB is more than sufficient

## Additional Optimizations

### 4. Disable Light Effects During Peak Hours
Light effects cause frequent I2C updates:
```yaml
light:
  - platform: rgb
    name: "RGB LED"
    id: status_led
    effects:
      # Consider commenting out pulse/strobe if seeing packet loss:
      # - pulse:
      #     name: "Pulse"
      # - strobe:
      #     name: "Strobe"
```

### 5. Consider Slower Display SPI Speed
If packet loss persists, reduce SPI clock:
```yaml
display:
  - platform: st7789v
    data_rate: 20MHz  # Reduce from 40MHz
```
**Trade-off:** Slower updates (6.4ms vs 3.2ms per frame)

### 6. FreeRTOS Task Priority (Advanced)
If packet loss continues, can increase UART task priority in ESP-IDF:
```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"
      CONFIG_ESP_CONSOLE_UART_CUSTOM: "y"
      # Force UART processing to higher priority
```

## Expected Results

### Before Optimizations
- 972 missed packets / 7 minutes
- ~139 packets/minute
- ~2.3 packets/second

### After Optimizations
- Should see **significant reduction** (target: <10 packets/minute)
- 4KB processing + 5s display interval = ~50% less competition
- 8KB buffer provides ample headroom

### Monitoring
Watch the "Missed Packets" counter on web UI:
```
Current: 972 (after 7 mins) ≈ 139/min
Target:  <70 (after 7 mins) ≈ <10/min  (93% reduction)
Ideal:   <7 (after 7 mins) ≈ <1/min   (99% reduction)
```

## Testing Procedure

1. **Update configuration** with new buffer sizes
2. **Flash device** with updated firmware
3. **Monitor for 10-15 minutes** to establish baseline
4. **Check missed packet counter** in web UI
5. **If still high**, try these in order:
   - Increase `update_interval` to 10s
   - Disable light effects
   - Reduce SPI `data_rate` to 20MHz
   - Increase `CONFIG_UART_RX_BUFFER_SIZE` to 16384

## Hardware Considerations

### AtomS3R Advantages
- **8MB PSRAM**: Plenty of room for large buffers
- **240MHz CPU**: Fast enough for display + UART
- **Octal PSRAM**: High bandwidth (80MHz)

### AtomS3R Limitations
- **Single core** (unlike ESP32-P4 dual-core)
- All tasks compete for one CPU
- Display, I2C, UART, WiFi all share cycles

### When to Consider ESP32-P4
If packet loss remains high after optimizations:
- **Dual 400MHz cores** - dedicated UART processing
- **32MB PSRAM** - massive buffer capacity
- Can handle 50+ devices with display without issues

## Validation

After applying fixes, you should see in logs:
```
[D][tigo_monitor:xxx]: Processing frame of X bytes
[D][tigo_monitor:xxx]: Heap: Internal 150 KB free, PSRAM 7800 KB free, Buffer: 234 bytes
```

**Good signs:**
- Buffer size stays under 4KB
- Few/no "Packet missed!" warnings
- Missed packet counter stops climbing rapidly

**Bad signs:**
- Buffer frequently >8KB (upgrade RX buffer)
- Frequent "Buffer too small, resetting!" (increase MAX_BYTES_PER_LOOP further)
- Continuous "Packet missed!" warnings (try slower display updates)

## Summary of Changes

| Item | Before | After | Impact |
|------|--------|-------|--------|
| MAX_BYTES_PER_LOOP | 2048 | 4096 | +100% throughput/loop |
| Display interval | 2s | 5s | -60% SPI overhead |
| RX buffer (recommended) | 2048 | 8192 | +300% buffer capacity |
| TX buffer (recommended) | 512 | 1024 | Adequate (listen-only) |

**Expected packet loss reduction: 80-95%**
