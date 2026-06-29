#include "tigo_web_server.h"
#include "web_assets.h"  // Auto-generated from components/tigo_server/web/*.html

#ifdef USE_ESP_IDF

#include "esphome/core/application.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"
#include "esphome/core/time.h"
#include "esphome/components/network/util.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_call.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <cstring>
#include <cmath>
#include <sys/time.h>
#include <mbedtls/base64.h>
#include <driver/temperature_sensor.h>
#include "cJSON.h"

namespace esphome {
namespace tigo_server {

static const char *const TAG = "tigo_web_server";

// Helper class to manage PSRAM-allocated strings for large HTML content
class PSRAMString {
 public:
  PSRAMString() : data_(nullptr), size_(0), capacity_(0) {}
  
  ~PSRAMString() {
    if (data_) {
      heap_caps_free(data_);
    }
  }
  
  // Append string, allocating from PSRAM if available
  void append(const char* str) {
    size_t len = strlen(str);
    reserve(size_ + len + 1);
    if (data_) {
      memcpy(data_ + size_, str, len);
      size_ += len;
      data_[size_] = '\0';
    }
  }
  
  void append(const std::string& str) {
    reserve(size_ + str.length() + 1);
    if (data_) {
      memcpy(data_ + size_, str.c_str(), str.length());
      size_ += str.length();
      data_[size_] = '\0';
    }
  }
  
  const char* c_str() const { return data_ ? data_ : ""; }
  size_t length() const { return size_; }
  
 private:
  void reserve(size_t new_capacity) {
    if (new_capacity <= capacity_) return;
    
    size_t alloc_size = new_capacity + 1024;  // Add some headroom
    char* new_data = nullptr;
    
    // Try PSRAM first if available
    size_t psram_available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_available >= alloc_size) {
      new_data = static_cast<char*>(heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM));
      if (new_data) {
        ESP_LOGV(TAG, "Allocated %zu bytes from PSRAM (available: %zu)", alloc_size, psram_available);
      }
    }
    
    // Fallback to regular heap
    if (!new_data) {
      new_data = static_cast<char*>(heap_caps_malloc(alloc_size, MALLOC_CAP_DEFAULT));
      if (!new_data) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for buffer", alloc_size);
        return;
      }
      ESP_LOGV(TAG, "Allocated %zu bytes from regular heap", alloc_size);
    }
    
    if (data_) {
      memcpy(new_data, data_, size_);
      heap_caps_free(data_);
    }
    data_ = new_data;
    capacity_ = alloc_size;
  }
  
  char* data_;
  size_t size_;
  size_t capacity_;
};

void TigoWebServer::setup() {
  ESP_LOGI(TAG, "Starting Tigo Web Server on port %d...", port_);

#ifdef USE_TIGO_CCA_BLE
  this->ble_setup_();
#endif

  // Check PSRAM availability
  size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (total_psram > 0) {
    ESP_LOGI(TAG, "PSRAM detected: %zu bytes total, %zu bytes free", total_psram, free_psram);
  } else {
    ESP_LOGW(TAG, "PSRAM not available - log streaming and large buffers disabled");
    
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)
    // On ESP32-S3 and ESP32-P4, check if PSRAM is physically present but not configured
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "**************************************************************");
    ESP_LOGW(TAG, "* If your board has PSRAM (e.g., AtomS3R, P4-EVBoard),      *");
    ESP_LOGW(TAG, "* you MUST enable it in your YAML configuration!            *");
    ESP_LOGW(TAG, "*                                                            *");
#ifdef CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGW(TAG, "* For ESP32-S3, add this to your YAML:                      *");
    ESP_LOGW(TAG, "*   esphome:                                                 *");
    ESP_LOGW(TAG, "*     platformio_options:                                    *");
    ESP_LOGW(TAG, "*       board_build.arduino.memory_type: qio_opi            *");
    ESP_LOGW(TAG, "*       board_build.f_flash: 80000000L                      *");
    ESP_LOGW(TAG, "*       board_build.flash_mode: qio                         *");
    ESP_LOGW(TAG, "*       build_flags:                                         *");
    ESP_LOGW(TAG, "*         - -DBOARD_HAS_PSRAM                               *");
#endif
#ifdef CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGW(TAG, "* For ESP32-P4, add this to your YAML:                      *");
    ESP_LOGW(TAG, "*   psram:                                                   *");
    ESP_LOGW(TAG, "*     mode: hex                                              *");
    ESP_LOGW(TAG, "*     speed: 200MHz                                          *");
#endif
    ESP_LOGW(TAG, "*                                                            *");
    ESP_LOGW(TAG, "* PSRAM is REQUIRED for 15+ devices!                        *");
    ESP_LOGW(TAG, "* See boards/ folder for complete examples.                 *");
    ESP_LOGW(TAG, "**************************************************************");
    ESP_LOGW(TAG, "");
#endif
  }
  
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port_;
  config.ctrl_port = port_ + 1;
  // Must be >= the number of httpd_register_uri_handler() calls below (40 on a TSDB
  // build: 29 base + 4 TSDB + 3 CCA-discovery + 4 CCA-network routes). Handlers past this cap
  // silently fail to register and 404 — TSDB stats registers last, so it's the canary.
  // Keep generous headroom so adding a route doesn't quietly drop the tail again.
  config.max_uri_handlers = 48;
  config.stack_size = 8192;
  config.lru_purge_enable = true;  // Enable LRU purging of connections
  config.max_open_sockets = 4;     // Limit concurrent connections to reduce memory
  config.keep_alive_enable = false; // Disable keep-alive to free connections faster
  
  if (httpd_start(&server_, &config) == ESP_OK) {
    ESP_LOGI(TAG, "Web server started successfully on port %d", port_);
    
    // Register HTML page handlers
    httpd_uri_t dashboard_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = dashboard_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &dashboard_uri);
    
    httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico",
      .method = HTTP_GET,
      .handler = favicon_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &favicon_uri);
    
    httpd_uri_t node_table_uri = {
      .uri = "/nodes",
      .method = HTTP_GET,
      .handler = node_table_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &node_table_uri);
    
    httpd_uri_t esp_status_uri = {
      .uri = "/status",
      .method = HTTP_GET,
      .handler = esp_status_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &esp_status_uri);
    
    httpd_uri_t yaml_config_uri = {
      .uri = "/yaml",
      .method = HTTP_GET,
      .handler = yaml_config_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &yaml_config_uri);
    
    httpd_uri_t history_uri = {
      .uri = "/history",
      .method = HTTP_GET,
      .handler = history_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &history_uri);

    httpd_uri_t app_uri = {
      .uri = "/app",
      .method = HTTP_GET,
      .handler = app_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &app_uri);

    httpd_uri_t cca_info_uri = {
      .uri = "/cca",
      .method = HTTP_GET,
      .handler = cca_info_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &cca_info_uri);
    
    // Register API endpoints
    httpd_uri_t api_devices_uri = {
      .uri = "/api/devices",
      .method = HTTP_GET,
      .handler = api_devices_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_devices_uri);
    
    httpd_uri_t api_overview_uri = {
      .uri = "/api/overview",
      .method = HTTP_GET,
      .handler = api_overview_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_overview_uri);
    
    httpd_uri_t api_node_table_uri = {
      .uri = "/api/nodes",
      .method = HTTP_GET,
      .handler = api_node_table_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_node_table_uri);
    
    httpd_uri_t api_strings_uri = {
      .uri = "/api/strings",
      .method = HTTP_GET,
      .handler = api_strings_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_strings_uri);
    
    httpd_uri_t api_energy_history_uri = {
      .uri = "/api/energy/history",
      .method = HTTP_GET,
      .handler = api_energy_history_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_energy_history_uri);
    
    httpd_uri_t api_inverters_uri = {
      .uri = "/api/inverters",
      .method = HTTP_GET,
      .handler = api_inverters_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_inverters_uri);

    httpd_uri_t api_inverters_rename_uri = {
      .uri = "/api/inverters/rename",
      .method = HTTP_POST,
      .handler = api_inverters_rename_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_inverters_rename_uri);

    httpd_uri_t api_strings_rename_uri = {
      .uri = "/api/strings/rename",
      .method = HTTP_POST,
      .handler = api_strings_rename_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_strings_rename_uri);

    httpd_uri_t api_strings_rating_uri = {
      .uri = "/api/strings/rating",
      .method = HTTP_POST,
      .handler = api_strings_rating_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_strings_rating_uri);
    
    httpd_uri_t api_esp_status_uri = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = api_esp_status_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_esp_status_uri);
    
    httpd_uri_t api_yaml_uri = {
      .uri = "/api/yaml",
      .method = HTTP_GET,
      .handler = api_yaml_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_yaml_uri);
    
    httpd_uri_t api_cca_info_uri = {
      .uri = "/api/cca",
      .method = HTTP_GET,
      .handler = api_cca_info_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_info_uri);
    
    httpd_uri_t api_cca_refresh_uri = {
      .uri = "/api/cca/refresh",
      .method = HTTP_GET,
      .handler = api_cca_refresh_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_refresh_uri);

    httpd_uri_t api_cca_discovery_uri = {
      .uri = "/api/cca/discovery",
      .method = HTTP_GET,
      .handler = api_cca_discovery_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_discovery_uri);

    httpd_uri_t api_cca_discovery_start_uri = {
      .uri = "/api/cca/discovery/start",
      .method = HTTP_POST,
      .handler = api_cca_discovery_start_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_discovery_start_uri);

    httpd_uri_t api_cca_discovery_poll_uri = {
      .uri = "/api/cca/discovery/poll",
      .method = HTTP_POST,
      .handler = api_cca_discovery_poll_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_discovery_poll_uri);

    httpd_uri_t api_cca_network_uri = {
      .uri = "/api/cca/network",
      .method = HTTP_GET,
      .handler = api_cca_network_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_network_uri);

    httpd_uri_t api_cca_network_poll_uri = {
      .uri = "/api/cca/network/poll",
      .method = HTTP_POST,
      .handler = api_cca_network_poll_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_network_poll_uri);

    httpd_uri_t api_cca_wifi_connect_uri = {
      .uri = "/api/cca/network/wifi-connect",
      .method = HTTP_POST,
      .handler = api_cca_wifi_connect_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_wifi_connect_uri);

    httpd_uri_t api_cca_wifi_clear_uri = {
      .uri = "/api/cca/network/wifi-clear",
      .method = HTTP_POST,
      .handler = api_cca_wifi_clear_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_cca_wifi_clear_uri);

    httpd_uri_t api_node_delete_uri = {
      .uri = "/api/nodes/delete",
      .method = HTTP_POST,
      .handler = api_node_delete_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_node_delete_uri);
    
    httpd_uri_t api_node_import_uri = {
      .uri = "/api/nodes/import",
      .method = HTTP_POST,
      .handler = api_node_import_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_node_import_uri);
    
    httpd_uri_t api_restart_uri = {
      .uri = "/api/restart",
      .method = HTTP_POST,
      .handler = api_restart_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_restart_uri);
    
    httpd_uri_t api_reset_peak_power_uri = {
      .uri = "/api/reset_peak_power",
      .method = HTTP_POST,
      .handler = api_reset_peak_power_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_reset_peak_power_uri);
    
    httpd_uri_t api_reset_node_table_uri = {
      .uri = "/api/reset_node_table",
      .method = HTTP_POST,
      .handler = api_reset_node_table_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_reset_node_table_uri);
    
    httpd_uri_t api_health_uri = {
      .uri = "/api/health",
      .method = HTTP_GET,
      .handler = api_health_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_health_uri);
    
    httpd_uri_t api_backlight_uri = {
      .uri = "/api/backlight",
      .method = HTTP_POST,
      .handler = api_backlight_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_backlight_uri);
    
    httpd_uri_t api_github_release_uri = {
      .uri = "/api/github/release",
      .method = HTTP_GET,
      .handler = api_github_release_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_github_release_uri);

