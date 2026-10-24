#include "mqtt_publisher.h"

#include "log.h"

#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <cmath>

namespace tigod {

using nlohmann::json;

struct MqttCallbacks {
  static void on_connect(mosquitto *, void *obj, int rc) {
    auto *self = static_cast<MqttPublisher *>(obj);
    if (rc != 0) {
      std::lock_guard<std::mutex> lock(self->err_mu_);
      self->last_error_ = mosquitto_connack_string(rc);
      LOGW("MQTT: broker refused the connection: %s", self->last_error_.c_str());
      return;
    }
    LOGI("MQTT: connected to %s:%d", self->settings_.mqtt.host.c_str(), self->settings_.mqtt.port);
    {
      std::lock_guard<std::mutex> lock(self->err_mu_);
      self->last_error_.clear();
    }
    self->connected_ = true;
    self->need_discovery_ = true;  // the broker may have lost retained configs
    self->pub_(self->base_() + "/status", "online", true);
  }
  static void on_disconnect(mosquitto *, void *obj, int rc) {
    auto *self = static_cast<MqttPublisher *>(obj);
    if (self->connected_) LOGW("MQTT: disconnected (%s); reconnecting", mosquitto_strerror(rc));
    self->connected_ = false;
  }
};

MqttPublisher::MqttPublisher(const Monitor &monitor, std::string version)
    : monitor_(monitor), version_(std::move(version)) {
  mosquitto_lib_init();
}

MqttPublisher::~MqttPublisher() {
  stop_();
  mosquitto_lib_cleanup();
}

std::string MqttPublisher::last_error() const {
  std::lock_guard<std::mutex> lock(err_mu_);
  return last_error_;
}

std::string MqttPublisher::base_() const { return settings_.mqtt.base_topic + "/" + settings_.device_id; }

void MqttPublisher::stop_() {
  if (mosq_ == nullptr) return;
  if (connected_) {
    pub_(base_() + "/status", "offline", true);
    mosquitto_disconnect(mosq_);
  }
  mosquitto_loop_stop(mosq_, true);
  mosquitto_destroy(mosq_);
  mosq_ = nullptr;
  connected_ = false;
}

void MqttPublisher::configure(const Settings &settings) {
  stop_();
  settings_ = settings;
  announced_.clear();
  if (settings_.mqtt.host.empty()) {
    LOGI("MQTT: no broker set; Home Assistant publishing is off");
    return;
  }
  std::string client_id = "tigomonitor-" + settings_.device_id;
  mosq_ = mosquitto_new(client_id.c_str(), true, this);
  if (mosq_ == nullptr) {
    LOGE("MQTT: out of memory");
    return;
  }
  mosquitto_connect_callback_set(mosq_, MqttCallbacks::on_connect);
  mosquitto_disconnect_callback_set(mosq_, MqttCallbacks::on_disconnect);
  mosquitto_reconnect_delay_set(mosq_, 2, 60, true);
  if (!settings_.mqtt.username.empty())
    mosquitto_username_pw_set(mosq_, settings_.mqtt.username.c_str(), settings_.mqtt.password.c_str());
  std::string will = base_() + "/status";
  mosquitto_will_set(mosq_, will.c_str(), 7, "offline", 1, true);
  int rc = mosquitto_connect_async(mosq_, settings_.mqtt.host.c_str(), settings_.mqtt.port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::lock_guard<std::mutex> lock(err_mu_);
    last_error_ = mosquitto_strerror(rc);
    LOGW("MQTT: cannot reach %s:%d: %s; retrying", settings_.mqtt.host.c_str(), settings_.mqtt.port,
         last_error_.c_str());
  }
  mosquitto_loop_start(mosq_);  // the loop thread retries a failed connect too
}

bool MqttPublisher::pub_(const std::string &topic, const std::string &payload, bool retain) {
  if (mosq_ == nullptr) return false;
  int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(), 1,
                             retain);
  return rc == MOSQ_ERR_SUCCESS;
}

static json monitor_device(const Settings &s, const std::string &version) {
  return {{"identifiers", json::array({"tigomonitor_" + s.device_id})},
          {"name", "Tigo Monitor"},
          {"manufacturer", "esphome-tigomonitor"},
          {"model", "tigomonitord"},
          {"sw_version", version}};
}

static std::string panel_name(const Panel &p) {
  if (!p.label.empty()) return p.label;
  return "Tigo " + p.barcode.substr(p.barcode.size() > 6 ? p.barcode.size() - 6 : 0);
}

struct Metric {
  const char *key, *name, *unit, *device_class, *state_class;
  int precision;
};

