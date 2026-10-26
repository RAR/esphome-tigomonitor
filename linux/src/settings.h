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
  // Empty until someone picks one, so installing on a host with other serial
  // devices (a Zigbee stick, say) never touches them. "auto" picks the first
  // USB serial adapter, then the Pi's own UART; the SD card image starts there.
  std::string serial_port;
  int baud_rate = 38400;
  // "listen" (the default): only read the bus, beside a CCA. "master": run
  // the bus in the CCA's place, with the CCA unplugged (bus_master.h).
  std::string bus_role = "listen";
  bool master_bench = false;    // also send the reads a bench TAP answers
  bool master_startup = false;  // re-address and bring up a TAP found at the factory address
  int gateway_id = 0x1201;    // the TAP's RS485 id, as the CCA addresses it
  float power_calibration = 1.0f;
  // Plenty of RAM here, so the node table is sized for any real site rather
  // than to the panel count, as number_of_devices has to be on the ESP32.
  int max_nodes = 512;
  int publish_interval_s = 30;
  // 0: port 80, or 8080 when something else already has 80.
  int web_port = 0;
  // A login for the web UI and API, as web_username/web_password on the
  // ESP32. An empty password leaves it open to the LAN, the default there too.
  std::string web_username = "admin";
  std::string web_password;
  // History snapshots. The ESP32 takes one every 30 minutes to spare its
  // flash; an SD card can take one a minute and keep two years of them.
  int history_interval_min = 1;
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