#ifdef TIGO_TSDB_AVAILABLE
    httpd_uri_t api_history_power_uri = {
      .uri = "/api/history/power",
      .method = HTTP_GET,
      .handler = api_history_power_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_history_power_uri);

    httpd_uri_t api_history_panel_uri = {
      .uri = "/api/history/panel",
      .method = HTTP_GET,
      .handler = api_history_panel_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_history_panel_uri);

    httpd_uri_t api_panels_uri = {
      .uri = "/api/panels",
      .method = HTTP_GET,
      .handler = api_panels_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_panels_uri);

    httpd_uri_t api_tsdb_stats_uri = {
      .uri = "/api/tsdb/stats",
      .method = HTTP_GET,
      .handler = api_tsdb_stats_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_tsdb_stats_uri);
#endif

    // Log web authentication status
    if (!web_username_.empty() && !web_password_.empty()) {
      ESP_LOGI(TAG, "HTTP Basic Authentication configured for web pages (user: %s)", web_username_.c_str());
    } else {
      ESP_LOGI(TAG, "Web authentication not configured - pages remain open");
    }
    
    // Initialize the internal temperature sensor once at startup — but only if
    // the user hasn't wired an existing internal_temperature sensor. The ESP32
    // has a single temperature peripheral that installs exactly once, so if
    // another component (e.g. the internal_temperature platform) already owns
    // it, our install would lose the race and read nothing (#28). When a sensor
    // is wired we read its published state instead and skip the install.
    if (external_temp_sensor_ != nullptr) {
      ESP_LOGI(TAG, "Using configured internal_temperature sensor for die temp");
    } else {
    // Use a wide measurement range so a hot S3 die (can exceed 80 °C under
    // load) still reads in-range rather than erroring out and reporting nothing.
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 110);
    esp_err_t err = temperature_sensor_install(&temp_sensor_config, &temp_sensor_handle_);
    if (err == ESP_OK) {
      err = temperature_sensor_enable(temp_sensor_handle_);
      if (err == ESP_OK) {
        ESP_LOGI(TAG, "Temperature sensor initialized successfully");
      } else {
        ESP_LOGW(TAG, "Failed to enable temperature sensor: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(temp_sensor_handle_);
        temp_sensor_handle_ = nullptr;
      }
    } else {
      ESP_LOGW(TAG, "Failed to install temperature sensor: %s — if you also use the "
                    "internal_temperature platform, wire it via internal_temperature_id", esp_err_to_name(err));
      temp_sensor_handle_ = nullptr;
    }
    }  // end else (no external sensor wired)

    ESP_LOGI(TAG, "All routes registered");
  } else {
    ESP_LOGE(TAG, "Failed to start web server");
  }
}

tigo_monitor::TigoMonitorComponent *TigoWebServer::get_parent_from_req(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  return server->parent_;
}

bool TigoWebServer::check_api_auth(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // If no token is configured, allow all requests (backward compatible)
  if (server->api_token_.empty()) {
    return true;
  }
  
  // Get Authorization header
  size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (buf_len == 0) {
    ESP_LOGW(TAG, "API request without Authorization header");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Authorization required\"}", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Read Authorization header
  char *auth_header = static_cast<char*>(malloc(buf_len + 1));
  if (!auth_header) {
    httpd_resp_send_500(req);
    return false;
  }
  
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, buf_len + 1) != ESP_OK) {
    free(auth_header);
    httpd_resp_send_500(req);
    return false;
  }
  
  // Check for "Bearer <token>" format
  std::string auth_str(auth_header);
  free(auth_header);
  
  if (auth_str.length() < 7 || auth_str.substr(0, 7) != "Bearer ") {
    ESP_LOGW(TAG, "Invalid Authorization header format (expected 'Bearer <token>')");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Invalid authorization format\"}", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Extract token and compare
  std::string provided_token = auth_str.substr(7);
  if (provided_token != server->api_token_) {
    ESP_LOGW(TAG, "Invalid API token provided");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"Invalid token\"}", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Token valid
  return true;
}

bool TigoWebServer::check_web_auth(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // If no credentials configured, allow all requests (backward compatible)
  if (server->web_username_.empty() || server->web_password_.empty()) {
    return true;
  }
  
  // Get Authorization header
  size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (buf_len == 0) {
    // Send 401 with WWW-Authenticate header to trigger browser auth prompt
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tigo Monitor\"");
    httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Read Authorization header
  char *auth_header = static_cast<char*>(malloc(buf_len + 1));
  if (!auth_header) {
    httpd_resp_send_500(req);
    return false;
  }
  
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, buf_len + 1) != ESP_OK) {
    free(auth_header);
    httpd_resp_send_500(req);
    return false;
  }
  
  // Check for "Basic <credentials>" format
  std::string auth_str(auth_header);
  free(auth_header);
  
  if (auth_str.length() < 6 || auth_str.substr(0, 6) != "Basic ") {
    // Send 401 to trigger browser auth prompt
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tigo Monitor\"");
    httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Extract and decode base64 credentials
  std::string encoded_creds = auth_str.substr(6);
  
  // Decode base64
  size_t decoded_len = 0;
  unsigned char decoded[256];
  int ret = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                                   (const unsigned char*)encoded_creds.c_str(),
                                   encoded_creds.length());
  
  if (ret != 0 || decoded_len == 0) {
    ESP_LOGW(TAG, "Failed to decode Basic Auth credentials");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tigo Monitor\"");
    httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  std::string credentials((char*)decoded, decoded_len);
  
  // Split username:password
  size_t colon_pos = credentials.find(':');
  if (colon_pos == std::string::npos) {
    ESP_LOGW(TAG, "Invalid Basic Auth format");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tigo Monitor\"");
    httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  std::string username = credentials.substr(0, colon_pos);
  std::string password = credentials.substr(colon_pos + 1);
  
  // Compare credentials
  if (username != server->web_username_ || password != server->web_password_) {
    ESP_LOGW(TAG, "Invalid web credentials provided for user: %s", username.c_str());
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tigo Monitor\"");
    httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
  }
  
  // Credentials valid
  return true;
}

// ===== HTML Page Handlers =====

esp_err_t TigoWebServer::favicon_handler(httpd_req_t *req) {
  // Brand mark — same artwork as docs/images/logo.svg and the SPA sidebar
  // brand-logo. Rounded gradient tile + 3x3 panel grid with a "live" cell
  // highlight + faint telemetry pulse. Kept inline as a single string so we
  // don't pay an extra HTTP round-trip + cache miss for an icon.
  const char *favicon_svg =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
    "<defs>"
      "<linearGradient id='b' x1='0' y1='0' x2='1' y2='1'>"
        "<stop offset='0%' stop-color='#4ade80'/>"
        "<stop offset='100%' stop-color='#38bdf8'/>"
      "</linearGradient>"
      "<radialGradient id='g' cx='50%' cy='40%' r='60%'>"
        "<stop offset='0%' stop-color='#fff' stop-opacity='.85'/>"
        "<stop offset='60%' stop-color='#fff' stop-opacity='.2'/>"
        "<stop offset='100%' stop-color='#fff' stop-opacity='0'/>"
      "</radialGradient>"
    "</defs>"
    "<rect width='64' height='64' rx='14' fill='url(#b)'/>"
    "<g transform='translate(10 10)' fill='#061018' fill-opacity='.18'>"
      "<rect width='13' height='13' rx='2'/>"
      "<rect x='15' width='13' height='13' rx='2'/>"
      "<rect x='30' width='13' height='13' rx='2'/>"
      "<rect y='15' width='13' height='13' rx='2'/>"
      "<rect x='15' y='15' width='13' height='13' rx='2' fill-opacity='.55'/>"
      "<rect x='30' y='15' width='13' height='13' rx='2'/>"
      "<rect y='30' width='13' height='13' rx='2'/>"
      "<rect x='15' y='30' width='13' height='13' rx='2'/>"
      "<rect x='30' y='30' width='13' height='13' rx='2'/>"
    "</g>"
    "<rect x='25' y='25' width='13' height='13' rx='2' fill='url(#g)'/>"
    "<path d='M8 50 18 50 22 44 28 52 34 47 40 49 56 49' fill='none' "
      "stroke='#061018' stroke-opacity='.55' stroke-width='2' "
      "stroke-linecap='round' stroke-linejoin='round'/>"
    "</svg>";

  httpd_resp_set_type(req, "image/svg+xml");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  httpd_resp_send(req, favicon_svg, strlen(favicon_svg));
  return ESP_OK;
}

// Shared helper for the legacy-page → SPA redirects. Using a RELATIVE Location
// ("app#x" rather than "/app#x") matters under HA Ingress: the browser is
// at /api/hassio_ingress/<token>/<page>, so a relative target resolves to
// /api/hassio_ingress/<token>/app#x. An absolute "/app#x" would bypass the
// ingress prefix and 404 at the supervisor. Standalone access works either
// way.
static esp_err_t redirect_to_app_view(httpd_req_t *req, const char *fragment) {
  // Buffers stay valid until httpd_resp_send returns, which is fine for
  // stack-local strings — esp_http_server reads them synchronously.
  char loc[48];
  snprintf(loc, sizeof(loc), "app#%s", fragment);
  char body[128];
  snprintf(body, sizeof(body), "<a href=\"%s\">%s</a>", loc, loc);

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t TigoWebServer::dashboard_handler(httpd_req_t *req) {
  // / is now a redirect into the SPA — the legacy dashboard.html is retired.
  return redirect_to_app_view(req, "dashboard");
}

esp_err_t TigoWebServer::node_table_handler(httpd_req_t *req) {
  // /nodes (R4) — /api/nodes stays live, consumed from #view-nodes inside /app.
  return redirect_to_app_view(req, "nodes");
}

esp_err_t TigoWebServer::esp_status_handler(httpd_req_t *req) {
  // /status (R5) — /api/status stays live; consumed from #view-diagnostics
  // alongside /api/tsdb/stats.
  return redirect_to_app_view(req, "diagnostics");
}

esp_err_t TigoWebServer::history_handler(httpd_req_t *req) {
  // /history (R3) — /api/history/* endpoints stay live, consumed from
  // #view-history inside /app.
  return redirect_to_app_view(req, "history");
}

esp_err_t TigoWebServer::app_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);

  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }

  PSRAMString html;
  server->get_app_html(html);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Connection", "close");
  return httpd_resp_send(req, html.c_str(), html.length());
}

esp_err_t TigoWebServer::yaml_config_handler(httpd_req_t *req) {
  // /yaml (R6) — /api/yaml stays live, consumed from #view-tools.
  return redirect_to_app_view(req, "tools");
}

// ===== API Handlers (JSON) =====

esp_err_t TigoWebServer::api_devices_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;  // Response already sent by check_api_auth
  }
  
  PSRAMString json_buffer;
  server->build_devices_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_overview_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_overview_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_node_table_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_node_table_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_strings_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_strings_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_energy_history_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_energy_history_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_inverters_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  PSRAMString json_buffer;
  server->build_inverters_json(json_buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

// POST body shape: {"name":"<canonical>","display_name":"<friendly>"}
// "name" matches the YAML-defined inverter name; "display_name" is the
// override (empty string clears the override and falls back to canonical).
esp_err_t TigoWebServer::api_inverters_rename_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) return ESP_OK;
  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }

  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"empty body\"}");
    return ESP_OK;
  }
  buf[ret] = '\0';

  // Tiny hand-rolled extractor — same approach the rest of this file uses for
  // small POST bodies (no JSON parser dependency).
  auto extract = [](const char *body, const char *key, char *out, size_t outlen) -> bool {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(body, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t) (q2 - q1 - 1);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
  };

  char canonical[64] = {};
  char display[64] = {};
  if (!extract(buf, "name", canonical, sizeof(canonical))) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"missing 'name'\"}");
    return ESP_OK;
  }
  // display_name is optional — empty means "clear override".
  extract(buf, "display_name", display, sizeof(display));

  bool ok = server->parent_->set_inverter_display_name(canonical, display);
  if (!ok) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"unknown inverter\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