static const Metric PANEL_METRICS[] = {
    {"power", "Power", "W", "power", "measurement", 1},
    {"voltage_in", "Voltage in", "V", "voltage", "measurement", 2},
    {"voltage_out", "Voltage out", "V", "voltage", "measurement", 2},
    {"current_in", "Current in", "A", "current", "measurement", 3},
    {"current_out", "Current out", "A", "current", "measurement", 3},
    {"temperature", "Temperature", "°C", "temperature", "measurement", 1},
    {"duty_cycle", "Duty cycle", "%", nullptr, "measurement", 0},
    {"efficiency", "Efficiency", "%", nullptr, "measurement", 1},
    {"rssi", "RSSI", nullptr, nullptr, "measurement", 0},
};

void MqttPublisher::publish_discovery_(const Panel &p) {
  json device = {{"identifiers", json::array({"tigo_" + p.barcode})},
                 {"name", panel_name(p)},
                 {"manufacturer", "Tigo"},
                 {"model", p.reading.is_optimizer ? "TS4 optimizer" : "TS4 monitor"},
                 {"serial_number", p.barcode},
                 {"via_device", "tigomonitor_" + settings_.device_id}};
  std::string state = base_() + "/panel/" + p.barcode + "/state";
  for (const auto &m : PANEL_METRICS) {
    std::string uid = "tigo_" + p.barcode + "_" + m.key;
    json c = {{"name", m.name},
              {"unique_id", uid},
              {"state_topic", state},
              {"value_template", std::string("{{ value_json.") + m.key + " }}"},
              {"availability_topic", base_() + "/status"},
              // A panel that stops reporting (night, a fault) goes unavailable
              // rather than freezing at its last value.
              {"expire_after", 300},
              {"suggested_display_precision", m.precision},
              {"device", device}};
    if (m.unit) c["unit_of_measurement"] = m.unit;
    if (m.device_class) c["device_class"] = m.device_class;
    if (m.state_class) c["state_class"] = m.state_class;
    if (std::string(m.key) == "rssi" || std::string(m.key) == "duty_cycle") c["entity_category"] = "diagnostic";
    pub_(settings_.mqtt.discovery_prefix + "/sensor/" + uid + "/config", c.dump(), true);
  }
}

void MqttPublisher::publish_system_discovery_() {
  json device = monitor_device(settings_, version_);
  std::string state = base_() + "/system/state";
  struct Sys {
    const char *key, *name, *unit, *device_class, *state_class;
  };
  static const Sys SYS[] = {
      {"power", "Power", "W", "power", "measurement"},
      {"energy", "Energy", "kWh", "energy", "total_increasing"},
      {"panels_online", "Panels online", nullptr, nullptr, "measurement"},
  };
  for (const auto &m : SYS) {
    std::string uid = "tigomonitor_" + settings_.device_id + "_" + m.key;
    json c = {{"name", m.name},
              {"unique_id", uid},
              {"state_topic", state},
              {"value_template", std::string("{{ value_json.") + m.key + " }}"},
              {"availability_topic", base_() + "/status"},
              {"device", device}};
    if (m.unit) c["unit_of_measurement"] = m.unit;
    if (m.device_class) c["device_class"] = m.device_class;
    if (m.state_class) c["state_class"] = m.state_class;
    pub_(settings_.mqtt.discovery_prefix + "/sensor/" + uid + "/config", c.dump(), true);
  }
}

static double round_to(double v, int digits) {
  double f = std::pow(10.0, digits);
  return std::round(v * f) / f;
}

void MqttPublisher::publish() {
  if (!connected_) return;
  if (need_discovery_.exchange(false)) {
    announced_.clear();
    publish_system_discovery_();
  }
  auto now = Clock::now();
  int online = 0;
  for (const auto &p : monitor_.panels()) {
    if (now - p.updated >= Monitor::ONLINE_WINDOW) continue;
    online++;
    if (p.barcode.empty()) continue;  // not named by a Frame 27 yet
    // Re-announce when the name changes (a CCA sync labelled it).
    std::string key = p.barcode + "|" + panel_name(p);
    if (announced_.insert(key).second) publish_discovery_(p);
    const auto &r = p.reading;
    json s = {{"power", round_to(r.power_out, 1)},
              {"voltage_in", round_to(r.voltage_in, 2)},
              {"voltage_out", round_to(r.voltage_out, 2)},
              {"current_in", round_to(r.current_in, 3)},
              {"current_out", round_to(r.current_out, 3)},
              {"temperature", round_to(r.temperature, 1)},
              {"duty_cycle", static_cast<int>(std::lround(r.duty_cycle * 100.0 / 255.0))},
              {"efficiency", round_to(r.efficiency, 1)},
              {"rssi", r.rssi}};
    pub_(base_() + "/panel/" + p.barcode + "/state", s.dump(), false);
  }
  json sys = {{"power", round_to(monitor_.total_power(), 1)},
              {"energy", round_to(monitor_.energy_kwh(), 3)},
              {"panels_online", online}};
  pub_(base_() + "/system/state", sys.dump(), false);
}

}  // namespace tigod
