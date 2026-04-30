#include "tigo_web_server.h"
#include "web_assets.h"  // Auto-generated from components/tigo_server/web/*.html

#ifdef USE_ESP_IDF

#include "esphome/core/application.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"
#include "esphome/core/time.h"
#include "esphome/components/network/util.h"
#include "esphome/components/logger/logger.h"
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
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <cstring>
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
  config.max_uri_handlers = 30;  // log + history endpoints + /history HTML page
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
#endif

    // Log web authentication status
    if (!web_username_.empty() && !web_password_.empty()) {
      ESP_LOGI(TAG, "HTTP Basic Authentication configured for web pages (user: %s)", web_username_.c_str());
    } else {
      ESP_LOGI(TAG, "Web authentication not configured - pages remain open");
    }
    
    // Initialize temperature sensor once at startup
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
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
      ESP_LOGW(TAG, "Failed to install temperature sensor: %s", esp_err_to_name(err));
      temp_sensor_handle_ = nullptr;
    }
    
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
  // Simple SVG favicon - solar panel icon
  const char* favicon_svg = 
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
    "<rect x='20' y='30' width='60' height='40' fill='#3498db' stroke='#2c3e50' stroke-width='2'/>"
    "<line x1='20' y1='50' x2='80' y2='50' stroke='#2c3e50' stroke-width='2'/>"
    "<line x1='50' y1='30' x2='50' y2='70' stroke='#2c3e50' stroke-width='2'/>"
    "<path d='M 40 70 L 35 85 L 65 85 L 60 70' fill='#95a5a6' stroke='#2c3e50' stroke-width='2'/>"
    "</svg>";
  
  httpd_resp_set_type(req, "image/svg+xml");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  httpd_resp_send(req, favicon_svg, strlen(favicon_svg));
  return ESP_OK;
}

esp_err_t TigoWebServer::dashboard_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Check web authentication
  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }
  
  // Use PSRAM for large HTML content
  PSRAMString html;
  server->get_dashboard_html(html);
  
  httpd_resp_set_type(req, "text/html");
  
  // Send in chunks to avoid internal RAM buffering
  const char* data = html.c_str();
  size_t len = html.length();
  const size_t chunk_size = 4096;
  size_t sent = 0;
  
  while (sent < len) {
    size_t to_send = (len - sent > chunk_size) ? chunk_size : (len - sent);
    if (httpd_resp_send_chunk(req, data + sent, to_send) != ESP_OK) {
      return ESP_FAIL;
    }
    sent += to_send;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  
  return ESP_OK;
}

esp_err_t TigoWebServer::node_table_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Check web authentication
  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString html;
  server->get_node_table_html(html);
  
  httpd_resp_set_type(req, "text/html");
  
  // Send in chunks to avoid internal RAM buffering
  const char* data = html.c_str();
  size_t len = html.length();
  const size_t chunk_size = 4096;
  size_t sent = 0;
  
  while (sent < len) {
    size_t to_send = (len - sent > chunk_size) ? chunk_size : (len - sent);
    if (httpd_resp_send_chunk(req, data + sent, to_send) != ESP_OK) {
      return ESP_FAIL;
    }
    sent += to_send;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  
  return ESP_OK;
}

esp_err_t TigoWebServer::esp_status_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Check web authentication
  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString html;
  server->get_esp_status_html(html);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Connection", "close");
  esp_err_t result = httpd_resp_send(req, html.c_str(), html.length());
  
  return result;
}

esp_err_t TigoWebServer::history_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);

  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }

  PSRAMString html;
  server->get_history_html(html);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Connection", "close");
  return httpd_resp_send(req, html.c_str(), html.length());
}