// POST body shape: {"label":"<canonical>","display_label":"<friendly>"}
// Mirror of api_inverters_rename_handler — strings have a different
// canonical-key field name (label, not name) but the rest is identical.
esp_err_t TigoWebServer::api_strings_rename_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) return ESP_OK;
  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }

  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"empty body\"}");
    return ESP_OK;
  }
  buf[ret] = '\0';

  auto extract = [](const char *body, const char *key, char *out, size_t outlen) -> bool {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(body, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t) (q2 - q1 - 1);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
  };

  char canonical[64] = {};
  char display[64] = {};
  if (!extract(buf, "label", canonical, sizeof(canonical))) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"missing 'label'\"}");
    return ESP_OK;
  }
  extract(buf, "display_label", display, sizeof(display));

  bool ok = server->parent_->set_string_display_label(canonical, display);
  if (!ok) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"unknown string\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

// POST body: {"label":"<canonical>","rating_w":<int>}
// rating_w is the per-panel nameplate watts in this string (0-65535).
// 0 clears the override.
esp_err_t TigoWebServer::api_strings_rating_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) return ESP_OK;
  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }

  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"empty body\"}");
    return ESP_OK;
  }
  buf[ret] = '\0';

  // Quoted-string extractor (same shape as the rename handlers).
  auto extract_str = [](const char *body, const char *key, char *out, size_t outlen) -> bool {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(body, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t) (q2 - q1 - 1);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
  };
  // Numeric value extractor — finds "key": then strtoul over the digits.
  auto extract_num = [](const char *body, const char *key, long *out) -> bool {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(body, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    char *end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
  };

  char canonical[64] = {};
  long rating = -1;
  if (!extract_str(buf, "label", canonical, sizeof(canonical))) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"missing 'label'\"}");
    return ESP_OK;
  }
  if (!extract_num(buf, "rating_w", &rating) || rating < 0 || rating > 65535) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"missing or invalid 'rating_w'\"}");
    return ESP_OK;
  }

  bool ok = server->parent_->set_string_panel_rating(canonical, (uint16_t) rating);
  if (!ok) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"unknown string\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":true}");
  return ESP_OK;
}

esp_err_t TigoWebServer::api_esp_status_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_esp_status_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_yaml_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  // Parse query parameters to get selected sensors
  char query[512];
  std::set<std::string> selected_sensors;
  std::set<std::string> selected_hub_sensors;
  std::string grouping = "none";  // none | panel | mppt | inverter

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char sensors_param[400];
    if (httpd_query_key_value(query, "sensors", sensors_param, sizeof(sensors_param)) == ESP_OK) {
      // Parse comma-separated sensor list
      std::string sensors_str(sensors_param);
      size_t start = 0;
      size_t end = sensors_str.find(',');

      while (end != std::string::npos) {
        selected_sensors.insert(sensors_str.substr(start, end - start));
        start = end + 1;
        end = sensors_str.find(',', start);
      }
      selected_sensors.insert(sensors_str.substr(start));
    }

    char hub_sensors_param[400];
    if (httpd_query_key_value(query, "hub_sensors", hub_sensors_param, sizeof(hub_sensors_param)) == ESP_OK) {
      // Parse comma-separated hub sensor list
      std::string hub_sensors_str(hub_sensors_param);
      size_t start = 0;
      size_t end = hub_sensors_str.find(',');

      while (end != std::string::npos) {
        selected_hub_sensors.insert(hub_sensors_str.substr(start, end - start));
        start = end + 1;
        end = hub_sensors_str.find(',', start);
      }
      selected_hub_sensors.insert(hub_sensors_str.substr(start));
    }

    char grouping_param[32];
    if (httpd_query_key_value(query, "grouping", grouping_param, sizeof(grouping_param)) == ESP_OK) {
      std::string g(grouping_param);
      if (g == "panel" || g == "mppt" || g == "inverter" || g == "none") {
        grouping = g;
      }
    }
  }

  // If no sensors specified, use default set
  if (selected_sensors.empty()) {
    selected_sensors = {"power_in", "peak_power", "voltage_in", "voltage_out", "current_in", "current_out", "power_out", "temperature", "rssi"};
  }

  PSRAMString json_buffer;
  server->build_yaml_json(json_buffer, selected_sensors, selected_hub_sensors, grouping);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::cca_info_handler(httpd_req_t *req) {
  // /cca (R7) — /api/cca and /api/cca/refresh stay live, consumed from
  // #view-cca.
  return redirect_to_app_view(req, "cca");
}

esp_err_t TigoWebServer::api_cca_info_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString json_buffer;
  server->build_cca_info_json(json_buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_refresh_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  const char* response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    // BLE-sourced: hand off to the main loop (this handler runs on the httpd task,
    // and the BLE stack must be driven from the main loop). ble_loop_ picks this up
    // and runs connect → DEVICE_INFO → auto-disconnect.
    server->ble_refresh_requested_ = true;
    response = "{\"status\":\"ok\",\"message\":\"BLE refresh initiated\"}";
  } else
#endif
  {
    // Trigger full CCA refresh over HTTP (device info + config sync with sequencing)
    server->parent_->refresh_cca_data();
    response = "{\"status\":\"ok\",\"message\":\"CCA refresh initiated\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_discovery_handler(httpd_req_t *req) {
  // GET: return the last cached DISCOVERY_STATUS payload + its age. No BLE side effect —
  // the UI triggers fresh reads via /api/cca/discovery/poll.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  PSRAMString json;
#ifdef USE_TIGO_CCA_BLE
  std::string status = server->ble_get_discovery_json_();
  if (status.empty()) {
    json.append("{\"age_s\":null,\"status\":null}");
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", server->ble_get_discovery_seconds_ago_());
    json.append("{\"age_s\":");
    json.append(buf);
    json.append(",\"status\":");
    json.append(status.c_str());  // CCA payload is already a JSON object
    json.append("}");
  }
#else
  json.append("{\"age_s\":null,\"status\":null}");
#endif
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_discovery_start_handler(httpd_req_t *req) {
  // POST: hand off START_DISCOVERY to the main loop (BLE stack must run there).
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  const char *response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    server->ble_discovery_start_requested_ = true;
    response = "{\"status\":\"ok\",\"message\":\"Discovery start requested\"}";
  } else
#endif
  {
    httpd_resp_set_status(req, "400 Bad Request");
    response = "{\"status\":\"error\",\"message\":\"Discovery requires cca_source: ble\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_discovery_poll_handler(httpd_req_t *req) {
  // POST: hand off a DISCOVERY_STATUS read to the main loop; result lands in the cache
  // for the next GET /api/cca/discovery.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  const char *response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    server->ble_discovery_poll_requested_ = true;
    response = "{\"status\":\"ok\",\"message\":\"Discovery status poll requested\"}";
  } else
#endif
  {
    httpd_resp_set_status(req, "400 Bad Request");
    response = "{\"status\":\"error\",\"message\":\"Discovery requires cca_source: ble\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

namespace {
// Network reads the generic /poll endpoint may trigger (writes get dedicated routes).
// Only WiFi scan remains: WIFI_STATUS duplicated the NETWORK_INFO wlan block already shown,
// RUN_TEST returns "Internal error getting test results" on firmware 4.0.5-ct, and cellular
// status wasn't useful enough to keep its own control.
bool tigo_is_net_read_cmd(const char *cmd) {
  return strcmp(cmd, "NETWORK_WIFI_SCAN") == 0;
}
// Anything the GET cache endpoint will echo (read + write results). Gating the cmd here
// keeps an arbitrary query value from being interpolated raw into the JSON response.
bool tigo_is_known_net_cmd(const char *cmd) {
  return tigo_is_net_read_cmd(cmd) || strcmp(cmd, "NETWORK_WIFI_CONNECT") == 0 ||
         strcmp(cmd, "NETWORK_WIFI_CLEAR") == 0 || strcmp(cmd, "NETWORK_INFO") == 0;
}
}  // namespace

esp_err_t TigoWebServer::api_cca_network_handler(httpd_req_t *req) {
  // GET ?cmd=<CMD> — return the cached reply for that network command + its age. No BLE
  // side effect; the UI triggers fresh reads via /api/cca/network/poll.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  char query[96] = {};
  char cmd[48] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "cmd", cmd, sizeof(cmd)) != ESP_OK || !tigo_is_known_net_cmd(cmd)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"missing or unknown cmd\"}");
    return ESP_OK;
  }

  PSRAMString json;
  json.append("{\"cmd\":\"");
  json.append(cmd);
#ifdef USE_TIGO_CCA_BLE
  uint32_t age = 0;
  std::string result = server->ble_get_net_result_(cmd, age);
  if (result.empty()) {
    json.append("\",\"age_s\":null,\"result\":null}");
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", age);
    json.append("\",\"age_s\":");
    json.append(buf);
    json.append(",\"result\":");
    json.append(result.c_str());  // CCA payload is already a JSON value
    json.append("}");
  }
#else
  json.append("\",\"age_s\":null,\"result\":null}");
#endif
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_network_poll_handler(httpd_req_t *req) {
  // POST ?cmd=<CMD> — queue a network *read* (allowlisted); the reply lands in the cache
  // for the next GET /api/cca/network.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  char query[96] = {};
  char cmd[48] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "cmd", cmd, sizeof(cmd)) != ESP_OK || !tigo_is_net_read_cmd(cmd)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"cmd must be a known network read\"}");
    return ESP_OK;
  }

  const char *response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    server->ble_enqueue_net_request_(cmd, "");
    response = "{\"status\":\"ok\",\"message\":\"queued\"}";
  } else
#endif
  {
    httpd_resp_set_status(req, "400 Bad Request");
    response = "{\"status\":\"error\",\"message\":\"requires cca_source: ble\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, response);
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_wifi_connect_handler(httpd_req_t *req) {
  // POST {nid, pwd} — join the CCA to a WiFi network. nid/pwd are the exact keys the
  // Tigo app sends; values are URI-encoded server-side before going on the wire.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  char body[320];
  int ret = httpd_req_recv(req, body, sizeof(body) - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"empty body\"}");
    return ESP_OK;
  }
  body[ret] = '\0';

  // Same hand-rolled extractor the rename handlers use (no JSON parser dependency).
  auto extract = [](const char *src, const char *key, char *out, size_t outlen) -> bool {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(src, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t n = (size_t) (q2 - q1 - 1);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return true;
  };

  char nid[64] = {};
  char pwd[128] = {};
  if (!extract(body, "nid", nid, sizeof(nid)) || nid[0] == '\0') {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"missing 'nid' (SSID)\"}");
    return ESP_OK;
  }
  extract(body, "pwd", pwd, sizeof(pwd));  // open network -> empty password is valid

  const char *response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    std::string params = "nid=" + ble_uri_encode_(nid) + " pwd=" + ble_uri_encode_(pwd);
    server->ble_enqueue_net_request_("NETWORK_WIFI_CONNECT", params);
    response = "{\"status\":\"ok\",\"message\":\"WiFi join requested\"}";
  } else
