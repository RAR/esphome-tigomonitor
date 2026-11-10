#pragma once

// Moving a barcode-keyed (YAML `long_address:`) sensor when the gateway gives
// its panel a new short address (place_barcode_sensors_()). Free of ESPHome
// types so the host test can run it on a std::map.

namespace esphome {
namespace tigo_monitor {

// Points `sensor` at `addr` in `target`, one of the per-address sensor maps,
// and frees `old_addr` only if it still holds this sensor: when two panels
// trade addresses in one re-commissioning, the other panel may already have
// moved into `old_addr`, and erasing it would leave that panel in no map.
// Returns the sensor it displaced at `addr`, if a different one was there
// (an address: entry for the same panel, say), so the caller can warn.
template <typename Map, typename Sensor, typename Addr>
Sensor *move_barcode_sensor(Map &target, Sensor *sensor, const Addr &old_addr, const Addr &addr) {
  if (!old_addr.empty()) {
    auto it = target.find(old_addr);
    if (it != target.end() && it->second == sensor) target.erase(it);
  }
  Sensor *displaced = nullptr;
  auto at = target.find(addr);
  if (at != target.end() && at->second != sensor) displaced = at->second;
  target[addr] = sensor;
  return displaced;
}

}  // namespace tigo_monitor
}  // namespace esphome
