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
//   <base>/master/state           {"status":"active","problem":false,"pv":"ON",…}
//                                 (the bus master role only)
//   <base>/pv/set                 ON / OFF, from Home Assistant (mqtt.pv_switch only)
//
// A standby that's standing by sends only its status and master state; the
// panels are the active master's (mqtt.standby_panels sends its readings
// too, without announcing them). Panel entities are keyed by barcode, not by
// monitor, so the one that takes the bus re-announces them with its own
// topics and Home Assistant's entities follow it.

#include "bus_master.h"
#include "monitor.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

struct mosquitto;

namespace tigod {

class MqttPublisher {
 public:
  MqttPublisher(const Monitor &monitor, std::string version);
  ~MqttPublisher();

  // (Re)connects with new settings; an empty host turns MQTT off. Returns at
  // once: the reconnect, and its lookup of the broker's name, happen on a
  // thread of their own, so a slow or unknown name can't hold up the caller.
  void configure(const Settings &settings);
  // Publishes discovery for new panels and the current state. Call every
  // publish_interval_s. Skipped while a reconfigure is under way.
  void publish();

  // Where publish() reads the bus master's status, in the master role.
  void set_master_status(std::function<MasterStatus()> f) { master_status_ = std::move(f); }
  // Whether the TAP-reconfiguration alarm is on (BusLog).
  void set_tap_reconfigured(std::function<bool()> f) { tap_reconfigured_ = std::move(f); }
  // Where a PV switch command goes (true: off); false if it was refused.
  void set_pv_command(std::function<bool(bool off)> f) { pv_command_ = std::move(f); }

  bool connected() const { return connected_; }
  std::string last_error() const;

 private:
  void stop_();
  void apply_(const Settings &settings);  // configure's work, on worker_
  void work_();
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
  bool standing_by_ = false;
  std::function<MasterStatus()> master_status_;
  std::function<bool()> tap_reconfigured_;
  std::function<bool(bool off)> pv_command_;
  void on_pv_command_(const std::string &payload);
  mutable std::mutex err_mu_;
  std::string last_error_;
  // The client and settings_: held by apply_ while it reconfigures, and
  // tried by publish().
  std::mutex client_mu_;
  // Settings waiting for the worker; the newest wins.
  std::mutex cfg_mu_;
  std::condition_variable cfg_cv_;
  std::optional<Settings> pending_;
  bool quit_ = false;
  std::thread worker_;

  friend struct MqttCallbacks;
};

}  // namespace tigod