#endif
  {
    httpd_resp_set_status(req, "400 Bad Request");
    response = "{\"status\":\"error\",\"message\":\"requires cca_source: ble\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, response);
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_wifi_clear_handler(httpd_req_t *req) {
  // POST — wipe the CCA's saved WiFi settings (destructive). Paramless protected command.
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  const char *response;
#ifdef USE_TIGO_CCA_BLE
  if (server->cca_source_ == CcaSource::BLE || server->cca_source_ == CcaSource::AUTO) {
    server->ble_enqueue_net_request_("NETWORK_WIFI_CLEAR", "");
    response = "{\"status\":\"ok\",\"message\":\"WiFi clear requested\"}";
  } else
#endif
  {
    httpd_resp_set_status(req, "400 Bad Request");
    response = "{\"status\":\"error\",\"message\":\"requires cca_source: ble\"}";
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, response);
  return ESP_OK;
}

esp_err_t TigoWebServer::api_node_delete_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }

  // Parse query parameter "addr"
  char query_str[64];
  if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) != ESP_OK) {
    const char* error_response = "{\"status\":\"error\",\"message\":\"Missing addr parameter\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  // Extract addr value
  char addr_str[16];
  if (httpd_query_key_value(query_str, "addr", addr_str, sizeof(addr_str)) != ESP_OK) {
    const char* error_response = "{\"status\":\"error\",\"message\":\"Invalid addr parameter\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  // Convert addr from hex string to uint16_t
  uint16_t addr = (uint16_t) strtol(addr_str, nullptr, 16);
  
  // Call remove_node
  bool success = server->parent_->remove_node(addr);
  
  // Return response
  std::string response;
  if (success) {
    response = "{\"status\":\"ok\",\"message\":\"Node deleted successfully\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
  } else {
    response = "{\"status\":\"error\",\"message\":\"Node not found\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_send(req, response.c_str(), response.length());
  }
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_node_import_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  // Log content type
  char content_type[64] = {0};
  if (httpd_req_get_hdr_value_len(req, "Content-Type") > 0) {
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    ESP_LOGI(TAG, "Import request Content-Type: %s", content_type);
  }
  
  // Read the POST body
  char* buf = nullptr;
  size_t buf_len = req->content_len;
  
  ESP_LOGI(TAG, "Import request content_len: %zu", buf_len);
  
  if (buf_len == 0 || buf_len > 102400) {  // Max 100KB
    const char* error_response = "{\"status\":\"error\",\"message\":\"Invalid content length\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  // Try to allocate from PSRAM first for large buffers
  size_t psram_available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (psram_available >= buf_len + 1024) {
    buf = static_cast<char*>(heap_caps_malloc(buf_len + 1, MALLOC_CAP_SPIRAM));
    if (buf) {
      ESP_LOGI(TAG, "Allocated %zu bytes from PSRAM for import buffer", buf_len + 1);
    }
  }
  
  // Fallback to regular heap
  if (!buf) {
    buf = static_cast<char*>(heap_caps_malloc(buf_len + 1, MALLOC_CAP_DEFAULT));
    if (!buf) {
      const char* error_response = "{\"status\":\"error\",\"message\":\"Out of memory\"}";
      httpd_resp_set_type(req, "application/json");
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_send(req, error_response, strlen(error_response));
      return ESP_OK;
    }
    ESP_LOGI(TAG, "Allocated %zu bytes from internal RAM for import buffer (PSRAM unavailable)", buf_len + 1);
  }
  
  // Read POST body - may require multiple calls for large payloads
  size_t total_received = 0;
  while (total_received < buf_len) {
    int ret = httpd_req_recv(req, buf + total_received, buf_len - total_received);
    if (ret <= 0) {
      heap_caps_free(buf);
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        const char* error_response = "{\"status\":\"error\",\"message\":\"Request timeout\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "408 Request Timeout");
        httpd_resp_send(req, error_response, strlen(error_response));
      } else {
        const char* error_response = "{\"status\":\"error\",\"message\":\"Failed to read request body\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_500(req);
      }
      return ESP_OK;
    }
    total_received += ret;
  }
  
  buf[total_received] = '\0';
  
  ESP_LOGD(TAG, "Received %zu bytes of JSON data", total_received);
  ESP_LOGV(TAG, "JSON content (first 200 chars): %.200s", buf);
  
  // Parse JSON using cJSON library
  std::vector<tigo_monitor::NodeTableData> nodes;
  
  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      ESP_LOGE(TAG, "JSON parse error before: %.50s", error_ptr);
    }
    heap_caps_free(buf);
    const char* error_response = "{\"status\":\"error\",\"message\":\"Invalid JSON - parse error\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  cJSON *nodes_array = cJSON_GetObjectItem(root, "nodes");
  if (!nodes_array || !cJSON_IsArray(nodes_array)) {
    cJSON_Delete(root);
    heap_caps_free(buf);
    const char* error_response = "{\"status\":\"error\",\"message\":\"Missing or invalid 'nodes' array\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  int array_size = cJSON_GetArraySize(nodes_array);
  ESP_LOGI(TAG, "Parsing %d nodes from JSON", array_size);
  
  for (int i = 0; i < array_size; i++) {
    cJSON *node_obj = cJSON_GetArrayItem(nodes_array, i);
    if (!node_obj || !cJSON_IsObject(node_obj)) continue;
    
    tigo_monitor::NodeTableData node;
    
    // Extract string fields
    cJSON *item;
    if ((item = cJSON_GetObjectItem(node_obj, "addr")) && cJSON_IsString(item)) {
      node.addr = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "long_address")) && cJSON_IsString(item)) {
      node.long_address = item->valuestring;
    }
    // frame09_barcode field removed - Frame 09 data is ignored
    if ((item = cJSON_GetObjectItem(node_obj, "checksum")) && cJSON_IsString(item)) {
      node.checksum = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "cca_label")) && cJSON_IsString(item)) {
      node.cca_label = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "cca_string")) && cJSON_IsString(item)) {
      node.cca_string_label = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "cca_inverter")) && cJSON_IsString(item)) {
      node.cca_inverter_label = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "cca_channel")) && cJSON_IsString(item)) {
      node.cca_channel = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(node_obj, "cca_object_id")) && cJSON_IsString(item)) {
      node.cca_object_id = item->valuestring;
    }
    
    // Extract int field
    if ((item = cJSON_GetObjectItem(node_obj, "sensor_index")) && cJSON_IsNumber(item)) {
      node.sensor_index = item->valueint;
    }
    
    // Extract bool field
    if ((item = cJSON_GetObjectItem(node_obj, "cca_validated")) && cJSON_IsBool(item)) {
      node.cca_validated = cJSON_IsTrue(item);
    }
    
    node.is_persistent = true;  // All imported nodes are persistent
    
    // Only add nodes that have actual device data (non-empty address AND barcode)
    if (!node.addr.empty() && !node.long_address.empty()) {
      nodes.push_back(node);
      ESP_LOGD(TAG, "Imported node %zu: addr=%s, barcode=%s", nodes.size(), 
               node.addr.c_str(), node.long_address.c_str());
    } else {
      ESP_LOGD(TAG, "Skipped empty node: addr=%s", node.addr.c_str());
    }
  }
  
  cJSON_Delete(root);
  
  ESP_LOGI(TAG, "Parsed %d JSON objects, added %zu valid nodes", array_size, nodes.size());
  
  // Free the input buffer immediately after parsing to reduce memory pressure
  heap_caps_free(buf);
  buf = nullptr;
  
  if (nodes.empty()) {
    const char* error_response = "{\"status\":\"error\",\"message\":\"No valid nodes found in import data\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  
  // Log memory status before import
  size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  ESP_LOGI(TAG, "Free internal RAM before import: %zu bytes", free_before);
  
  // Import the nodes
  bool success = server->parent_->import_node_table(nodes);
  
  // Log memory status after import
  size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  ESP_LOGI(TAG, "Free internal RAM after import: %zu bytes (used %zu bytes, minimum ever: %zu bytes)", 
           free_after, free_before - free_after, min_free);
  
  if (success) {
    char response[256];
    snprintf(response, sizeof(response),
      "{\"status\":\"ok\",\"message\":\"Successfully imported %zu nodes\",\"imported\":%zu}",
      nodes.size(), nodes.size());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
  } else {
    const char* error_response = "{\"status\":\"error\",\"message\":\"Failed to import node table\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, error_response, strlen(error_response));
  }
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_restart_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  ESP_LOGW(TAG, "Restart requested via web interface - rebooting...");
  
  // Send success response
  const char* response = "{\"status\":\"ok\",\"message\":\"Restarting ESP32...\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  
  // Schedule restart after a short delay to allow response to be sent
  App.safe_reboot();
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_reset_peak_power_handler(httpd_req_t *req) {
  TigoWebServer *server = (TigoWebServer *)req->user_ctx;
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  ESP_LOGI(TAG, "Reset peak power requested via web interface");
  
  // Reset peak power for all devices
  server->parent_->reset_peak_power();
  
  // Send success response
  const char* response = "{\"status\":\"ok\",\"message\":\"Peak power values reset successfully\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_reset_node_table_handler(httpd_req_t *req) {
  TigoWebServer *server = (TigoWebServer *)req->user_ctx;
  if (!server->check_api_auth(req)) {
    return ESP_OK;
  }
  
  ESP_LOGI(TAG, "Reset node table requested via web interface");
  
  // Reset node table (clears all device mappings, barcodes, CCA data)
  server->parent_->reset_node_table();
  
  // Send success response
  const char* response = "{\"status\":\"ok\",\"message\":\"Node table reset successfully\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_health_handler(httpd_req_t *req) {
  // Health check endpoint - no authentication required for monitoring systems
  // Returns simple JSON with status and uptime
  
  uint32_t uptime_seconds = millis() / 1000;
  
  char response[256];
  snprintf(response, sizeof(response),
    "{\"status\":\"ok\",\"uptime\":%u,\"heap_free\":%u,\"heap_min_free\":%u}",
    uptime_seconds,
    esp_get_free_heap_size(),
    esp_get_minimum_free_heap_size()
  );
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  
  return ESP_OK;
}
// ===== JSON Builders =====

void TigoWebServer::build_devices_json(PSRAMString& json) {
  json.append("{\"devices\":[");
  
  // Intermediate entries hold pointers into the live device/node tables rather
  // than copying each struct's std::string members. Copying churned the small
  // internal heap on every poll and, under dashboard refresh, fragmented it to
  // OOM (#23). The pointers are valid only while the state lock below is held.
  struct DeviceWithName {
    const tigo_monitor::DeviceData *device;  // nullptr if node-only (no runtime data)
    const std::string *addr;
    const std::string *barcode;
    const std::string *cca_label;            // nullptr when there is no CCA label
    const std::string *string_label;
    int sensor_index;
    bool has_runtime_data;
  };

  static const std::string EMPTY_STR;

  parent_->with_state_lock([&]() {
    const auto &devices = parent_->get_devices();
    const auto &node_table = parent_->get_node_table();

    std::vector<DeviceWithName> sorted_devices;

    // First, add all devices that have runtime data
    for (const auto &device : devices) {
      DeviceWithName dwn;
      dwn.device = &device;
      dwn.addr = &device.addr;
      dwn.barcode = &device.barcode;
      dwn.cca_label = nullptr;
      dwn.string_label = &EMPTY_STR;
      dwn.sensor_index = -1;
      dwn.has_runtime_data = true;

      // Find the node table entry to get CCA label, string label, sensor index
      for (const auto &node : node_table) {
        if (node.addr == device.addr) {
          dwn.sensor_index = node.sensor_index;
          dwn.string_label = &node.cca_string_label;
          if (!node.cca_label.empty()) dwn.cca_label = &node.cca_label;
          break;
        }
      }

      sorted_devices.push_back(dwn);
    }

    // Now add nodes from node_table that don't have runtime data yet (e.g. the
    // ESP32 restarted at night - known but not yet seen this session)
    for (const auto &node : node_table) {
      bool found = false;
      for (const auto &existing : sorted_devices) {
        if (*existing.addr == node.addr) { found = true; break; }
      }

      if (!found && node.sensor_index >= 0) {
        DeviceWithName dwn;
        dwn.device = nullptr;
        dwn.addr = &node.addr;
        dwn.barcode = &node.long_address;   // Frame 27 long address as barcode
        dwn.cca_label = node.cca_label.empty() ? nullptr : &node.cca_label;
        dwn.string_label = &node.cca_string_label;
        dwn.sensor_index = node.sensor_index;
        dwn.has_runtime_data = false;
        sorted_devices.push_back(dwn);
      }
    }

    // CCA-labelled devices first (alphabetical), then unlabelled by sensor index
    std::sort(sorted_devices.begin(), sorted_devices.end(),
              [](const DeviceWithName &a, const DeviceWithName &b) {
                bool a_has_cca = (a.cca_label != nullptr);
                bool b_has_cca = (b.cca_label != nullptr);
                if (a_has_cca && b_has_cca) return *a.cca_label < *b.cca_label;
                if (!a_has_cca && !b_has_cca) return a.sensor_index < b.sensor_index;
                return a_has_cca;
              });

    bool first = true;
    for (const auto &dwn : sorted_devices) {
      if (!first) json.append(",");
      first = false;

      // Resolve the display name without copying: CCA label, else barcode,
      // else a "Module <addr>" fallback built in a small stack buffer.
      char name_buf[48];
      const char *name_cstr;
      if (dwn.cca_label != nullptr) {
        name_cstr = dwn.cca_label->c_str();
      } else if (dwn.barcode->length() >= 5) {
        name_cstr = dwn.barcode->c_str();
      } else {
        snprintf(name_buf, sizeof(name_buf), "Module %s", dwn.addr->c_str());
        name_cstr = name_buf;
      }

      char buffer[600];

      if (dwn.has_runtime_data && dwn.device != nullptr) {
        // Device has runtime data - show actual values
        const auto &device = *dwn.device;
        // If last_update is 0, device hasn't been updated yet - use ULONG_MAX to indicate "never"
        unsigned long data_age_ms = (device.last_update == 0) ? ULONG_MAX : (millis() - device.last_update);
        float duty_cycle_percent = (device.duty_cycle / 255.0f) * 100.0f;

        snprintf(buffer, sizeof(buffer),
          "{\"addr\":\"%s\",\"barcode\":\"%s\",\"name\":\"%s\",\"string_label\":\"%s\",\"voltage_in\":%.2f,\"voltage_out\":%.2f,"
          "\"current\":%.3f,\"current_out\":%.3f,\"power_in\":%.1f,\"power\":%.1f,\"power_out\":%.1f,\"peak_power\":%.1f,\"temperature\":%.1f,\"rssi\":%d,"
          "\"duty_cycle\":%.1f,\"efficiency\":%.2f,\"data_age_ms\":%lu,\"stale\":%s}",
          device.addr.c_str(), device.barcode.c_str(), name_cstr, dwn.string_label->c_str(), device.voltage_in, device.voltage_out,
          device.current_in, device.current_out, device.power_in, device.power_out, device.power_out, device.peak_power, device.temperature, device.rssi,
          duty_cycle_percent, device.efficiency, data_age_ms, device.is_stale ? "true" : "false");
      } else {
        // Device is known but has no runtime data yet (e.g., ESP32 restarted at night)
        // Show zeros with a very large data_age to indicate no recent data
        snprintf(buffer, sizeof(buffer),
          "{\"addr\":\"%s\",\"barcode\":\"%s\",\"name\":\"%s\",\"string_label\":\"%s\",\"voltage_in\":0.00,\"voltage_out\":0.00,"
          "\"current\":0.000,\"current_out\":0.000,\"power_in\":0.0,\"power\":0.0,\"power_out\":0.0,\"peak_power\":0.0,\"temperature\":0.0,\"rssi\":0,"
          "\"duty_cycle\":0.0,\"efficiency\":0.00,\"data_age_ms\":999999999,\"stale\":true}",
          dwn.addr->c_str(), dwn.barcode->c_str(), name_cstr, dwn.string_label->c_str());
      }

      json.append(buffer);
    }
  });

  json.append("]}");
}

void TigoWebServer::build_overview_json(PSRAMString& json) {
  // In night mode, use cached values (all zeros)
  if (parent_->is_in_night_mode()) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
      "{\"total_power\":%.1f,\"total_current\":%.3f,\"avg_efficiency\":%.2f,"
      "\"avg_temperature\":%.1f,\"active_devices\":%d,\"max_devices\":%d,\"total_energy\":%.3f,\"total_energy_in\":%.3f,\"total_energy_out\":%.3f}",
      0.0f, 0.0f, 0.0f, 0.0f,
      0, parent_->get_number_of_devices(),
      parent_->get_total_energy_out_kwh(), parent_->get_total_energy_in_kwh(), parent_->get_total_energy_out_kwh());
    
    json.append(buffer);
    return;
  }
  
  float total_power_out = 0.0f;
  float total_current = 0.0f;
  float avg_efficiency = 0.0f;
  float avg_temp = 0.0f;
  int active_devices = 0;

  parent_->with_state_lock([&]() {
    for (const auto &device : parent_->get_devices()) {
      total_power_out += device.power_out;
      total_current += device.current_in;
      avg_efficiency += device.efficiency;
      avg_temp += device.temperature;
      active_devices++;
    }
  });
  
  if (active_devices > 0) {
    avg_efficiency /= active_devices;
    avg_temp /= active_devices;
  }
  
  float total_energy_in = parent_->get_total_energy_in_kwh();
  float total_energy_out = parent_->get_total_energy_out_kwh();
  
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
    "{\"total_power\":%.1f,\"total_current\":%.3f,\"avg_efficiency\":%.2f,"
    "\"avg_temperature\":%.1f,\"active_devices\":%d,\"max_devices\":%d,\"total_energy\":%.3f,\"total_energy_in\":%.3f,\"total_energy_out\":%.3f}",
    total_power_out, total_current, avg_efficiency, avg_temp,
    active_devices, parent_->get_number_of_devices(), total_energy_out, total_energy_in, total_energy_out);
  
  json.append(buffer);
}

