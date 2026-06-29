#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/tigo_monitor/tigo_monitor.h"

// Optional CCA-over-BLE client. Defined by tigo_server/__init__.py only when a
// ble_client_id is configured (cca_source: ble/auto), so plain HTTP builds pull in
// none of the ble_client stack. When defined, TigoWebServer *is* the BLEClientNode
// and talks the CCA's mobile_api over Bluetooth.
#ifdef USE_TIGO_CCA_BLE
#include <mutex>
#include <atomic>
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#endif

#ifdef USE_ESP_IDF
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <driver/temperature_sensor.h>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <utility>
#include <ctime>

// Forward declare Logger and Light from esphome
namespace esphome {
namespace logger {
class Logger;
}
namespace light {
class LightState;
}
namespace sensor {
class Sensor;
}
}

#endif

namespace esphome {
namespace tigo_server {

#ifdef USE_ESP_IDF

// Forward declarations
class PSRAMString;

// CCA Info page data source (set from cca_source: in YAML).
enum class CcaSource : uint8_t { HTTP = 0, BLE = 1, AUTO = 2 };

class TigoWebServer : public Component
#ifdef USE_TIGO_CCA_BLE
                      , public ble_client::BLEClientNode
#endif
{
 public:
  TigoWebServer() = default;
  ~TigoWebServer();

  void set_tigo_monitor(tigo_monitor::TigoMonitorComponent *parent) { parent_ = parent; }
  void set_backlight(light::LightState *backlight) { backlight_ = backlight; }
  // Optional: read die temperature from an existing ESPHome internal_temperature
  // sensor instead of installing our own. The ESP32 has a single temperature
  // peripheral that can only be installed once, so when the user also runs the
  // internal_temperature platform our install loses the race and reads nothing
  // (#28). Wiring that sensor here is the conflict-free path.
  void set_internal_temperature_sensor(sensor::Sensor *s) { external_temp_sensor_ = s; }

  void set_cca_source(CcaSource source) { cca_source_ = source; }

#ifdef USE_TIGO_CCA_BLE
  // --- CCA-over-BLE client (mobile_api). Callable from YAML lambdas/buttons. ---
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void cca_ble_connect();      // open the ble_client link
  void cca_ble_disconnect();   // close it (frees the CCA for the phone app)
  void cca_device_info();      // unauthenticated; refreshes the CCA Info cache
  void cca_ble_refresh_once(); // connect → DEVICE_INFO → auto-disconnect (dashboard refresh)
  void cca_device_ping();      // unauthenticated connectivity check
  // Protected commands (auto fresh-session sid): DISCOVERY_STATUS / NETWORK_INFO.
  void cca_send_protected(const std::string &command, const std::string &extra_params = "");
  bool cca_ble_ready() const { return ble_ready_; }
  // Topology discovery (config action). START_DISCOVERY kicks a recoverable CCA
  // rescan; DISCOVERY_STATUS polls progress. Both protected (auto fresh-session sid),
  // each a self-contained one-shot connect → … → auto-disconnect like the refresh.
  void cca_start_discovery();        // connect → sid → START_DISCOVERY → disconnect
  void cca_poll_discovery_status();  // connect → sid → DISCOVERY_STATUS → cache → disconnect
#endif

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }
  
  void set_port(uint16_t port) { port_ = port; }
  uint16_t get_port() const { return port_; }
  
  void set_api_token(const std::string &token) { api_token_ = token; }
  const std::string &get_api_token() const { return api_token_; }
  
  void set_web_username(const std::string &username) { web_username_ = username; }
  const std::string &get_web_username() const { return web_username_; }
  
  void set_web_password(const std::string &password) { web_password_ = password; }
  const std::string &get_web_password() const { return web_password_; }

 protected:
  tigo_monitor::TigoMonitorComponent *parent_{nullptr};
  light::LightState *backlight_{nullptr};
  httpd_handle_t server_{nullptr};
  uint16_t port_{80};
  std::string api_token_{""};
  std::string web_username_{""};
  std::string web_password_{""};
  temperature_sensor_handle_t temp_sensor_handle_{nullptr};
  sensor::Sensor *external_temp_sensor_{nullptr};  // optional, wins over our own handle
  CcaSource cca_source_{CcaSource::HTTP};

#ifdef USE_TIGO_CCA_BLE
  // ---- CCA-over-BLE client state (mobile_api over GATT) ----
  // GATT handles (fallback values from a BLE scan of the CCA)
  uint16_t ble_char_notify_{0x1A42};
  uint16_t ble_char_write_{0x1A44};
  bool ble_connected_{false};
  bool ble_ready_{false};  // true after the CCCD write succeeds

