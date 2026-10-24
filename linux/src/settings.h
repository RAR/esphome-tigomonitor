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
  // "auto" picks the first USB serial adapter, then the Pi's own UART.
  std::string serial_port = "auto";
  int baud_rate = 38400;
  float power_calibration = 1.0f;
  // Plenty of RAM here, so the node table is sized for any real site rather
  // than to the panel count, as number_of_devices has to be on the ESP32.
  int max_nodes = 512;
  int publish_interval_s = 30;
  // History snapshots. The ESP32 takes one every 30 minutes to spare its
  // flash; an SD card can take one every 5 and keep two years of them.
  int history_interval_min = 5;
  int history_days = 730;
  // The firmware's runtime knobs (/api/config, Tools > Device Configuration),
  // with its defaults.
  int night_mode_timeout_min = 60;
  int stale_timeout_min = 10;  // 0: never
  bool reset_at_midnight = false;
  bool sync_cca_on_startup = true;
  std::string cca_ip;  // CCA firmware before 4.0.4 only
  std::string device_id;  // empty: the hostname
  MqttSettings mqtt;
};

Settings load_settings(const std::string &path);
bool save_settings(const std::string &path, const Settings &s);

}  // namespace tigod