void TigoWebServer::build_strings_json(PSRAMString& json) {
  json.append("{\"strings\":[");

  bool first = true;

  parent_->with_state_lock([&]() {
  const auto &strings = parent_->get_strings();
  ESP_LOGD(TAG, "Building strings JSON - found %d strings", strings.size());

  for (const auto &pair : strings) {
    if (!first) json.append(",");
    first = false;
    
    const auto &string_data = pair.second;
    
    ESP_LOGD(TAG, "String: %s, devices: %d/%d, power: %.0fW", 
             string_data.string_label.c_str(), 
             string_data.active_device_count, 
             string_data.total_device_count,
             string_data.total_power);
    
    char buffer[680];
    snprintf(buffer, sizeof(buffer),
      "{\"label\":\"%s\",\"display_label\":\"%s\",\"inverter\":\"%s\","
      "\"panel_rating_w\":%u,"
      "\"total_power\":%.1f,\"peak_power\":%.1f,"
      "\"total_current\":%.3f,\"avg_voltage_in\":%.2f,\"avg_voltage_out\":%.2f,"
      "\"avg_temperature\":%.1f,\"avg_efficiency\":%.2f,\"min_efficiency\":%.2f,"
      "\"max_efficiency\":%.2f,\"active_devices\":%d,\"total_devices\":%d}",
      string_data.string_label.c_str(), string_data.display_label.c_str(),
      string_data.inverter_label.c_str(),
      (unsigned) string_data.panel_rating_w,
      string_data.total_power, string_data.peak_power, string_data.total_current,
      string_data.avg_voltage_in, string_data.avg_voltage_out,
      string_data.avg_temperature, string_data.avg_efficiency,
      string_data.min_efficiency, string_data.max_efficiency,
      string_data.active_device_count, string_data.total_device_count);

    json.append(buffer);
  }
  });

  json.append("]}");
}

void TigoWebServer::build_inverters_json(PSRAMString& json) {
  json.append("{\"inverters\":[");

  parent_->with_state_lock([&]() {
  const auto &inverters = parent_->get_inverters();
  const auto &strings = parent_->get_strings();

  ESP_LOGD(TAG, "Building inverters JSON - found %d inverters", inverters.size());

  bool first_inv = true;
  for (const auto &inverter : inverters) {
    if (!first_inv) json.append(",");
    first_inv = false;
    
    ESP_LOGD(TAG, "Inverter: %s, devices: %d/%d, power: %.0fW", 
             inverter.name.c_str(),
             inverter.active_device_count, 
             inverter.total_device_count,
             inverter.total_power);
    
    // Build MPPT labels array
    PSRAMString mppt_labels_json;
    mppt_labels_json.append("[");
    bool first_mppt = true;
    for (const auto &mppt : inverter.mppt_labels) {
      if (!first_mppt) mppt_labels_json.append(",");
      first_mppt = false;
      mppt_labels_json.append("\"");
      mppt_labels_json.append(mppt.c_str());
      mppt_labels_json.append("\"");
    }
    mppt_labels_json.append("]");
    
    // Build strings array for this inverter
    PSRAMString strings_json;
    strings_json.append("[");
    bool first_str = true;
    for (const auto &mppt_label : inverter.mppt_labels) {
      for (const auto &string_pair : strings) {
        const auto &string_data = string_pair.second;
        if (string_data.inverter_label == mppt_label) {
          if (!first_str) strings_json.append(",");
          first_str = false;
          
          char buffer[680];
          snprintf(buffer, sizeof(buffer),
            "{\"label\":\"%s\",\"display_label\":\"%s\",\"mppt\":\"%s\","
            "\"panel_rating_w\":%u,"
            "\"total_power\":%.1f,\"peak_power\":%.1f,"
            "\"active_devices\":%d,\"total_devices\":%d}",
            string_data.string_label.c_str(), string_data.display_label.c_str(),
            string_data.inverter_label.c_str(),
            (unsigned) string_data.panel_rating_w,
            string_data.total_power, string_data.peak_power,
            string_data.active_device_count, string_data.total_device_count);
          strings_json.append(buffer);
        }
      }
    }
    strings_json.append("]");
    
    // Build the inverter JSON object
    json.append("{\"name\":\"");
    json.append(inverter.name.c_str());
    json.append("\",\"display_name\":\"");
    json.append(inverter.display_name.c_str());
    json.append("\",\"mppts\":");
    json.append(mppt_labels_json.c_str());
    json.append(",\"total_power\":");
    
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.1f", inverter.total_power);
    json.append(buffer);
    json.append(",\"peak_power\":");
    snprintf(buffer, sizeof(buffer), "%.1f", inverter.peak_power);
    json.append(buffer);
    
    json.append(",\"active_devices\":");
    snprintf(buffer, sizeof(buffer), "%d", inverter.active_device_count);
    json.append(buffer);
    json.append(",\"total_devices\":");
    snprintf(buffer, sizeof(buffer), "%d", inverter.total_device_count);
    json.append(buffer);
    
    json.append(",\"strings\":");
    json.append(strings_json.c_str());
    json.append("}");
  }
  });

  json.append("]}");
}

void TigoWebServer::build_energy_history_json(PSRAMString& json) {
  if (!parent_) {
    json.append("{\"error\":\"No parent component\"}");
    return;
  }
  
  auto history = parent_->get_daily_energy_history();
  float current_energy = parent_->get_total_energy_out_kwh();
  float energy_at_day_start = parent_->get_energy_at_day_start();
  
  json.append("{\"current_energy\":");
  
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.3f", current_energy);
  json.append(buffer);
  
  json.append(",\"history\":[");
  
  // Fill in last 7 days with 0 for missing data
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t now_time = tv.tv_sec;
  
  bool first = true;
  for (int days_ago = 6; days_ago >= 0; days_ago--) {
    if (!first) json.append(",");
    first = false;
    
    // Calculate date for days_ago
    time_t day_time = now_time - (days_ago * 86400);
    struct tm day_tm;
    localtime_r(&day_time, &day_tm);
    
    // Look for this date in history
    float energy = 0.0f;
    
    // For today (days_ago == 0), use current production
    if (days_ago == 0) {
      energy = current_energy - energy_at_day_start;
    } else {
      // For past days, look in archived history
      for (const auto &entry : history) {
        if (entry.year == (day_tm.tm_year + 1900) && 
            entry.month == (day_tm.tm_mon + 1) && 
            entry.day == day_tm.tm_mday) {
          energy = entry.energy_kwh;
          break;
        }
      }
    }
    
    json.append("{\"date\":\"");
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", 
             day_tm.tm_year + 1900, day_tm.tm_mon + 1, day_tm.tm_mday);
    json.append(buffer);
    json.append("\",\"energy\":");
    snprintf(buffer, sizeof(buffer), "%.3f", energy);
    json.append(buffer);
    json.append("}");
  }
  
  json.append("]}");
}