  std::vector<uint8_t> ble_response_buffer_;
  uint8_t ble_request_num_{1};
  uint32_t ble_last_command_time_{0};
  bool ble_awaiting_response_{false};
  std::string ble_session_key_;             // SHA512((uts + 159260)) hex, 128 chars
  std::string ble_pending_protected_cmd_;   // queued while fetching a fresh DEVICE_INFO for sid
  std::string ble_pending_protected_params_;// extra URI-encoded params for the protected cmd
  std::string ble_last_command_;            // command the in-flight response belongs to
  std::vector<std::string> ble_command_queue_;
  uint32_t ble_deferred_time_{0};
  std::string ble_deferred_command_;
  std::string ble_deferred_params_;
  bool ble_auto_disconnect_{false};  // cca_ble_refresh_once(): drop the link after the pull
  uint32_t ble_disconnect_at_{0};    // millis() deadline for the deferred disconnect (0 = none)
  // Set from the web-server task; consumed on the main loop so all BLE stack calls
  // (connect/queue) happen on the main loop, not the httpd task.
  std::atomic<bool> ble_refresh_requested_{false};
  std::atomic<bool> ble_discovery_start_requested_{false};  // POST /api/cca/discovery/start
  std::atomic<bool> ble_discovery_poll_requested_{false};   // POST /api/cca/discovery/poll
  // Network commands carry string args (cmd + pre-encoded params) the httpd task can't
  // pass through an atomic flag, so they ride a small mutex-guarded queue to the loop.
  std::mutex ble_net_req_mutex_;
  std::vector<std::pair<std::string, std::string>> ble_net_requests_;

  // Cached DEVICE_INFO + NETWORK_INFO JSON for the CCA Info page (guarded by cca_info_mutex_)
  std::string cca_info_json_;
  std::string cca_network_json_;
  std::string cca_discovery_json_;  // last DISCOVERY_STATUS payload (scan progress)
  // Per-command cache for network reads/writes (WIFI_STATUS/SCAN, CELLULAR_INFO, etc.)
  std::map<std::string, std::string> cca_net_results_;  // command -> raw JSON reply
  std::map<std::string, uint32_t> cca_net_times_;       // command -> millis() of reply
  uint32_t cca_info_time_{0};       // millis() of last cache update (0 = never)
  uint32_t cca_discovery_time_{0};  // millis() of last discovery poll (0 = never)
  std::mutex cca_info_mutex_;

  // BLE lifecycle, driven from setup()/loop()
  void ble_setup_();
  void ble_loop_();
  void ble_handle_connect_(esp_ble_gattc_cb_param_t *param);
  void ble_handle_disconnect_();
  void ble_handle_service_discovery_();
  void ble_handle_notification_(esp_ble_gattc_cb_param_t *param);
  void ble_discover_characteristics_(uint16_t svc_start, uint16_t svc_end);
  void ble_setup_notifications_(uint16_t svc_start, uint16_t svc_end);
  void ble_setup_notifications_fallback_();
  void ble_write_cccd_(uint16_t cccd_handle);
  void ble_queue_command_(const std::string &cmd);
  void ble_write_command_(const std::string &cmd, const std::string &params = "");
  void ble_send_payload_(const std::vector<uint8_t> &payload);
  void ble_generate_session_key_(uint32_t uts);
  void ble_process_response_(const std::string &data);
  void ble_arm_auto_disconnect_();
  void ble_store_cca_info_(const std::string &raw_device_info);
  void ble_store_network_(const std::string &raw_network_info);
  bool ble_has_cca_info_();
  std::string ble_get_cca_info_json_();
  std::string ble_get_network_json_();
  void ble_store_discovery_(const std::string &raw_discovery_status);
  std::string ble_get_discovery_json_();
  uint32_t ble_get_discovery_seconds_ago_();
  // Network config commands (mobile_api). cca_run_network_command_ runs on the main
  // loop: connect → sid → command(+params) → cache by name → disconnect.
  void cca_run_network_command_(const std::string &command, const std::string &params);
  void ble_enqueue_net_request_(const std::string &command, const std::string &params);
  void ble_store_net_result_(const std::string &command, const std::string &json);
  std::string ble_get_net_result_(const std::string &command, uint32_t &age_s);
  static std::string ble_uri_encode_(const std::string &value);  // matches the app's CK()
  uint32_t ble_get_cca_info_seconds_ago_();
  std::string ble_address_();
#endif