esp_err_t TigoWebServer::yaml_config_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Check web authentication
  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString html;
  server->get_yaml_config_html(html);
  
  httpd_resp_set_type(req, "text/html");
  
  // Send in chunks to avoid internal RAM buffering
  const char* data = html.c_str();
  size_t len = html.length();
  const size_t chunk_size = 4096;
  size_t sent = 0;
  
  while (sent < len) {
    size_t to_send = (len - sent > chunk_size) ? chunk_size : (len - sent);
    if (httpd_resp_send_chunk(req, data + sent, to_send) != ESP_OK) {
      return ESP_FAIL;
    }
    sent += to_send;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  
  return ESP_OK;
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
  }
  
  // If no sensors specified, use default set
  if (selected_sensors.empty()) {
    selected_sensors = {"power_in", "peak_power", "voltage_in", "voltage_out", "current_in", "current_out", "power_out", "temperature", "rssi"};
  }
  
  PSRAMString json_buffer;
  server->build_yaml_json(json_buffer, selected_sensors, selected_hub_sensors);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::cca_info_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Check web authentication
  if (!server->check_web_auth(req)) {
    return ESP_OK;
  }
  
  PSRAMString html;
  server->get_cca_info_html(html);
  
  httpd_resp_set_type(req, "text/html");
  
  // Send in chunks to avoid internal RAM buffering
  const char* data = html.c_str();
  size_t len = html.length();
  const size_t chunk_size = 4096;
  size_t sent = 0;
  
  while (sent < len) {
    size_t to_send = (len - sent > chunk_size) ? chunk_size : (len - sent);
    if (httpd_resp_send_chunk(req, data + sent, to_send) != ESP_OK) {
      return ESP_FAIL;
    }
    sent += to_send;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  
  return ESP_OK;
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
  
  // Trigger full CCA refresh (device info + config sync with proper sequencing)
  server->parent_->refresh_cca_data();
  
  // Return simple success response
  const char* response = "{\"status\":\"ok\",\"message\":\"CCA refresh initiated\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, response, strlen(response));
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
  
  const auto devices = parent_->snapshot_devices();
  const auto node_table = parent_->snapshot_node_table();

  // Create a vector of device info with names and sensor indices for sorting
  struct DeviceWithName {
    const tigo_monitor::DeviceData *device;  // May be nullptr if no runtime data yet
    std::string addr;
    std::string barcode;
    std::string name;
    std::string string_label;
    int sensor_index;
    bool has_runtime_data;
  };
  
  std::vector<DeviceWithName> sorted_devices;
  
  // First, add all devices that have runtime data
  for (const auto &device : devices) {
    DeviceWithName dwn;
    dwn.device = &device;
    dwn.addr = device.addr;
    dwn.barcode = device.barcode;
    dwn.sensor_index = -1;
    dwn.string_label = "";
    dwn.has_runtime_data = true;
    
    // Find the node table entry to get CCA label, string label, and sensor index
    for (const auto &node : node_table) {
      if (node.addr == device.addr) {
        dwn.sensor_index = node.sensor_index;
        dwn.string_label = node.cca_string_label;
        if (!node.cca_label.empty()) {
          dwn.name = node.cca_label;
        }
        break;
      }
    }
    
    // Fallback to barcode or address if no CCA label
    if (dwn.name.empty()) {
      if (!device.barcode.empty() && device.barcode.length() >= 5) {
        dwn.name = device.barcode;
      } else {
        dwn.name = "Module " + device.addr;
      }
    }
    
    sorted_devices.push_back(dwn);
  }
  
  // Now add nodes from node_table that don't have runtime data yet
  // This handles the case where ESP32 restarted at night - we know about them but haven't seen them
  for (const auto &node : node_table) {
    // Check if this node already exists in sorted_devices
    bool found = false;
    for (const auto &existing : sorted_devices) {
      if (existing.addr == node.addr) {
        found = true;
        break;
      }
    }
    
    if (!found && node.sensor_index >= 0) {
      // This node is in the table but has no runtime data yet
      DeviceWithName dwn;
      dwn.device = nullptr;
      dwn.addr = node.addr;
      // Use Frame 27 long_address (16-char) as barcode
      dwn.barcode = node.long_address;
      dwn.sensor_index = node.sensor_index;
      dwn.string_label = node.cca_string_label;
      dwn.has_runtime_data = false;
      
      // Use CCA label if available, otherwise barcode, otherwise address
      if (!node.cca_label.empty()) {
        dwn.name = node.cca_label;
      } else if (!dwn.barcode.empty() && dwn.barcode.length() >= 5) {
        dwn.name = dwn.barcode;
      } else {
        dwn.name = "Module " + node.addr;
      }
      
      sorted_devices.push_back(dwn);
    }
  }
  
  // Sort by name (CCA label if available), then by sensor index
  std::sort(sorted_devices.begin(), sorted_devices.end(),
            [](const DeviceWithName &a, const DeviceWithName &b) {
              // If both have CCA labels or both don't, sort by name
              bool a_has_cca = (a.name.find("Module ") != 0 && a.name.length() != 16);
              bool b_has_cca = (b.name.find("Module ") != 0 && b.name.length() != 16);
              
              if (a_has_cca && b_has_cca) {
                // Both have CCA names - sort alphabetically
                return a.name < b.name;
              } else if (!a_has_cca && !b_has_cca) {
                // Neither has CCA name - sort by sensor index
                return a.sensor_index < b.sensor_index;
              } else {
                // CCA names come before non-CCA names
                return a_has_cca;
              }
            });
  
  bool first = true;
  
  for (const auto &dwn : sorted_devices) {
    if (!first) json.append(",");
    first = false;
    
    const std::string &device_name = dwn.name;
    const std::string &string_label = dwn.string_label;
    
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
        "\"duty_cycle\":%.1f,\"efficiency\":%.2f,\"data_age_ms\":%lu}",
        device.addr.c_str(), device.barcode.c_str(), device_name.c_str(), string_label.c_str(), device.voltage_in, device.voltage_out,
        device.current_in, device.current_out, device.power_in, device.power_out, device.power_out, device.peak_power, device.temperature, device.rssi,
        duty_cycle_percent, device.efficiency, data_age_ms);
    } else {
      // Device is known but has no runtime data yet (e.g., ESP32 restarted at night)
      // Show zeros with a very large data_age to indicate no recent data
      snprintf(buffer, sizeof(buffer),
        "{\"addr\":\"%s\",\"barcode\":\"%s\",\"name\":\"%s\",\"string_label\":\"%s\",\"voltage_in\":0.00,\"voltage_out\":0.00,"
        "\"current\":0.000,\"current_out\":0.000,\"power_in\":0.0,\"power\":0.0,\"power_out\":0.0,\"peak_power\":0.0,\"temperature\":0.0,\"rssi\":0,"
        "\"duty_cycle\":0.0,\"efficiency\":0.00,\"data_age_ms\":999999999}",
        dwn.addr.c_str(), dwn.barcode.c_str(), device_name.c_str(), string_label.c_str());
    }
    
    json.append(buffer);
  }
  
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
  
  const auto devices = parent_->snapshot_devices();

  float total_power_out = 0.0f;
  float total_current = 0.0f;
  float avg_efficiency = 0.0f;
  float avg_temp = 0.0f;
  int active_devices = 0;
  
  for (const auto &device : devices) {
    total_power_out += device.power_out;
    total_current += device.current_in;
    avg_efficiency += device.efficiency;
    avg_temp += device.temperature;
    active_devices++;
  }
  
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
  const auto strings = parent_->snapshot_strings();

  ESP_LOGD(TAG, "Building strings JSON - found %d strings", strings.size());
  
  json.append("{\"strings\":[");
  
  bool first = true;
  
  for (const auto &pair : strings) {
    if (!first) json.append(",");
    first = false;
    
    const auto &string_data = pair.second;
    
    ESP_LOGD(TAG, "String: %s, devices: %d/%d, power: %.0fW", 
             string_data.string_label.c_str(), 
             string_data.active_device_count, 
             string_data.total_device_count,
             string_data.total_power);
    
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
      "{\"label\":\"%s\",\"inverter\":\"%s\",\"total_power\":%.1f,\"peak_power\":%.1f,"
      "\"total_current\":%.3f,\"avg_voltage_in\":%.2f,\"avg_voltage_out\":%.2f,"
      "\"avg_temperature\":%.1f,\"avg_efficiency\":%.2f,\"min_efficiency\":%.2f,"
      "\"max_efficiency\":%.2f,\"active_devices\":%d,\"total_devices\":%d}",
      string_data.string_label.c_str(), string_data.inverter_label.c_str(),
      string_data.total_power, string_data.peak_power, string_data.total_current,
      string_data.avg_voltage_in, string_data.avg_voltage_out,
      string_data.avg_temperature, string_data.avg_efficiency,
      string_data.min_efficiency, string_data.max_efficiency,
      string_data.active_device_count, string_data.total_device_count);
    
    json.append(buffer);
  }
  
  json.append("]}");
}

