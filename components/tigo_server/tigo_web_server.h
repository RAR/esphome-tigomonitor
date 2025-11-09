#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/tigo_monitor/tigo_monitor.h"

#ifdef USE_ESP_IDF
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <driver/temperature_sensor.h>
#include <vector>
#include <string>
#include <ctime>

// Forward declare Logger from esphome
namespace esphome {
namespace logger {
class Logger;
}
}

#endif

namespace esphome {
namespace tigo_server {

#ifdef USE_ESP_IDF

// Forward declarations
class LogBuffer;
class PSRAMString;

// Helper function to add logs to buffer
void log_to_buffer(esp_log_level_t level, const char* tag, const char* message);

// Log entry structure
struct LogEntry {
  uint64_t timestamp;  // Unix timestamp in milliseconds
  esp_log_level_t level;
  std::string tag;
  std::string message;
};

// Ring buffer for storing log messages
class LogBuffer {
 public:
  LogBuffer(size_t max_entries = 500);
  ~LogBuffer();
  
  void add_log(esp_log_level_t level, const char* tag, const char* message);
  std::vector<LogEntry> get_logs(size_t start_index = 0);
  size_t get_current_index() const { return current_index_; }
  void clear();
  bool is_using_psram() const { return use_psram_; }
  
 private:
  LogEntry* buffer_;
  size_t max_entries_;
  size_t current_index_;
  SemaphoreHandle_t mutex_;
  bool use_psram_;
};

class TigoWebServer : public Component {
 public:
  TigoWebServer() = default;
  ~TigoWebServer();
  
  void set_tigo_monitor(tigo_monitor::TigoMonitorComponent *parent) { parent_ = parent; }
  
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
  
  // Log buffer access
  LogBuffer* get_log_buffer() { return log_buffer_; }

 protected:
  tigo_monitor::TigoMonitorComponent *parent_{nullptr};
  httpd_handle_t server_{nullptr};
  uint16_t port_{80};
  std::string api_token_{""};
  std::string web_username_{""};
  std::string web_password_{""};
  LogBuffer* log_buffer_{nullptr};
  temperature_sensor_handle_t temp_sensor_handle_{nullptr};
  
  // HTTP handlers
  static esp_err_t dashboard_handler(httpd_req_t *req);
  static esp_err_t node_table_handler(httpd_req_t *req);
  static esp_err_t esp_status_handler(httpd_req_t *req);
  static esp_err_t yaml_config_handler(httpd_req_t *req);
  static esp_err_t favicon_handler(httpd_req_t *req);
  
  // API endpoints (JSON)
  static esp_err_t api_devices_handler(httpd_req_t *req);
  static esp_err_t api_overview_handler(httpd_req_t *req);
  static esp_err_t api_node_table_handler(httpd_req_t *req);
  static esp_err_t api_strings_handler(httpd_req_t *req);
  static esp_err_t api_inverters_handler(httpd_req_t *req);
  static esp_err_t api_esp_status_handler(httpd_req_t *req);
  static esp_err_t api_yaml_handler(httpd_req_t *req);
  static esp_err_t cca_info_handler(httpd_req_t *req);
  static esp_err_t api_cca_info_handler(httpd_req_t *req);
  static esp_err_t api_cca_refresh_handler(httpd_req_t *req);
  static esp_err_t api_node_delete_handler(httpd_req_t *req);
  static esp_err_t api_node_import_handler(httpd_req_t *req);
  static esp_err_t api_restart_handler(httpd_req_t *req);
  static esp_err_t api_reset_peak_power_handler(httpd_req_t *req);
  static esp_err_t api_reset_node_table_handler(httpd_req_t *req);
  static esp_err_t api_health_handler(httpd_req_t *req);
  static esp_err_t api_logs_handler(httpd_req_t *req);
  static esp_err_t api_logs_status_handler(httpd_req_t *req);
  static esp_err_t api_logs_clear_handler(httpd_req_t *req);
  static esp_err_t api_logs_stream_handler(httpd_req_t *req);
  
  // Helper functions
  bool check_api_auth(httpd_req_t *req);
  bool check_web_auth(httpd_req_t *req);
  tigo_monitor::TigoMonitorComponent *get_parent_from_req(httpd_req_t *req);
  void get_dashboard_html(PSRAMString& html);
  void get_node_table_html(PSRAMString& html);
  void get_esp_status_html(PSRAMString& html);
  void get_yaml_config_html(PSRAMString& html);
  void get_cca_info_html(PSRAMString& html);
  
  // JSON builders
  std::string build_devices_json();
  std::string build_overview_json();
  std::string build_node_table_json();
  std::string build_strings_json();
  std::string build_inverters_json();
  std::string build_esp_status_json();
  std::string build_yaml_json();
  std::string build_cca_info_json();
};

#endif  // USE_ESP_IDF

}  // namespace tigo_server
}  // namespace esphome