  // HTTP handlers
  static esp_err_t dashboard_handler(httpd_req_t *req);
  static esp_err_t node_table_handler(httpd_req_t *req);
  static esp_err_t esp_status_handler(httpd_req_t *req);
  static esp_err_t yaml_config_handler(httpd_req_t *req);
  static esp_err_t history_handler(httpd_req_t *req);
  static esp_err_t app_handler(httpd_req_t *req);
  static esp_err_t favicon_handler(httpd_req_t *req);
  
  // API endpoints (JSON)
  static esp_err_t api_devices_handler(httpd_req_t *req);
  static esp_err_t api_overview_handler(httpd_req_t *req);
  static esp_err_t api_node_table_handler(httpd_req_t *req);
  static esp_err_t api_strings_handler(httpd_req_t *req);
  static esp_err_t api_energy_history_handler(httpd_req_t *req);
  static esp_err_t api_inverters_handler(httpd_req_t *req);
  static esp_err_t api_inverters_rename_handler(httpd_req_t *req);
  static esp_err_t api_strings_rename_handler(httpd_req_t *req);
  static esp_err_t api_strings_rating_handler(httpd_req_t *req);
  static esp_err_t api_esp_status_handler(httpd_req_t *req);
  static esp_err_t api_yaml_handler(httpd_req_t *req);
  static esp_err_t cca_info_handler(httpd_req_t *req);
  static esp_err_t api_cca_info_handler(httpd_req_t *req);
  static esp_err_t api_cca_refresh_handler(httpd_req_t *req);
  static esp_err_t api_cca_discovery_handler(httpd_req_t *req);        // GET cached scan status
  static esp_err_t api_cca_discovery_start_handler(httpd_req_t *req);  // POST trigger START_DISCOVERY
  static esp_err_t api_cca_discovery_poll_handler(httpd_req_t *req);   // POST trigger DISCOVERY_STATUS
  static esp_err_t api_cca_network_handler(httpd_req_t *req);          // GET cached result ?cmd=
  static esp_err_t api_cca_network_poll_handler(httpd_req_t *req);     // POST trigger read ?cmd=
  static esp_err_t api_cca_wifi_connect_handler(httpd_req_t *req);     // POST {nid,pwd}
  static esp_err_t api_cca_wifi_clear_handler(httpd_req_t *req);       // POST (destructive)
  static esp_err_t api_node_delete_handler(httpd_req_t *req);
  static esp_err_t api_node_import_handler(httpd_req_t *req);
  static esp_err_t api_restart_handler(httpd_req_t *req);
  static esp_err_t api_reset_peak_power_handler(httpd_req_t *req);
  static esp_err_t api_reset_node_table_handler(httpd_req_t *req);
  static esp_err_t api_health_handler(httpd_req_t *req);
  static esp_err_t api_backlight_handler(httpd_req_t *req);
  static esp_err_t api_github_release_handler(httpd_req_t *req);
#ifdef TIGO_TSDB_AVAILABLE
  static esp_err_t api_history_power_handler(httpd_req_t *req);
  static esp_err_t api_history_panel_handler(httpd_req_t *req);
  static esp_err_t api_panels_handler(httpd_req_t *req);
  static esp_err_t api_tsdb_stats_handler(httpd_req_t *req);
#endif
  
  // Helper functions
  bool check_api_auth(httpd_req_t *req);
  bool check_web_auth(httpd_req_t *req);
  tigo_monitor::TigoMonitorComponent *get_parent_from_req(httpd_req_t *req);
  void get_app_html(PSRAMString& html);
  
  // JSON builders - now write directly to PSRAMString to avoid internal RAM allocation
  void build_devices_json(PSRAMString& json);
  void build_overview_json(PSRAMString& json);
  void build_node_table_json(PSRAMString& json);
  void build_strings_json(PSRAMString& json);
  void build_energy_history_json(PSRAMString& json);
  void build_inverters_json(PSRAMString& json);
  void build_esp_status_json(PSRAMString& json);
  void build_yaml_json(PSRAMString& json, const std::set<std::string>& selected_sensors, const std::set<std::string>& selected_hub_sensors, const std::string& grouping);
  void build_cca_info_json(PSRAMString& json);
};

#endif  // USE_ESP_IDF

}  // namespace tigo_server
}  // namespace esphome