void TigoWebServer::build_node_table_json(PSRAMString& json) {
  // Configure cJSON to use PSRAM if available
  cJSON_Hooks hooks;
  bool using_psram = false;
  size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  
  if (total_psram > 0) {
    hooks.malloc_fn = [](size_t size) -> void* {
      void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
      if (!ptr) {
        ptr = malloc(size);  // Fallback to regular heap
      }
      return ptr;
    };
    hooks.free_fn = [](void* ptr) { heap_caps_free(ptr); };
    cJSON_InitHooks(&hooks);
    using_psram = true;
  }
  
  cJSON *root = cJSON_CreateObject();
  cJSON *nodes_array = cJSON_CreateArray();

  parent_->with_state_lock([&]() {
  for (const auto &node : parent_->get_node_table()) {
    cJSON *node_obj = cJSON_CreateObject();

    cJSON_AddStringToObject(node_obj, "addr", node.addr.c_str());
    cJSON_AddStringToObject(node_obj, "long_address", node.long_address.c_str());
    // frame09_barcode field removed - Frame 09 data is ignored
    cJSON_AddNumberToObject(node_obj, "sensor_index", node.sensor_index);
    cJSON_AddStringToObject(node_obj, "checksum", node.checksum.c_str());
    cJSON_AddBoolToObject(node_obj, "cca_validated", node.cca_validated);
    cJSON_AddStringToObject(node_obj, "cca_label", node.cca_label.c_str());
    cJSON_AddStringToObject(node_obj, "cca_string", node.cca_string_label.c_str());
    cJSON_AddStringToObject(node_obj, "cca_inverter", node.cca_inverter_label.c_str());
    cJSON_AddStringToObject(node_obj, "cca_channel", node.cca_channel.c_str());
    
    cJSON_AddItemToArray(nodes_array, node_obj);
  }
  });

  cJSON_AddItemToObject(root, "nodes", nodes_array);
  
  char *json_str = cJSON_Print(root);
  json.append(json_str);
  
  cJSON_free(json_str);
  cJSON_Delete(root);
  
  // Reset to default allocators
  if (using_psram) {
    cJSON_InitHooks(NULL);
  }
}

void TigoWebServer::build_esp_status_json(PSRAMString& json) {
  // Get heap info - use MALLOC_CAP_INTERNAL for internal RAM only (excludes PSRAM)
  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  
  // Get uptime
  uint32_t uptime_sec = millis() / 1000;
  uint32_t uptime_days = uptime_sec / 86400;
  uint32_t uptime_hours = (uptime_sec % 86400) / 3600;
  uint32_t uptime_mins = (uptime_sec % 3600) / 60;
  
  // Get task count (this function is always available)
  UBaseType_t task_count = uxTaskGetNumberOfTasks();
  
  // Get minimum free heap ever seen - also use INTERNAL to track internal RAM watermark
  size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  
  // Get UART diagnostics from parent
  uint32_t invalid_checksum = parent_->get_invalid_checksum_count();
  uint32_t missed_frames = parent_->get_missed_frame_count();
  uint32_t total_frames = parent_->get_total_frames_processed();
  uint32_t command_frames = parent_->get_command_frame_count();
  uint32_t frame_27_count = parent_->get_frame_27_count();
  
  // ESP32 internal die temperature. Prefer a user-wired internal_temperature
  // sensor (the conflict-free path on the single-peripheral ESP32); otherwise
  // read our own handle. Either source can be unavailable — no peripheral (the
  // classic ESP32 lacks it), a failed read, or no state yet — so track
  // availability and report null rather than a misleading 0 °C (#28).
  float internal_temp = 0.0f;
  bool internal_temp_ok = false;
  if (external_temp_sensor_ != nullptr) {
    if (external_temp_sensor_->has_state() && std::isfinite(external_temp_sensor_->state)) {
      internal_temp = external_temp_sensor_->state;
      internal_temp_ok = true;
    }
  } else if (temp_sensor_handle_ != nullptr &&
             temperature_sensor_get_celsius(temp_sensor_handle_, &internal_temp) == ESP_OK) {
    internal_temp_ok = true;
  }
  
  // Get network stats
  bool network_connected = network::is_connected();
  int8_t wifi_rssi = 0;
  std::string ip_address = "N/A";
  std::string mac_address = "N/A";
  std::string ssid = "N/A";
  
  // IP and MAC come from the network layer so they populate on Ethernet too,
  // not only WiFi (#27). RSSI/SSID are inherently WiFi-only and stay "N/A" on a
  // wired link.
#ifdef USE_NETWORK
  for (auto &addr : network::get_ip_addresses()) {
    if (addr.is_set()) {
      char buf[40];
      addr.str_to(buf);
      ip_address = buf;
      break;
    }
  }
  mac_address = get_mac_address_pretty();
#endif

#ifdef USE_WIFI
  if (network_connected && wifi::global_wifi_component != nullptr) {
    wifi_rssi = wifi::global_wifi_component->wifi_rssi();
    char ssid_buf[wifi::SSID_BUFFER_SIZE];
    ssid = wifi::global_wifi_component->wifi_ssid_to(ssid_buf);
  }
#endif

  // Get active socket count from HTTP server if available
  int active_sockets = 0;
  if (server_) {
    // Query the HTTP server for active client sessions
    // This is much more efficient than scanning all LWIP sockets
    // First call to get count, then allocate and get the actual FDs
    size_t max_clients = 10;  // Should be enough for our config (max_open_sockets = 4)
    int client_fds[10];
    esp_err_t err = httpd_get_client_list(server_, &max_clients, client_fds);
    if (err == ESP_OK) {
      active_sockets = max_clients;
    }
  }
  
#ifdef CONFIG_LWIP_MAX_SOCKETS
  int max_sockets = CONFIG_LWIP_MAX_SOCKETS;
#else
  int max_sockets = 16;
#endif
  
  // Emit a JSON null (not 0) when the die-temperature read isn't available, so
  // the dashboard can show N/A instead of a misleading 0 °C (#28).
  char temp_str[16];
  if (internal_temp_ok) {
    snprintf(temp_str, sizeof(temp_str), "%.1f", internal_temp);
  } else {
    strcpy(temp_str, "null");
  }

  char buffer[1536];
  snprintf(buffer, sizeof(buffer),
    "{\"free_heap\":%zu,\"total_heap\":%zu,\"free_psram\":%zu,\"total_psram\":%zu,"
    "\"min_free_heap\":%zu,\"min_free_psram\":%zu,"
    "\"uptime_sec\":%u,\"uptime_days\":%u,\"uptime_hours\":%u,\"uptime_mins\":%u,"
    "\"esphome_version\":\"%s\",\"compilation_time\":\"%s %s\","
    "\"task_count\":%u,\"internal_temp\":%s,"
    "\"invalid_checksum\":%u,\"missed_frames\":%u,\"total_frames\":%u,"
    "\"command_frames\":%u,\"frame_27_count\":%u,"
    "\"network_connected\":%s,\"wifi_rssi\":%d,\"wifi_ssid\":\"%s\",\"ip_address\":\"%s\",\"mac_address\":\"%s\","
    "\"active_sockets\":%d,\"max_sockets\":%d}",
    free_heap, total_heap, free_psram, total_psram,
    min_free_heap, min_free_psram,
    uptime_sec, uptime_days, uptime_hours, uptime_mins,
    ESPHOME_VERSION, __DATE__, __TIME__,
    (unsigned int)task_count, temp_str,
    invalid_checksum, missed_frames, total_frames,
    command_frames, frame_27_count,
    network_connected ? "true" : "false", wifi_rssi, ssid.c_str(), ip_address.c_str(), mac_address.c_str(),
    active_sockets, max_sockets);
  
  json.append(buffer);
}

