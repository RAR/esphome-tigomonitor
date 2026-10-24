#pragma once

// User settings, kept as JSON in <state_dir>/settings.json. The web UI edits
// them; anything missing from the file takes the default below.

#include <string>

namespace tigod {

struct MqttSettings {
  std::string host;  // empty: MQTT off
  int port = 1883;
  std::string username;
  std::string password;
  std::string discovery_prefix = "homeassistant";
  std::string base_topic = "tigomonitor";
};

struct Settings {
  std::string serial_port = "/dev/ttyUSB0";
  int baud_rate = 38400;
  float power_calibration = 1.0f;
  // Plenty of RAM here, so the node table is sized for any real site rather
  // than to the panel count, as number_of_devices has to be on the ESP32.
  int max_nodes = 512;
  int publish_interval_s = 30;
  std::string device_id;  // empty: the hostname
  MqttSettings mqtt;
};

Settings load_settings(const std::string &path);
bool save_settings(const std::string &path, const Settings &s);

}  // namespace tigod
