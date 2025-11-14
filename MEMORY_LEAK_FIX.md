# Memory Leak Fix - Midnight Reset

## Problem
User reported ~2KB RAM consumption every night when `reset_at_midnight` executes.

## Root Cause
Multiple functions were creating temporary `std::string` objects inside loops:

1. **`reset_peak_power()`** - Called every midnight
   - Loop through all devices (30+)
   - Created new string: `std::string pref_key = "peak_" + device.addr`
   - With 30 devices × ~25 bytes per string = ~750 bytes minimum
   - Heap fragmentation amplified the issue

2. **`save_peak_power_data()`** - Called on shutdown
   - Same pattern: loop creating temporary strings for each device

3. **`load_peak_power_data()`** - Called on startup
   - Same pattern: loop creating temporary strings for each device

4. **`save_node_table()`** - Called on shutdown
   - Two loops creating temporary strings: `"node_" + std::to_string(i)`

5. **`load_node_table()`** - Called on startup
   - Loop creating temporary strings: `"node_" + std::to_string(i)`

## Solution
Pre-allocate a static `std::string` buffer and reuse it for each iteration:

```cpp
// OLD CODE (creates new allocation each iteration):
for (size_t i = 0; i < devices_.size(); i++) {
    std::string pref_key = "peak_" + device.addr;  // NEW ALLOCATION
    uint32_t hash = esphome::fnv1_hash(pref_key);
    // ...
}

// NEW CODE (reuses same buffer):
static std::string pref_key;
pref_key.reserve(32);  // Pre-allocate capacity

for (size_t i = 0; i < devices_.size(); i++) {
    pref_key = "peak_";
    pref_key += device.addr;  // REUSES BUFFER
    uint32_t hash = esphome::fnv1_hash(pref_key);
    // ...
}
```

## Benefits
- **Zero heap allocations** per loop iteration (after first call)
- **Eliminates heap fragmentation** from repeated alloc/free cycles
- **Reduces RAM usage** by ~2KB at midnight reset
- **Same behavior** - string content is identical, just memory-efficient

## Functions Modified
1. `TigoMonitorComponent::reset_peak_power()` - line 1965
2. `TigoMonitorComponent::save_peak_power_data()` - line 1926
3. `TigoMonitorComponent::load_peak_power_data()` - line 1946
4. `TigoMonitorComponent::save_node_table()` - line 1879
5. `TigoMonitorComponent::load_node_table()` - line 1781

## Testing Recommendations
1. Monitor free heap before/after midnight reset
2. Add debug logging: `ESP_LOGD(TAG, "Free heap: %u", ESP.getFreeHeap());`
3. Verify peak power persistence still works correctly
4. Confirm node table save/load functions properly
5. Test with 30+ devices to validate memory improvement

## Expected Results
- **Before**: ~2KB RAM lost each midnight reset
- **After**: No measurable RAM loss at midnight reset
- Long-term stability improved for 24/7 operation