void TigoWebServer::build_yaml_json(PSRAMString& json, const std::set<std::string>& selected_sensors, const std::set<std::string>& selected_hub_sensors, const std::string& grouping) {
  PSRAMString yaml_text;
  const auto node_table = parent_->snapshot_node_table();

  // Build YAML configuration. Hold pointers into the local snapshot rather than
  // copying each NodeTableData (and its std::string members) a second time — the
  // by-value copy here was the heaviest internal-RAM allocation in the web layer
  // and the exact site that OOM-crashed under heap exhaustion (#23).
  std::vector<const tigo_monitor::NodeTableData*> assigned_nodes;
  for (const auto &node : node_table) {
    if (node.sensor_index >= 0) {
      assigned_nodes.push_back(&node);
    }
  }

  // Sort by sensor index
  std::sort(assigned_nodes.begin(), assigned_nodes.end(),
            [](const auto *a, const auto *b) { return a->sensor_index < b->sensor_index; });

  // Resolve effective grouping: "inverter" falls back to "mppt" if no
  // inverters are configured (otherwise every panel lands on the same
  // fallback bucket).
  std::string effective_grouping = grouping;
  if (effective_grouping == "inverter" && parent_->get_inverters().empty()) {
    effective_grouping = "mppt";
  }

  auto slugify = [](const std::string &s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c >= 'A' && c <= 'Z') out.push_back(c + ('a' - 'A'));
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
      else if (!out.empty() && out.back() != '_') out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out = "x_" + out;
    return out;
  };

  // Build "tigo_<type>_<slug>", but skip the <type> tag when the slug
  // already starts with it (e.g. label "MPPT 4" -> "tigo_mppt_4", not
  // "tigo_mppt_mppt_4"). Inverter "FlexBoss A" stays "tigo_inverter_flexboss_a".
  auto make_id = [&slugify](const char *type_tag, const std::string &label) -> std::string {
    std::string slug = slugify(label);
    std::string tag(type_tag);
    bool already_prefixed = (slug == tag) ||
        (slug.size() > tag.size() && slug[tag.size()] == '_' &&
         slug.compare(0, tag.size(), tag) == 0);
    return already_prefixed ? ("tigo_" + slug) : ("tigo_" + tag + "_" + slug);
  };

  std::map<int, std::string> node_device_id;
  std::vector<std::pair<std::string, std::string>> devices;
  std::set<std::string> seen_ids;
  auto register_device = [&](const std::string &id, const std::string &display) {
    if (seen_ids.insert(id).second) {
      devices.emplace_back(id, display);
    }
  };

  if (effective_grouping == "panel") {
    for (const auto *node_ptr : assigned_nodes) {
      const auto &node = *node_ptr;
      std::string idx_str = std::to_string(node.sensor_index + 1);
      std::string display = !node.cca_label.empty() ? node.cca_label
                                                    : ("Tigo Panel " + idx_str);
      std::string id = "tigo_panel_" + idx_str;
      node_device_id[node.sensor_index] = id;
      register_device(id, display);
    }
  } else if (effective_grouping == "mppt") {
    for (const auto *node_ptr : assigned_nodes) {
      const auto &node = *node_ptr;
      std::string label = node.cca_inverter_label.empty() ? std::string("Unassigned MPPT")
                                                          : node.cca_inverter_label;
      std::string id = make_id("mppt", label);
      node_device_id[node.sensor_index] = id;
      register_device(id, label);
    }
  } else if (effective_grouping == "inverter") {
    std::map<std::string, std::string> mppt_to_inverter;
    for (const auto &inv : parent_->get_inverters()) {
      const std::string &name = inv.display_name.empty() ? inv.name : inv.display_name;
      for (const auto &mppt : inv.mppt_labels) {
        mppt_to_inverter[mppt] = name;
      }
    }
    for (const auto *node_ptr : assigned_nodes) {
      const auto &node = *node_ptr;
      auto it = mppt_to_inverter.find(node.cca_inverter_label);
      std::string display = (it != mppt_to_inverter.end()) ? it->second : std::string("Unassigned");
      std::string id = make_id("inverter", display);
      node_device_id[node.sensor_index] = id;
      register_device(id, display);
    }
  }

  if (!devices.empty()) {
    yaml_text.append("# Add these device entries under your existing `esphome:` block.\n");
    yaml_text.append("esphome:\n");
    yaml_text.append("  devices:\n");
    for (const auto &d : devices) {
      yaml_text.append("    - id: ");
      yaml_text.append(d.first.c_str());
      yaml_text.append("\n      name: \"");
      yaml_text.append(d.second.c_str());
      yaml_text.append("\"\n");
    }
    yaml_text.append("\n");
  }

  yaml_text.append("sensor:\n");
  
  // Add hub-level sensors if any are selected
  // Each hub sensor must be its own platform entry with keyword-rich name
  // (sensor type is inferred from name keywords in sensor.py)
  if (!selected_hub_sensors.empty()) {
    yaml_text.append("  # Hub-level sensors (system-wide, no address required)\n");
    yaml_text.append("  # Sensor type is auto-detected from name keywords\n");
    
    if (selected_hub_sensors.count("power_sum") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Total Input Power\"\n\n");
    }
    if (selected_hub_sensors.count("power_out_sum") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Total Output Power\"\n\n");
    }
    if (selected_hub_sensors.count("energy_in_sum") > 0 || selected_hub_sensors.count("energy_sum") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Total Energy In\"\n\n");
    }
    if (selected_hub_sensors.count("energy_out_sum") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Total Energy Out\"\n\n");
    }
    if (selected_hub_sensors.count("device_count") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Active Device Count\"\n\n");
    }
    if (selected_hub_sensors.count("invalid_checksum") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Invalid Checksum Count\"\n\n");
    }
    if (selected_hub_sensors.count("missed_frame") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Missed Frame Count\"\n\n");
    }
    if (selected_hub_sensors.count("internal_ram_free") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Free Internal RAM\"\n\n");
    }
    if (selected_hub_sensors.count("internal_ram_min") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Min Free Internal RAM\"\n\n");
    }
    if (selected_hub_sensors.count("psram_free") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Free PSRAM\"\n\n");
    }
    if (selected_hub_sensors.count("stack_free") > 0) {
      yaml_text.append("  - platform: tigo_monitor\n");
      yaml_text.append("    tigo_monitor_id: tigo_hub\n");
      yaml_text.append("    name: \"Free Stack\"\n\n");
    }
  }
  
  // Add per-device sensors
  for (const auto *node_ptr : assigned_nodes) {
    const auto &node = *node_ptr;
    std::string index_str = std::to_string(node.sensor_index + 1);
    std::string barcode_comment = "";
    std::string device_name;
    
    // Prefer CCA label if available, otherwise use generic name
    if (!node.cca_label.empty()) {
      device_name = node.cca_label;
      if (!node.cca_string_label.empty() || !node.cca_inverter_label.empty()) {
        barcode_comment = " - CCA: " + node.cca_inverter_label + " / " + node.cca_string_label;
      }
    } else {
      device_name = "Tigo Device " + index_str;
      if (!node.long_address.empty()) {
        barcode_comment = " - Frame27: " + node.long_address;
      }
    }
    
    yaml_text.append("  # ");
    yaml_text.append(device_name.c_str());
    yaml_text.append(" (discovered");
    yaml_text.append(barcode_comment.c_str());
    yaml_text.append(")\n");
    yaml_text.append("  - platform: tigo_monitor\n");
    yaml_text.append("    tigo_monitor_id: tigo_hub\n");
    yaml_text.append("    address: \"");
    yaml_text.append(node.addr.c_str());
    yaml_text.append("\"\n");
    yaml_text.append("    name: \"");
    yaml_text.append(device_name.c_str());
    yaml_text.append("\"\n");

    auto it_dev = node_device_id.find(node.sensor_index);
    if (it_dev != node_device_id.end()) {
      yaml_text.append("    device_id: ");
      yaml_text.append(it_dev->second.c_str());
      yaml_text.append("\n");
    }

    // Add only selected sensors
    if (selected_sensors.count("power_in") > 0 || selected_sensors.count("power") > 0) {
      yaml_text.append("    power_in: {}\n");
    }
    if (selected_sensors.count("peak_power") > 0) {
      yaml_text.append("    peak_power: {}\n");
    }
    if (selected_sensors.count("power_out") > 0) {
      yaml_text.append("    power_out: {}\n");
    }
    if (selected_sensors.count("voltage_in") > 0) {
      yaml_text.append("    voltage_in: {}\n");
    }
    if (selected_sensors.count("voltage_out") > 0) {
      yaml_text.append("    voltage_out: {}\n");
    }
    if (selected_sensors.count("current_in") > 0) {
      yaml_text.append("    current_in: {}\n");
    }
    if (selected_sensors.count("current_out") > 0) {
      yaml_text.append("    current_out: {}\n");
    }
    if (selected_sensors.count("temperature") > 0) {
      yaml_text.append("    temperature: {}\n");
    }
    if (selected_sensors.count("rssi") > 0) {
      yaml_text.append("    rssi: {}\n");
    }
    if (selected_sensors.count("duty_cycle") > 0) {
      yaml_text.append("    duty_cycle: {}\n");
    }
    if (selected_sensors.count("efficiency") > 0) {
      yaml_text.append("    efficiency: {}\n");
    }
    if (selected_sensors.count("power_factor") > 0) {
      yaml_text.append("    power_factor: {}\n");
    }
    if (selected_sensors.count("load_factor") > 0) {
      yaml_text.append("    load_factor: {}\n");
    }
    if (selected_sensors.count("barcode") > 0) {
      yaml_text.append("    barcode: {}\n");
    }
    
    yaml_text.append("\n");
  }
  
  // Escape for JSON - convert newlines to \n
  json.append("{\"yaml\":\"");
  for (size_t i = 0; i < yaml_text.length(); i++) {
    char c = yaml_text.c_str()[i];
    if (c == '"') json.append("\\\"");
    else if (c == '\\') json.append("\\\\");
    else if (c == '\n') json.append("\\n");
    else if (c == '\r') json.append("\\r");
    else if (c == '\t') json.append("\\t");
    else {
      char buf[2] = {c, '\0'};
      json.append(buf);
    }
  }
  
  char count_buf[64];
  snprintf(count_buf, sizeof(count_buf), "\",\"device_count\":%zu}", assigned_nodes.size());
  json.append(count_buf);
}

// ===== HTML Page Generators =====

void TigoWebServer::get_app_html(PSRAMString& html) {
  html.append(APP_HTML_PRE);
  html.append(api_token_);
  html.append(APP_HTML_POST);
}

void TigoWebServer::build_cca_info_json(PSRAMString& json) {
  // Source of the device-info payload + freshness, per cca_source_:
  //   HTTP — tigo_monitor's local HTTP query (401s on CCA firmware 4.0.4+)
  //   BLE  — this server's own BLE cache (authoritative; no HTTP fallback)
  //   AUTO — BLE cache if it has data, else HTTP
  std::string device_info;
  std::string source_id = parent_->get_cca_ip();
  unsigned long seconds_ago = 0;

  bool use_ble = false;
#ifdef USE_TIGO_CCA_BLE
  if (cca_source_ == CcaSource::BLE) {
    use_ble = true;  // authoritative even before the first refresh (shows "awaiting")
  } else if (cca_source_ == CcaSource::AUTO && this->ble_has_cca_info_()) {
    use_ble = true;
  }

  if (use_ble) {
    device_info = this->ble_get_cca_info_json_();  // "" until first DEVICE_INFO
    seconds_ago = this->ble_get_cca_info_seconds_ago_();
    source_id = "BLE " + this->ble_address_();
  }
#endif

  if (!use_ble) {
    // Query CCA device info over HTTP if not cached or stale
    if (parent_->get_cca_device_info().empty() ||
        parent_->get_last_cca_sync_time() == 0) {
      parent_->query_cca_device_info();
    }
    device_info = parent_->get_cca_device_info();
    unsigned long last_sync = parent_->get_last_cca_sync_time();
    if (last_sync > 0) {
      seconds_ago = (millis() - last_sync) / 1000;
    }
  }

  json.append("{\"cca_ip\":\"");
  json.append(source_id.c_str());
  json.append("\",\"last_sync_seconds_ago\":");

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", seconds_ago);
  json.append(buf);
  // Embed a JSON document as an escaped string value.
  auto append_escaped = [&json](const std::string &doc) {
    if (doc.empty()) { json.append("{}"); return; }
    for (char c : doc) {
      if (c == '"') json.append("\\\"");
      else if (c == '\\') json.append("\\\\");
      else if (c == '\n') json.append("\\n");
      else if (c == '\r') json.append("\\r");
      else if (c == '\t') json.append("\\t");
      else {
        char ch[2] = {c, '\0'};
        json.append(ch);
      }
    }
  };

  json.append(",\"device_info\":\"");
  append_escaped(device_info);

  // network_info: only populated over BLE (NETWORK_INFO), {} otherwise.
  std::string network_info;
#ifdef USE_TIGO_CCA_BLE
  if (use_ble) network_info = this->ble_get_network_json_();
#endif
  json.append("\",\"network_info\":\"");
  append_escaped(network_info);

  json.append("\"}");
}

esp_err_t TigoWebServer::api_backlight_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
#ifdef USE_LIGHT
  if (server->backlight_ == nullptr) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Backlight not configured\"}");
    return ESP_OK;
  }
  
  // Read POST body
  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid request\"}");
    return ESP_OK;
  }
  buf[ret] = '\0';
  
  // Parse simple state=on|off or state=toggle
  bool turn_on = false;
  bool toggle = false;
  
  if (strstr(buf, "state=on")) {
    turn_on = true;
  } else if (strstr(buf, "state=off")) {
    turn_on = false;
  } else if (strstr(buf, "state=toggle")) {
    toggle = true;
    // Get current state
    auto current_values = server->backlight_->current_values;
    turn_on = current_values.get_state() == 0.0f;  // If off, turn on
  } else {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid state parameter\"}");
    return ESP_OK;
  }
  
  // Make the light call
  auto call = server->backlight_->make_call();
  call.set_state(turn_on);
  call.perform();
  
  // Return success
  httpd_resp_set_type(req, "application/json");
  const char *response = turn_on ? 
    "{\"success\":true,\"state\":\"on\"}" : 
    "{\"success\":true,\"state\":\"off\"}";
  httpd_resp_sendstr(req, response);
#else
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Light component not available\"}");
#endif
  
  return ESP_OK;
}

esp_err_t TigoWebServer::api_github_release_handler(httpd_req_t *req) {
  // This endpoint returns GitHub API URL for client-side fetch
  // Client will check for new releases directly from browser

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600"); // Cache for 1 hour

  const char *response = R"({"fetch_url":"https://api.github.com/repos/RAR/esphome-tigomonitor/releases/latest"})";

  httpd_resp_sendstr(req, response);
  return ESP_OK;
}

