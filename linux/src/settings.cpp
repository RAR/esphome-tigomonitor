#include "settings.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tigod {

using nlohmann::json;

template<class T> static void get(const json &j, const char *key, T &out) {
  auto it = j.find(key);
  if (it != j.end() && !it->is_null()) {
    try {
      out = it->get<T>();
    } catch (const json::exception &) {
      LOGW("settings: ignoring %s, wrong type", key);
    }
  }
}

Settings load_settings(const std::string &path) {
  Settings s;
  std::ifstream in(path);
  if (!in) return s;  // first run
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    LOGW("settings: %s is not valid JSON, using defaults", path.c_str());
    return s;
  }
  get(j, "serial_port", s.serial_port);
  get(j, "baud_rate", s.baud_rate);
  get(j, "bus_role", s.bus_role);
  get(j, "standby_takeover_s", s.standby_takeover_s);
  get(j, "master_bench", s.master_bench);
  get(j, "master_startup", s.master_startup);
  get(j, "master_housekeeping", s.master_housekeeping);
  get(j, "master_queries", s.master_queries);
  get(j, "master_optimizer_writes", s.master_optimizer_writes);
  get(j, "gateway_id", s.gateway_id);
  get(j, "web_port", s.web_port);
  get(j, "web_username", s.web_username);
  get(j, "web_password", s.web_password);
  get(j, "power_calibration", s.power_calibration);
  get(j, "max_nodes", s.max_nodes);
  get(j, "publish_interval_s", s.publish_interval_s);
  get(j, "history_interval_min", s.history_interval_min);
  get(j, "history_days", s.history_days);
  get(j, "night_mode_timeout_min", s.night_mode_timeout_min);
  get(j, "stale_timeout_min", s.stale_timeout_min);
  get(j, "reset_at_midnight", s.reset_at_midnight);
  get(j, "sync_cca_on_startup", s.sync_cca_on_startup);
  get(j, "cca_ip", s.cca_ip);
  get(j, "device_id", s.device_id);
  if (auto m = j.find("mqtt"); m != j.end() && m->is_object()) {
    get(*m, "host", s.mqtt.host);
    get(*m, "port", s.mqtt.port);
    get(*m, "username", s.mqtt.username);
    get(*m, "password", s.mqtt.password);
    get(*m, "discovery", s.mqtt.discovery);
    get(*m, "discovery_prefix", s.mqtt.discovery_prefix);
    get(*m, "base_topic", s.mqtt.base_topic);
  }
  if (s.publish_interval_s < 5) s.publish_interval_s = 5;
  if (s.web_port < 0 || s.web_port > 65535) s.web_port = 0;
  if (s.web_username.empty()) s.web_username = "admin";
  if (s.bus_role != "master" && s.bus_role != "standby") s.bus_role = "listen";
  if (!standby_takeover_ok(s.standby_takeover_s)) s.standby_takeover_s = 3;
  if (!gateway_id_ok(s.gateway_id)) s.gateway_id = 0x1201;
  s.master_optimizer_writes = s.master_optimizer_writes && s.master_queries;
  if (s.history_interval_min < 1 || s.history_interval_min > 60) s.history_interval_min = 1;
  if (s.history_days < 1) s.history_days = 730;
  if (s.night_mode_timeout_min < 1 || s.night_mode_timeout_min > 1440) s.night_mode_timeout_min = 60;
  if (s.stale_timeout_min < 0 || s.stale_timeout_min > 1440) s.stale_timeout_min = 10;
  return s;
}

bool save_settings(const std::string &path, const Settings &s) {
  json j = {
      {"serial_port", s.serial_port},
      {"baud_rate", s.baud_rate},
      {"bus_role", s.bus_role},
      {"standby_takeover_s", s.standby_takeover_s},
      {"master_bench", s.master_bench},
      {"master_startup", s.master_startup},
      {"master_housekeeping", s.master_housekeeping},
      {"master_queries", s.master_queries},
      {"master_optimizer_writes", s.master_optimizer_writes},
      {"gateway_id", s.gateway_id},
      {"web_port", s.web_port},
      {"web_username", s.web_username},
      {"web_password", s.web_password},
      {"power_calibration", s.power_calibration},
      {"max_nodes", s.max_nodes},
      {"publish_interval_s", s.publish_interval_s},
      {"history_interval_min", s.history_interval_min},
      {"history_days", s.history_days},
      {"night_mode_timeout_min", s.night_mode_timeout_min},
      {"stale_timeout_min", s.stale_timeout_min},
      {"reset_at_midnight", s.reset_at_midnight},
      {"sync_cca_on_startup", s.sync_cca_on_startup},
      {"cca_ip", s.cca_ip},
      {"device_id", s.device_id},
      {"mqtt",
       {{"host", s.mqtt.host},
        {"port", s.mqtt.port},
        {"username", s.mqtt.username},
        {"password", s.mqtt.password},
        {"discovery", s.mqtt.discovery},
        {"discovery_prefix", s.mqtt.discovery_prefix},
        {"base_topic", s.mqtt.base_topic}}},
  };
  // The MQTT and web passwords are in here: owner-only.
  return write_file_atomic(path, j.dump(2) + "\n", 0600);
}

}  // namespace tigod