void TigoWebServer::build_inverters_json(PSRAMString& json) {
  const auto inverters = parent_->snapshot_inverters();
  const auto strings = parent_->snapshot_strings();

  ESP_LOGD(TAG, "Building inverters JSON - found %d inverters", inverters.size());
  
  json.append("{\"inverters\":[");
  
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
          
          char buffer[512];
          snprintf(buffer, sizeof(buffer),
            "{\"label\":\"%s\",\"mppt\":\"%s\",\"total_power\":%.1f,\"peak_power\":%.1f,"
            "\"active_devices\":%d,\"total_devices\":%d}",
            string_data.string_label.c_str(), string_data.inverter_label.c_str(),
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

  const auto node_table = parent_->snapshot_node_table();

  for (const auto &node : node_table) {
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
  
  // Get ESP32 internal temperature (using persistent sensor handle)
  float internal_temp = 0.0f;
  if (temp_sensor_handle_ != nullptr) {
    temperature_sensor_get_celsius(temp_sensor_handle_, &internal_temp);
  }
  
  // Get network stats
  bool network_connected = network::is_connected();
  int8_t wifi_rssi = 0;
  std::string ip_address = "N/A";
  std::string mac_address = "N/A";
  std::string ssid = "N/A";
  
#ifdef USE_WIFI
  if (network_connected && wifi::global_wifi_component != nullptr) {
    wifi_rssi = wifi::global_wifi_component->wifi_rssi();
    char ssid_buf[wifi::SSID_BUFFER_SIZE];
    ssid = wifi::global_wifi_component->wifi_ssid_to(ssid_buf);
    
    // Get IP address - use the newer get_ip_addresses() method
    auto addresses = wifi::global_wifi_component->get_ip_addresses();
    if (!addresses.empty() && addresses[0].is_set()) {
      char buf[20];
      addresses[0].str_to(buf);
      ip_address = buf;
    }
    
    // Get MAC address via global function
    mac_address = get_mac_address_pretty();
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
  
  char buffer[1536];
  snprintf(buffer, sizeof(buffer),
    "{\"free_heap\":%zu,\"total_heap\":%zu,\"free_psram\":%zu,\"total_psram\":%zu,"
    "\"min_free_heap\":%zu,\"min_free_psram\":%zu,"
    "\"uptime_sec\":%u,\"uptime_days\":%u,\"uptime_hours\":%u,\"uptime_mins\":%u,"
    "\"esphome_version\":\"%s\",\"compilation_time\":\"%s %s\","
    "\"task_count\":%u,\"internal_temp\":%.1f,"
    "\"invalid_checksum\":%u,\"missed_frames\":%u,\"total_frames\":%u,"
    "\"command_frames\":%u,\"frame_27_count\":%u,"
    "\"network_connected\":%s,\"wifi_rssi\":%d,\"wifi_ssid\":\"%s\",\"ip_address\":\"%s\",\"mac_address\":\"%s\","
    "\"active_sockets\":%d,\"max_sockets\":%d}",
    free_heap, total_heap, free_psram, total_psram,
    min_free_heap, min_free_psram,
    uptime_sec, uptime_days, uptime_hours, uptime_mins,
    ESPHOME_VERSION, __DATE__, __TIME__,
    (unsigned int)task_count, internal_temp,
    invalid_checksum, missed_frames, total_frames,
    command_frames, frame_27_count,
    network_connected ? "true" : "false", wifi_rssi, ssid.c_str(), ip_address.c_str(), mac_address.c_str(),
    active_sockets, max_sockets);
  
  json.append(buffer);
}

void TigoWebServer::build_yaml_json(PSRAMString& json, const std::set<std::string>& selected_sensors, const std::set<std::string>& selected_hub_sensors) {
  PSRAMString yaml_text;
  const auto node_table = parent_->snapshot_node_table();

  // Build YAML configuration
  std::vector<tigo_monitor::NodeTableData> assigned_nodes;
  for (const auto &node : node_table) {
    if (node.sensor_index >= 0) {
      assigned_nodes.push_back(node);
    }
  }
  
  // Sort by sensor index
  std::sort(assigned_nodes.begin(), assigned_nodes.end(),
            [](const auto &a, const auto &b) { return a.sensor_index < b.sensor_index; });
  
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
  for (const auto &node : assigned_nodes) {
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

void TigoWebServer::get_dashboard_html(PSRAMString& html) {
  html.append(DASHBOARD_HTML_PRE);
  html.append(api_token_);
  html.append(DASHBOARD_HTML_POST);
}

void TigoWebServer::get_node_table_html(PSRAMString& html) {
  html.append(NODES_HTML_PRE);
  html.append(api_token_);
  html.append(NODES_HTML_POST);
}

void TigoWebServer::get_esp_status_html(PSRAMString& html) {
  html.append(STATUS_HTML_PRE);
  html.append(api_token_);
  html.append(STATUS_HTML_POST);
}

void TigoWebServer::get_yaml_config_html(PSRAMString& html) {
  html.append(YAML_HTML_PRE);
  html.append(api_token_);
  html.append(YAML_HTML_POST);
}

void TigoWebServer::get_cca_info_html(PSRAMString& html) {
  html.append(CCA_HTML_PRE);
  html.append(api_token_);
  html.append(CCA_HTML_POST);
}

void TigoWebServer::get_history_html(PSRAMString& html) {
  html.append(HISTORY_HTML_PRE);
  html.append(api_token_);
  html.append(HISTORY_HTML_POST);
}

void TigoWebServer::build_cca_info_json(PSRAMString& json) {
  // Query CCA device info if not cached or stale
  if (parent_->get_cca_device_info().empty() || 
      parent_->get_last_cca_sync_time() == 0) {
    parent_->query_cca_device_info();
  }
  
  // Calculate seconds since last sync (ESP32 millis() to seconds)
  unsigned long last_sync = parent_->get_last_cca_sync_time();
  unsigned long seconds_ago = 0;
  if (last_sync > 0) {
    seconds_ago = (millis() - last_sync) / 1000;
  }
  
  json.append("{\"cca_ip\":\"");
  json.append(parent_->get_cca_ip().c_str());
  json.append("\",\"last_sync_seconds_ago\":");
  
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", seconds_ago);
  json.append(buf);
  json.append(",\"device_info\":\"");
  
  // Embed the device info JSON (already a JSON string)
  const std::string &device_info = parent_->get_cca_device_info();
  if (device_info.empty()) {
    json.append("{}");
  } else {
    // Escape the JSON string for embedding
    for (char c : device_info) {
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
  }
  
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
#endif  // TIGO_TSDB_AVAILABLE

void TigoWebServer::loop() {
  // Nothing to do in loop
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

