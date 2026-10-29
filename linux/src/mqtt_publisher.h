#pragma once

// Publishes to Home Assistant over MQTT, using MQTT discovery so panels appear
// without any YAML. Each panel is its own HA device, keyed by barcode — never
// by short address, which the gateway can reassign (#74) — and linked to the
// monitor device with via_device. Panels with no barcode yet (before the
// first Frame 27, at most ~15 min on a fresh install) are held back.
//
// Topics, with <base> = base_topic/device_id:
//   <base>/status                 online / offline (last will)
//   <base>/system/state           {"power":…,"energy":…,"strings":{label:W},…}
//   <base>/panel/<barcode>/state  {"power":…,"voltage_in":…, …}
//   <base>/master/state           {"status":"active","problem":false,…}
//                                 (the bus master role only)

#include "bus_master.h"
#include "monitor.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>

struct mosquitto;

namespace tigod {

class MqttPublisher {
 public:
  MqttPublisher(const Monitor &monitor, std::string version);
  ~MqttPublisher();

  // (Re)connects with new settings; an empty host turns MQTT off.
  void configure(const Settings &settings);
  // Publishes discovery for new panels and the current state. Call every
  // publish_interval_s.
  void publish();

  // Where publish() reads the bus master's status, in the master role.
  void set_master_status(std::function<MasterStatus()> f) { master_status_ = std::move(f); }

  bool connected() const { return connected_; }
  std::string last_error() const;

 private:
  void stop_();
  void publish_discovery_(const Panel &p);
  void publish_system_discovery_();
  void publish_string_discovery_(const std::string &label);
  void publish_master_discovery_(bool remove);
  void publish_master_();
  bool pub_(const std::string &topic, const std::string &payload, bool retain);
  // A discovery config, or with discovery off the empty retained message
  // that removes it.
  void announce_(const std::string &component, const std::string &uid, const nlohmann::json &config,
                 bool remove = false);
  std::string base_() const;

  const Monitor &monitor_;
  std::string version_;
  Settings settings_;
  mosquitto *mosq_ = nullptr;
  std::atomic<bool> connected_{false};
  std::atomic<bool> need_discovery_{true};
  std::set<std::string> announced_, announced_strings_;
  std::function<MasterStatus()> master_status_;
  mutable std::mutex err_mu_;
  std::string last_error_;

  friend struct MqttCallbacks;
};

}  // namespace tigod