#ifdef TIGO_TSDB_AVAILABLE
esp_err_t TigoWebServer::api_history_power_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req))
    return ESP_OK;

  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "{\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }
  tigo_monitor::TigoHistory *hist = server->parent_->get_history();
  if (hist == nullptr || !hist->initialized()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"history not initialized\"}");
    return ESP_OK;
  }

  // Parse range. Supported: day | week | month | year. Default day.
  uint32_t window_seconds = 24 * 3600;
  const char *range_label = "day";
  char query_buf[64] = {0};
  if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
    char range[16] = {0};
    if (httpd_query_key_value(query_buf, "range", range, sizeof(range)) == ESP_OK) {
      if (strcmp(range, "week") == 0) {
        window_seconds = 7UL * 24 * 3600;
        range_label = "week";
      } else if (strcmp(range, "month") == 0) {
        window_seconds = 30UL * 24 * 3600;
        range_label = "month";
      } else if (strcmp(range, "year") == 0) {
        window_seconds = 365UL * 24 * 3600;
        range_label = "year";
      } else if (strcmp(range, "day") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"range must be day|week|month|year\"}");
        return ESP_OK;
      }
    }
  }

  uint32_t now_ts = (uint32_t) ::time(nullptr);
  if (now_ts < 1577836800u /* 2020-01-01 */) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"system clock not set\"}");
    return ESP_OK;
  }
  uint32_t start_ts = (now_ts > window_seconds) ? (now_ts - window_seconds) : 0;

  PSRAMString json;
  char tmp[96];
  json.append("{\"range\":\"");
  json.append(range_label);
  json.append("\",\"start\":");
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long) start_ts);
  json.append(tmp);
  json.append(",\"end\":");
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long) now_ts);
  json.append(tmp);
  json.append(",\"records\":[");

  bool first = true;
  // Hard cap response to ~1 MB; runaway query (corrupt index, huge range) cant exhaust heap.
  constexpr size_t kMaxBytes = 1u << 20;
  uint32_t t0_ms = (uint32_t) (esp_timer_get_time() / 1000);
  int n = hist->iterate_power(start_ts, now_ts,
      [&](uint32_t ts, int16_t total_p, int16_t total_e_x100) {
        if (json.length() > kMaxBytes)
          return;  // we still get called, just skip emit
        if (!first)
          json.append(",");
        first = false;
        char row[64];
        snprintf(row, sizeof(row), "{\"t\":%lu,\"p\":%d,\"e\":%.2f}",
                 (unsigned long) ts, (int) total_p, total_e_x100 / 100.0f);
        json.append(row);
      });
  uint32_t dt_ms = (uint32_t) (esp_timer_get_time() / 1000) - t0_ms;

  json.append("],\"count\":");
  snprintf(tmp, sizeof(tmp), "%d", (n < 0) ? 0 : n);
  json.append(tmp);
  json.append(",\"query_ms\":");
  snprintf(tmp, sizeof(tmp), "%u", (unsigned) dt_ms);
  json.append(tmp);
  if (n < 0)
    json.append(",\"error\":\"query failed\"");
  json.append("}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_history_panel_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req))
    return ESP_OK;

  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "{\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }
  tigo_monitor::TigoHistory *hist = server->parent_->get_history();
  if (hist == nullptr || !hist->initialized()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"history not initialized\"}");
    return ESP_OK;
  }

  // Parse slot (required) + range (optional, default day).
  int slot_int = -1;
  uint32_t window_seconds = 24 * 3600;
  const char *range_label = "day";
  char query_buf[64] = {0};
  if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
    char slot_str[8] = {0};
    if (httpd_query_key_value(query_buf, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
      slot_int = atoi(slot_str);
    }
    char range[16] = {0};
    if (httpd_query_key_value(query_buf, "range", range, sizeof(range)) == ESP_OK) {
      if (strcmp(range, "week") == 0) {
        window_seconds = 7UL * 24 * 3600;
        range_label = "week";
      } else if (strcmp(range, "month") == 0) {
        window_seconds = 30UL * 24 * 3600;
        range_label = "month";
      } else if (strcmp(range, "year") == 0) {
        window_seconds = 365UL * 24 * 3600;
        range_label = "year";
      } else if (strcmp(range, "day") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"range must be day|week|month|year\"}");
        return ESP_OK;
      }
    }
  }

  if (slot_int < 0 || slot_int >= (int) tigo_monitor::kMaxPanelSlots) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"slot must be 0..47\"}");
    return ESP_OK;
  }

  uint32_t now_ts = (uint32_t) ::time(nullptr);
  if (now_ts < 1577836800u /* 2020-01-01 */) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"system clock not set\"}");
    return ESP_OK;
  }
  uint32_t start_ts = (now_ts > window_seconds) ? (now_ts - window_seconds) : 0;

  PSRAMString json;
  char tmp[96];
  json.append("{\"slot\":");
  snprintf(tmp, sizeof(tmp), "%d", slot_int);
  json.append(tmp);
  json.append(",\"range\":\"");
  json.append(range_label);
  json.append("\",\"start\":");
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long) start_ts);
  json.append(tmp);
  json.append(",\"end\":");
  snprintf(tmp, sizeof(tmp), "%lu", (unsigned long) now_ts);
  json.append(tmp);
  json.append(",\"records\":[");

  bool first = true;
  constexpr size_t kMaxBytes = 1u << 20;
  uint32_t t0_ms = (uint32_t) (esp_timer_get_time() / 1000);
  int n = hist->iterate_panel((uint8_t) slot_int, start_ts, now_ts,
      [&](uint32_t ts, int16_t power_w) {
        if (json.length() > kMaxBytes)
          return;
        if (!first)
          json.append(",");
        first = false;
        char row[48];
        snprintf(row, sizeof(row), "{\"t\":%lu,\"p\":%d}",
                 (unsigned long) ts, (int) power_w);
        json.append(row);
      });
  uint32_t dt_ms = (uint32_t) (esp_timer_get_time() / 1000) - t0_ms;

  json.append("],\"count\":");
  snprintf(tmp, sizeof(tmp), "%d", (n < 0) ? 0 : n);
  json.append(tmp);
  json.append(",\"query_ms\":");
  snprintf(tmp, sizeof(tmp), "%u", (unsigned) dt_ms);
  json.append(tmp);
  if (n < 0)
    json.append(",\"error\":\"query failed\"");
  json.append("}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_panels_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req))
    return ESP_OK;

  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "{\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }
  tigo_monitor::TigoHistory *hist = server->parent_->get_history();
  if (hist == nullptr || !hist->initialized()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"history not initialized\"}");
    return ESP_OK;
  }

  // Build a barcode-suffix -> NodeTableData lookup so we can attach friendly
  // names from CCA metadata to each slot. node_table_ keys are 16-char
  // long_addresses; the slot map keys on the last 6 chars of the barcode.
  auto nodes = server->parent_->snapshot_node_table();
  std::unordered_map<std::string, const tigo_monitor::NodeTableData *> by_suffix;
  for (const auto &n : nodes) {
    if (n.long_address.size() >= 6) {
      by_suffix[n.long_address.substr(n.long_address.size() - 6)] = &n;
    }
  }

  std::vector<tigo_monitor::PanelSlot> slots = hist->snapshot_slot_map();

  PSRAMString json;
  json.append("{\"slots\":[");
  bool first = true;
  for (const auto &s : slots) {
    if (!first) json.append(",");
    first = false;
    char row[256];
    const tigo_monitor::NodeTableData *node = nullptr;
    auto it = by_suffix.find(s.barcode_last6);
    if (it != by_suffix.end()) node = it->second;

    // CCA labels can contain quotes/backslashes — escape them. Most installs
    // won't, but better to be defensive than to break the parse.
    auto json_escape = [](const std::string &in) {
      std::string out;
      out.reserve(in.size());
      for (char c : in) {
        if (c == '"' || c == '\\') {
          out.push_back('\\');
          out.push_back(c);
        } else if ((unsigned char) c < 0x20) {
          // Drop control chars rather than emit \u escapes.
          continue;
        } else {
          out.push_back(c);
        }
      }
      return out;
    };

    snprintf(row, sizeof(row),
             "{\"slot\":%u,\"barcode\":\"%s\"",
             (unsigned) s.slot, s.barcode_last6.c_str());
    json.append(row);
    if (node != nullptr) {
      std::string label = json_escape(node->cca_label);
      std::string mppt = json_escape(node->cca_inverter_label);
      std::string str_lbl = json_escape(node->cca_string_label);
      json.append(",\"label\":\"");
      json.append(label.c_str());
      json.append("\",\"mppt\":\"");
      json.append(mppt.c_str());
      json.append("\",\"string\":\"");
      json.append(str_lbl.c_str());
      json.append("\"");
    }
    json.append("}");
  }
  json.append("],\"count\":");
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%zu", slots.size());
  json.append(tmp);
  json.append(",\"max_slots\":");
  snprintf(tmp, sizeof(tmp), "%zu", tigo_monitor::kMaxPanelSlots);
  json.append(tmp);
  json.append("}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_tsdb_stats_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  if (!server->check_api_auth(req)) return ESP_OK;

  if (server->parent_ == nullptr) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "{\"error\":\"monitor not bound\"}");
    return ESP_OK;
  }
  tigo_monitor::TigoHistory *hist = server->parent_->get_history();
  if (hist == nullptr || !hist->initialized()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"history not initialized\"}");
    return ESP_OK;
  }

  PSRAMString json;
  char buf[160];

  // LittleFS partition info — denominator for "how full is /tsdb".
  size_t fs_total = 0, fs_used = 0;
  esp_littlefs_info("tsdb", &fs_total, &fs_used);

  json.append("{\"littlefs\":{\"total\":");
  snprintf(buf, sizeof(buf), "%zu", fs_total); json.append(buf);
  json.append(",\"used\":");
  snprintf(buf, sizeof(buf), "%zu", fs_used); json.append(buf);
  json.append("},\"slots\":{\"used\":");
  snprintf(buf, sizeof(buf), "%zu", hist->slot_count()); json.append(buf);
  json.append(",\"next_free\":");
  snprintf(buf, sizeof(buf), "%u", (unsigned) hist->next_free_slot()); json.append(buf);
  json.append(",\"max\":");
  snprintf(buf, sizeof(buf), "%zu", tigo_monitor::kMaxPanelSlots); json.append(buf);
  json.append("},\"databases\":[");

  // Per-DB stats. tsdb_get_stats_h returns ESP_ERR_INVALID_STATE for nullptr,
  // so we skip lazy-unopened panel DBs (which is the right behavior — they
  // have nothing to report yet).
  auto append_db = [&](const char *label, tsdb_t *db, bool first) {
    if (db == nullptr) return;
    tsdb_stats_t stats = {};
    if (tsdb_get_stats_h(db, &stats) != ESP_OK) return;
    if (!first) json.append(",");
    json.append("{\"label\":\"");
    json.append(label);
    json.append("\",\"records\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.total_records); json.append(buf);
    json.append(",\"max_records\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.max_records); json.append(buf);
    json.append(",\"writes\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.total_writes); json.append(buf);
    json.append(",\"evictions\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.total_evictions); json.append(buf);
    json.append(",\"oldest_ts\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.oldest_timestamp); json.append(buf);
    json.append(",\"newest_ts\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.newest_timestamp); json.append(buf);
    json.append(",\"size_bytes\":");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long) stats.storage_bytes); json.append(buf);
    json.append(",\"params\":");
    snprintf(buf, sizeof(buf), "%u", (unsigned) stats.num_params); json.append(buf);
    json.append("}");
  };

  bool first = true;
  if (hist->system_db() != nullptr) {
    append_db("system", hist->system_db(), first);
    first = false;
  }
  for (size_t i = 0; i < hist->panel_db_count(); ++i) {
    tsdb_t *db = hist->panel_db(i);
    if (db == nullptr) continue;
    char label[16];
    snprintf(label, sizeof(label), "panels%zu", i);
    append_db(label, db, first);
    first = false;
  }

  json.append("]}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}
#endif  // TIGO_TSDB_AVAILABLE

void TigoWebServer::loop() {
#ifdef USE_TIGO_CCA_BLE
  this->ble_loop_();
#endif
}

TigoWebServer::~TigoWebServer() {
  // Clean up temperature sensor if it was initialized
  if (temp_sensor_handle_ != nullptr) {
    temperature_sensor_disable(temp_sensor_handle_);
    temperature_sensor_uninstall(temp_sensor_handle_);
    temp_sensor_handle_ = nullptr;
  }
}

}  // namespace tigo_server
}  // namespace esphome

#endif  // USE_ESP_IDF

