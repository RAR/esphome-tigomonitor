#include "tigo_web_server.h"

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
  config.max_uri_handlers = 25;  // Increased for log endpoints
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
    selected_sensors = {"power", "peak_power", "voltage_in", "voltage_out", "current_in", "temperature", "rssi"};
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
  
  const auto &devices = parent_->get_devices();
  const auto &node_table = parent_->get_node_table();
  
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
      float power = device.voltage_out * device.current_in * parent_->get_power_calibration();
      // If last_update is 0, device hasn't been updated yet - use ULONG_MAX to indicate "never"
      unsigned long data_age_ms = (device.last_update == 0) ? ULONG_MAX : (millis() - device.last_update);
      float duty_cycle_percent = (device.duty_cycle / 255.0f) * 100.0f;
      
      snprintf(buffer, sizeof(buffer),
        "{\"addr\":\"%s\",\"barcode\":\"%s\",\"name\":\"%s\",\"string_label\":\"%s\",\"voltage_in\":%.2f,\"voltage_out\":%.2f,"
        "\"current\":%.3f,\"power\":%.1f,\"peak_power\":%.1f,\"temperature\":%.1f,\"rssi\":%d,"
        "\"duty_cycle\":%.1f,\"efficiency\":%.2f,\"data_age_ms\":%lu}",
        device.addr.c_str(), device.barcode.c_str(), device_name.c_str(), string_label.c_str(), device.voltage_in, device.voltage_out,
        device.current_in, power, device.peak_power, device.temperature, device.rssi,
        duty_cycle_percent, device.efficiency, data_age_ms);
    } else {
      // Device is known but has no runtime data yet (e.g., ESP32 restarted at night)
      // Show zeros with a very large data_age to indicate no recent data
      snprintf(buffer, sizeof(buffer),
        "{\"addr\":\"%s\",\"barcode\":\"%s\",\"name\":\"%s\",\"string_label\":\"%s\",\"voltage_in\":0.00,\"voltage_out\":0.00,"
        "\"current\":0.000,\"power\":0.0,\"peak_power\":0.0,\"temperature\":0.0,\"rssi\":0,"
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
      "\"avg_temperature\":%.1f,\"active_devices\":%d,\"max_devices\":%d,\"total_energy\":%.3f}",
      0.0f, 0.0f, 0.0f, 0.0f,
      0, parent_->get_number_of_devices(), parent_->get_total_energy_kwh());
    
    json.append(buffer);
    return;
  }
  
  const auto &devices = parent_->get_devices();
  
  float total_power = 0.0f;
  float total_current = 0.0f;
  float avg_efficiency = 0.0f;
  float avg_temp = 0.0f;
  int active_devices = 0;
  
  for (const auto &device : devices) {
    float power = device.voltage_out * device.current_in * parent_->get_power_calibration();
    total_power += power;
    total_current += device.current_in;
    avg_efficiency += device.efficiency;
    avg_temp += device.temperature;
    active_devices++;
  }
  
  if (active_devices > 0) {
    avg_efficiency /= active_devices;
    avg_temp /= active_devices;
  }
  
  float total_energy = parent_->get_total_energy_kwh();
  
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
    "{\"total_power\":%.1f,\"total_current\":%.3f,\"avg_efficiency\":%.2f,"
    "\"avg_temperature\":%.1f,\"active_devices\":%d,\"max_devices\":%d,\"total_energy\":%.3f}",
    total_power, total_current, avg_efficiency, avg_temp,
    active_devices, parent_->get_number_of_devices(), total_energy);
  
  json.append(buffer);
}

void TigoWebServer::build_strings_json(PSRAMString& json) {
  const auto &strings = parent_->get_strings();
  
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
  const auto &inverters = parent_->get_inverters();
  const auto &strings = parent_->get_strings();
  
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
  float current_energy = parent_->get_total_energy_kwh();
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
  
  const auto &node_table = parent_->get_node_table();
  
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
    ssid = wifi::global_wifi_component->wifi_ssid();
    
    // Get IP address - use the newer get_ip_addresses() method
    auto addresses = wifi::global_wifi_component->get_ip_addresses();
    if (!addresses.empty() && addresses[0].is_set()) {
      ip_address = addresses[0].str();
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
  const auto &node_table = parent_->get_node_table();
  
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
  if (!selected_hub_sensors.empty()) {
    yaml_text.append("  # Hub-level sensors (system-wide, no address required)\n");
    yaml_text.append("  - platform: tigo_monitor\n");
    yaml_text.append("    tigo_monitor_id: tigo_hub\n");
    
    if (selected_hub_sensors.count("power_sum") > 0) {
      yaml_text.append("    power_sum:\n");
      yaml_text.append("      name: \"Total Power\"\n");
    }
    if (selected_hub_sensors.count("energy_sum") > 0) {
      yaml_text.append("    energy_sum:\n");
      yaml_text.append("      name: \"Total Energy\"\n");
    }
    if (selected_hub_sensors.count("device_count") > 0) {
      yaml_text.append("    device_count:\n");
      yaml_text.append("      name: \"Device Count\"\n");
    }
    if (selected_hub_sensors.count("invalid_checksum") > 0) {
      yaml_text.append("    invalid_checksum:\n");
      yaml_text.append("      name: \"Invalid Checksum Count\"\n");
    }
    if (selected_hub_sensors.count("missed_frame") > 0) {
      yaml_text.append("    missed_frame:\n");
      yaml_text.append("      name: \"Missed Frame Count\"\n");
    }
    if (selected_hub_sensors.count("internal_ram_free") > 0) {
      yaml_text.append("    internal_ram_free:\n");
      yaml_text.append("      name: \"Free Internal RAM\"\n");
    }
    if (selected_hub_sensors.count("internal_ram_min") > 0) {
      yaml_text.append("    internal_ram_min:\n");
      yaml_text.append("      name: \"Min Free Internal RAM\"\n");
    }
    if (selected_hub_sensors.count("psram_free") > 0) {
      yaml_text.append("    psram_free:\n");
      yaml_text.append("      name: \"Free PSRAM\"\n");
    }
    if (selected_hub_sensors.count("stack_free") > 0) {
      yaml_text.append("    stack_free:\n");
      yaml_text.append("      name: \"Free Stack\"\n");
    }
    
    yaml_text.append("\n");
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
    if (selected_sensors.count("power") > 0) {
      yaml_text.append("    power: {}\n");
    }
    if (selected_sensors.count("peak_power") > 0) {
      yaml_text.append("    peak_power: {}\n");
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
  html.append(R"html(<!DOCTYPE html)
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - Dashboard</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    :root { --text-color: #2c3e50; --axis-color: #666; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .temp-toggle, .theme-toggle, .github-link { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-flex; align-items: center; }
    .temp-toggle:hover, .theme-toggle:hover, .github-link:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: all 0.2s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1400px; margin: 2rem auto; padding: 0 1rem; }
    .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; margin-bottom: 2rem; }
    .stat-card { background: white; padding: 1.5rem; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; }
    .stat-card h3 { font-size: 0.875rem; color: #7f8c8d; margin-bottom: 0.5rem; text-transform: uppercase; }
    .stat-card .value { font-size: 2rem; font-weight: bold; color: #2c3e50; }
    .stat-card .unit { font-size: 1rem; color: #95a5a6; margin-left: 0.25rem; }
    .devices-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(350px, 1fr)); gap: 1rem; }
    .string-summary { grid-column: 1 / -1; background: linear-gradient(135deg, #6b7280 0%, #4b5563 100%); border-radius: 8px; padding: 1.5rem; margin-top: 0rem; box-shadow: 0 4px 6px rgba(0,0,0,0.1); transition: box-shadow 0.3s; }
    .string-summary:first-child { margin-top: 0; }
    .string-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; }
    .string-header h3 { color: white; font-size: 1.5rem; margin: 0; }
    .string-inverter { background: rgba(255,255,255,0.2); color: white; padding: 0.5rem 1rem; border-radius: 20px; font-size: 0.875rem; font-weight: 600; }
    .string-stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 1rem; }
    .string-stat { background: rgba(255,255,255,0.15); padding: 1rem; border-radius: 8px; text-align: center; }
    .string-stat .stat-label { display: block; color: rgba(255,255,255,0.8); font-size: 0.75rem; text-transform: uppercase; margin-bottom: 0.5rem; }
    .string-stat .stat-value { display: block; color: white; font-size: 1.5rem; font-weight: bold; }
    .device-card { background: white; border-radius: 8px; padding: 1.5rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; }
    .device-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; border-bottom: 2px solid #ecf0f1; padding-bottom: 0.75rem; }
    .device-title-section { flex: 1; }
    .device-title { font-size: 1.125rem; font-weight: bold; color: #2c3e50; }
    .device-subtitle { font-size: 0.75rem; color: #7f8c8d; margin-top: 0.25rem; }
    .device-badge { background: #27ae60; color: white; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; }
    .device-metrics { display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.75rem; }
    .metric { display: flex; justify-content: space-between; padding: 0.5rem; background: #f8f9fa; border-radius: 4px; transition: background-color 0.3s; }
    .metric-label { color: #7f8c8d; font-size: 0.875rem; }
    .metric-value { font-weight: 600; color: #2c3e50; }
    .loading { text-align: center; padding: 2rem; color: #7f8c8d; }
    .error { background: #e74c3c; color: white; padding: 1rem; border-radius: 4px; margin-bottom: 1rem; }
    
    /* Release banner */
    .release-banner { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 1rem 2rem; display: none; align-items: center; justify-content: space-between; box-shadow: 0 2px 8px rgba(0,0,0,0.15); position: relative; }
    .release-banner-content { display: flex; align-items: center; gap: 1rem; flex: 1; }
    .release-banner-icon { font-size: 1.5rem; }
    .release-banner-text { flex: 1; }
    .release-banner-title { font-weight: 600; margin-bottom: 0.25rem; }
    .release-banner-message { font-size: 0.875rem; opacity: 0.9; }
    .release-banner-actions { display: flex; gap: 0.75rem; align-items: center; }
    .release-banner-btn { background: rgba(255,255,255,0.2); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-block; }
    .release-banner-btn:hover { background: rgba(255,255,255,0.3); }
    .release-banner-dismiss { background: transparent; border: none; color: white; font-size: 1.5rem; cursor: pointer; opacity: 0.8; padding: 0.25rem 0.5rem; transition: opacity 0.2s; }
    .release-banner-dismiss:hover { opacity: 1; }
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1c2833; }
    body.dark-mode .stat-card, body.dark-mode .device-card { background: #2d2d2d; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .stat-card h3, body.dark-mode .metric-label, body.dark-mode .device-subtitle { color: #b0b0b0; }
    body.dark-mode .stat-card .value, body.dark-mode .metric-value, body.dark-mode .device-title { color: #e0e0e0; }
    body.dark-mode .metric { background: #3a3a3a; }
    body.dark-mode .device-header { border-bottom-color: #444; }
    body.dark-mode .loading { color: #b0b0b0; }
    body.dark-mode .string-summary { box-shadow: 0 4px 6px rgba(0,0,0,0.5); }
    
    /* Energy history card */
    .energy-history-card { background: #ffffff; border-radius: 8px; padding: 20px; margin: 20px 0; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; color: #2c3e50; }
    .energy-history-card h2 { margin-top: 0; color: #2c3e50; }
    body.dark-mode .energy-history-card { background: #3a3a3a; box-shadow: 0 2px 4px rgba(0,0,0,0.3); color: #ffffff; }
    body.dark-mode .energy-history-card h2 { color: #ffffff; }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
        <a href="https://github.com/RAR/esphome-tigomonitor" target="_blank" class="github-link" title="View on GitHub">
          <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
        </a>
        <button class="temp-toggle" onclick="toggleTempUnit()" id="temp-toggle">°F</button>
        <button class="theme-toggle" onclick="toggleTheme()" id="theme-toggle">🌙</button>
      </div>
    </div>
    <div class="nav">
      <a href="/" class="active">Dashboard</a>
      <a href="/nodes">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="release-banner" id="release-banner">
    <div class="release-banner-content">
      <div class="release-banner-icon">🎉</div>
      <div class="release-banner-text">
        <div class="release-banner-title">New Release Available!</div>
        <div class="release-banner-message" id="release-message">A new version is available on GitHub</div>
      </div>
    </div>
    <div class="release-banner-actions">
      <a href="#" id="release-link" target="_blank" class="release-banner-btn">View Release</a>
      <button class="release-banner-dismiss" onclick="dismissReleaseBanner()" title="Dismiss">&times;</button>
    </div>
  </div>
  
  <div class="container">
    <div class="stats" id="stats">
      <div class="stat-card">
        <h3>Total Power</h3>
        <div><span class="value" id="total-power">--</span><span class="unit">W</span></div>
      </div>
      <div class="stat-card">
        <h3>Total Current</h3>
        <div><span class="value" id="total-current">--</span><span class="unit">A</span></div>
      </div>
      <div class="stat-card">
        <h3>Active Devices</h3>
        <div><span class="value" id="active-devices">--</span></div>
      </div>
      <div class="stat-card">
        <h3>Avg Efficiency</h3>
        <div><span class="value" id="avg-efficiency">--</span><span class="unit">%</span></div>
      </div>
      <div class="stat-card">
        <h3>Avg Temperature</h3>
        <div><span class="value" id="avg-temp">--</span><span class="unit" id="avg-temp-unit">°C</span></div>
      </div>
      <div class="stat-card">
        <h3>Total Energy</h3>
        <div><span class="value" id="total-energy">--</span><span class="unit">kWh</span></div>
      </div>
    </div>
    
    <div class="energy-history-card">
      <h2>Daily Energy History (Last 7 Days)</h2>
      <canvas id="energyChart" style="max-height: 300px;"></canvas>
    </div>
    
    <div class="devices-grid" id="devices"></div>
  </div>
  
  <script>
    // API Token configuration
    const API_TOKEN = ")html" + api_token_ + R"html(";
    
    // Fetch wrapper that includes authorization header if token is set
    async function apiFetch(url, options = {}) {
      if (API_TOKEN) {
        options.headers = options.headers || {};
        options.headers['Authorization'] = 'Bearer ' + API_TOKEN;
      }
      return fetch(url, options);
    }
    
    // Temperature unit management
    let useFahrenheit = localStorage.getItem('tempUnit') === 'F';
    
    function celsiusToFahrenheit(celsius) {
      return (celsius * 9/5) + 32;
    }
    
    // Theme toggle
    let darkMode = localStorage.getItem('darkMode') === 'true';
    
    function toggleTheme() {
      darkMode = !darkMode;
      localStorage.setItem('darkMode', darkMode);
      applyTheme();
    }
    
    function applyTheme() {
      if (darkMode) {
        document.body.classList.add('dark-mode');
        document.getElementById('theme-toggle').textContent = '☀️';
      } else {
        document.body.classList.remove('dark-mode');
        document.getElementById('theme-toggle').textContent = '🌙';
      }
      // Redraw energy chart with new theme colors (only if already loaded)
      if (energyHistoryLoaded && typeof loadEnergyHistory === 'function') {
        loadEnergyHistory();
      }
    }
    
    // Temperature toggle
    function toggleTempUnit() {
      useFahrenheit = !useFahrenheit;
      localStorage.setItem('tempUnit', useFahrenheit ? 'F' : 'C');
      document.getElementById('temp-toggle').textContent = useFahrenheit ? '°C' : '°F';
      loadData(); // Refresh display with new units
    }
    
    function formatTemperature(celsius) {
      if (useFahrenheit) {
        return celsiusToFahrenheit(celsius).toFixed(1);
      }
      return celsius.toFixed(1);
    }
    
    function getTempUnit() {
      return useFahrenheit ? '°F' : '°C';
    }
    
    // Release banner functions
    const CURRENT_VERSION = 'v1.3.1'; // Update this with each release
    
    async function checkForNewRelease() {
      try {
        // Check if banner was dismissed for this version
        const dismissedVersion = localStorage.getItem('dismissedReleaseVersion');
        if (dismissedVersion === CURRENT_VERSION) {
          return; // Already dismissed for current version
        }
        
        // Fetch latest release from GitHub
        const response = await fetch('https://api.github.com/repos/RAR/esphome-tigomonitor/releases/latest');
        if (!response.ok) {
          console.warn('Could not fetch release info');
          return;
        }
        
        const release = await response.json();
        const latestVersion = release.tag_name;
        
        // Compare versions - show banner if latest is newer than current
        if (latestVersion !== CURRENT_VERSION && !dismissedVersion) {
          document.getElementById('release-message').textContent = `Version ${latestVersion} is now available!`;
          document.getElementById('release-link').href = release.html_url;
          document.getElementById('release-banner').style.display = 'flex';
        }
      } catch (error) {
        console.error('Error checking for releases:', error);
      }
    }
    
    function dismissReleaseBanner() {
      document.getElementById('release-banner').style.display = 'none';
      // Remember dismissal for current version
      localStorage.setItem('dismissedReleaseVersion', CURRENT_VERSION);
    }
    
    // Initialize toggle buttons
    let invertersData = { inverters: [] }; // Cache inverter config (static, only changes on reboot)
    
    document.addEventListener('DOMContentLoaded', () => {
      applyTheme();
      document.getElementById('temp-toggle').textContent = useFahrenheit ? '°C' : '°F';
      // Check for new releases
      checkForNewRelease();
      // Initial load - fetch inverters once, then start refresh cycle
      loadInitialData();
    });
    
    async function loadInitialData() {
      // Fetch inverter config once on page load (static configuration)
      try {
        const invertersRes = await apiFetch('/api/inverters');
        invertersData = await invertersRes.json();
        console.log('Inverters data loaded:', invertersData);
      } catch (err) {
        console.warn('Inverters endpoint not available, using empty data', err);
      }
      
      // Load dynamic data
      try {
        await loadData();
        await loadEnergyHistory();
      } catch (error) {
        console.error('Error loading initial data:', error);
        setTimeout(loadInitialData, 5000);
      }
    }
    
    async function loadData() {
      try {
        // Load dynamic data sequentially to reduce server load
        const devicesRes = await apiFetch('/api/devices');
        const devicesData = await devicesRes.json();
        
        const overviewRes = await apiFetch('/api/overview');
        const overviewData = await overviewRes.json();
        
        const stringsRes = await apiFetch('/api/strings');
        const stringsData = await stringsRes.json();
        
        processAndRenderData(devicesData, overviewData, stringsData, invertersData);
      } catch (error) {
        console.error('Error loading data:', error);
        document.getElementById('devices').innerHTML = '<div class="device-card">Error loading data. Retrying...</div>';
        setTimeout(loadData, 5000);
      }
    }
    
    function processAndRenderData(devicesData, overviewData, stringsData, invertersData) {
      try {
        document.getElementById('total-power').textContent = overviewData.total_power.toFixed(1);
        document.getElementById('total-current').textContent = overviewData.total_current.toFixed(3);
        document.getElementById('active-devices').textContent = overviewData.active_devices;
        document.getElementById('avg-efficiency').textContent = overviewData.avg_efficiency.toFixed(1);
        document.getElementById('avg-temp').textContent = formatTemperature(overviewData.avg_temperature);
        document.getElementById('avg-temp-unit').textContent = getTempUnit();
        document.getElementById('total-energy').textContent = overviewData.total_energy.toFixed(3);
        
        // Group devices by string
        const devicesByString = {};
        const unassignedDevices = [];
        
        devicesData.devices.forEach(device => {
          if (device.string_label) {
            if (!devicesByString[device.string_label]) {
              devicesByString[device.string_label] = [];
            }
            devicesByString[device.string_label].push(device);
          } else {
            unassignedDevices.push(device);
          }
        });
        
        // Render device card helper
        function renderDevice(device) {
          // Check for "never updated" condition (ULONG_MAX or very large value)
          const ageText = device.data_age_ms > 86400000 ? 'Never' :  // > 1 day indicates invalid/never
                          device.data_age_ms < 1000 ? `${device.data_age_ms}ms` : 
                          device.data_age_ms < 60000 ? `${(device.data_age_ms/1000).toFixed(1)}s` :
                          `${(device.data_age_ms/60000).toFixed(1)}m`;
          
          const subtitle = device.barcode || `Addr: ${device.addr}`;
          
          return `
            <div class="device-card">
              <div class="device-header">
                <div class="device-title-section">
                  <div class="device-title">${device.name}</div>
                  <div class="device-subtitle">${subtitle}</div>
                </div>
                <div class="device-badge">${ageText}</div>
              </div>
              <div class="device-metrics">
                <div class="metric">
                  <span class="metric-label">Power</span>
                  <span class="metric-value">${device.power.toFixed(1)} W</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Peak Power</span>
                  <span class="metric-value">${device.peak_power.toFixed(1)} W</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Current</span>
                  <span class="metric-value">${device.current.toFixed(3)} A</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Voltage In</span>
                  <span class="metric-value">${device.voltage_in.toFixed(2)} V</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Voltage Out</span>
                  <span class="metric-value">${device.voltage_out.toFixed(2)} V</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Temperature</span>
                  <span class="metric-value">${formatTemperature(device.temperature)} ${getTempUnit()}</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Efficiency</span>
                  <span class="metric-value">${device.efficiency.toFixed(1)} %</span>
                </div>
                <div class="metric">
                  <span class="metric-label">Duty Cycle</span>
                  <span class="metric-value">${device.duty_cycle} %</span>
                </div>
                <div class="metric">
                  <span class="metric-label">RSSI</span>
                  <span class="metric-value">${device.rssi} dBm</span>
                </div>
              </div>
            </div>
          `;
        }
        
        // Build HTML with inverter/string groups
        const devicesContainer = document.getElementById('devices');
        let html = '';
        
        // Check if inverters are configured
        if (invertersData.inverters && invertersData.inverters.length > 0) {
          // Group by inverter -> MPPT -> String
          invertersData.inverters.forEach(inverter => {
            // Inverter summary card
            html += `
              <div class="string-summary" style="background: linear-gradient(135deg, #52525b 0%, #3f3f46 100%); color: white; margin-bottom: 0.5rem;">
                <div class="string-header" style="border-bottom-color: rgba(255,255,255,0.2);">
                  <h3 style="color: white; font-size: 1.3rem;">⚡ ${inverter.name}</h3>
                </div>
                <div class="string-stats">
                  <div class="string-stat">
                    <span class="stat-label" style="color: rgba(255,255,255,0.9);">Devices</span>
                    <span class="stat-value" style="color: white;">${inverter.total_devices}</span>
                  </div>
                  <div class="string-stat">
                    <span class="stat-label" style="color: rgba(255,255,255,0.9);">Power</span>
                    <span class="stat-value" style="color: white;">${inverter.total_power.toFixed(1)} W</span>
                  </div>
                  <div class="string-stat">
                    <span class="stat-label" style="color: rgba(255,255,255,0.9);">Peak</span>
                    <span class="stat-value" style="color: white;">${inverter.peak_power.toFixed(1)} W</span>
                  </div>
                </div>
              </div>
            `;
            
            // Render strings for this inverter's MPPTs
            inverter.mppts.forEach(mpptLabel => {
              const stringsForMppt = stringsData.strings.filter(s => s.inverter === mpptLabel);
              
              stringsForMppt.forEach(stringInfo => {
                const devices = devicesByString[stringInfo.label] || [];
                
                // String summary card (indented under inverter)
                html += `
                  <div class="string-summary" style="margin-left: 2rem; border-left: 4px solid rgba(255,255,255,0.3);">
                    <div class="string-header">
                      <h3>${stringInfo.label}</h3>
                      <span class="string-inverter">${stringInfo.inverter}</span>
                    </div>
                    <div class="string-stats">
                      <div class="string-stat">
                        <span class="stat-label">Devices</span>
                        <span class="stat-value">${stringInfo.total_devices}</span>
                      </div>
                      <div class="string-stat">
                        <span class="stat-label">Power</span>
                        <span class="stat-value">${stringInfo.total_power.toFixed(1)} W</span>
                      </div>
                      <div class="string-stat">
                        <span class="stat-label">Peak</span>
                        <span class="stat-value">${stringInfo.peak_power.toFixed(1)} W</span>
                      </div>
                      <div class="string-stat">
                        <span class="stat-label">Avg Eff</span>
                        <span class="stat-value">${stringInfo.avg_efficiency.toFixed(1)}%</span>
                      </div>
                    </div>
                  </div>
                `;
                
                // Devices in this string (further indented)
                devices.forEach(device => {
                  html += '<div style="margin-left: 2rem;">' + renderDevice(device) + '</div>';
                });
              });
            });
          });
          
          // Render unassigned strings (those not in any inverter's MPPTs)
          const assignedMppts = new Set();
          invertersData.inverters.forEach(inv => inv.mppts.forEach(m => assignedMppts.add(m)));
          
          const unassignedStrings = stringsData.strings.filter(s => !assignedMppts.has(s.inverter));
          if (unassignedStrings.length > 0) {
            html += '<div class="string-summary"><div class="string-header"><h3>Unassigned MPPTs</h3></div></div>';
            unassignedStrings.forEach(stringInfo => {
              const devices = devicesByString[stringInfo.label] || [];
              html += `
                <div class="string-summary">
                  <div class="string-header">
                    <h3>${stringInfo.label}</h3>
                    <span class="string-inverter">${stringInfo.inverter}</span>
                  </div>
                  <div class="string-stats">
                    <div class="string-stat">
                      <span class="stat-label">Devices</span>
                      <span class="stat-value">${stringInfo.total_devices}</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Power</span>
                      <span class="stat-value">${stringInfo.total_power.toFixed(1)} W</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Peak</span>
                      <span class="stat-value">${stringInfo.peak_power.toFixed(1)} W</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Avg Eff</span>
                      <span class="stat-value">${stringInfo.avg_efficiency.toFixed(1)}%</span>
                    </div>
                  </div>
                </div>
              `;
              devices.forEach(device => {
                html += renderDevice(device);
              });
            });
          }
        } else {
          // Original rendering: Render strings with their summary cards
          const stringLabels = Object.keys(devicesByString).sort();
          stringLabels.forEach(stringLabel => {
            const stringInfo = stringsData.strings.find(s => s.label === stringLabel);
            const devices = devicesByString[stringLabel];
            
            // String summary card
            if (stringInfo) {
              html += `
                <div class="string-summary">
                  <div class="string-header">
                    <h3>${stringLabel}</h3>
                    <span class="string-inverter">${stringInfo.inverter}</span>
                  </div>
                  <div class="string-stats">
                    <div class="string-stat">
                      <span class="stat-label">Devices</span>
                      <span class="stat-value">${stringInfo.total_devices}</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Power</span>
                      <span class="stat-value">${stringInfo.total_power.toFixed(1)} W</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Peak</span>
                      <span class="stat-value">${stringInfo.peak_power.toFixed(1)} W</span>
                    </div>
                    <div class="string-stat">
                      <span class="stat-label">Avg Eff</span>
                      <span class="stat-value">${stringInfo.avg_efficiency.toFixed(1)}%</span>
                    </div>
                  </div>
                </div>
              `;
            }
            
            // Devices in this string
            devices.forEach(device => {
              html += renderDevice(device);
            });
          });
        }
        
        // Unassigned devices section
        if (unassignedDevices.length > 0) {
          html += '<div class="string-summary"><div class="string-header"><h3>Unassigned Devices</h3></div></div>';
          unassignedDevices.forEach(device => {
            html += renderDevice(device);
          });
        }
        
        devicesContainer.innerHTML = html;
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    // Energy history chart
    let energyChart = null;
    let energyHistoryLoaded = false;  // Track if energy history has been loaded
    
    async function loadEnergyHistory() {
      try {
        const res = await apiFetch('/api/energy/history');
        const data = await res.json();
        
        if (data.error) {
          console.error('Energy history error:', data.error);
          return;
        }
        
        // Prepare chart data
        const dates = data.history.map(entry => {
          // Parse date string directly to avoid timezone shifts
          // Format: "YYYY-MM-DD" -> "M/D"
          const parts = entry.date.split('-');
          return parseInt(parts[1]) + '/' + parseInt(parts[2]);
        });
        const energies = data.history.map(entry => entry.energy);
        
        // Render simple bar chart
        const ctx = document.getElementById('energyChart');
        if (!ctx) return;
        
        // Destroy existing chart
        if (energyChart) {
          energyChart = null;
        }
        
        // Simple canvas-based bar chart
        const canvas = ctx;
        const context = canvas.getContext('2d');
        const width = canvas.parentElement.clientWidth - 40;
        const height = 300;
        canvas.width = width;
        canvas.height = height;
        
        // Clear canvas
        context.clearRect(0, 0, width, height);
        
        const isDarkMode = document.body.classList.contains('dark-mode');
        const textColor = isDarkMode ? '#ffffff' : '#2c3e50';
        const axisColor = isDarkMode ? '#cccccc' : '#666';
        
        if (energies.length === 0) {
          context.fillStyle = textColor;
          context.font = '14px Arial';
          context.textAlign = 'center';
          context.fillText('No energy history data yet', width / 2, height / 2);
          return;
        }
        
        // Calculate chart dimensions
        const padding = 50;
        const chartWidth = width - padding * 2;
        const chartHeight = height - padding * 2;
        const barWidth = chartWidth / energies.length - 10;
        const maxEnergy = Math.max(...energies);
        const scale = maxEnergy > 0 ? chartHeight / maxEnergy : 1;
        
        // Draw axes
        context.strokeStyle = axisColor;
        context.lineWidth = 1;
        context.beginPath();
        context.moveTo(padding, padding);
        context.lineTo(padding, height - padding);
        context.lineTo(width - padding, height - padding);
        context.stroke();
        
        // Draw bars
        energies.forEach((energy, index) => {
          const barHeight = energy * scale;
          const x = padding + index * (barWidth + 10) + 5;
          const y = height - padding - barHeight;
          
          // Bar
          const gradient = context.createLinearGradient(x, y, x, y + barHeight);
          gradient.addColorStop(0, isDarkMode ? '#60a5fa' : '#3b82f6');
          gradient.addColorStop(1, isDarkMode ? '#3b82f6' : '#2563eb');
          context.fillStyle = gradient;
          context.fillRect(x, y, barWidth, barHeight);
          
          // Value label on top
          context.fillStyle = textColor;
          context.font = '12px Arial';
          context.textAlign = 'center';
          context.fillText(energy.toFixed(1), x + barWidth / 2, y - 5);
          
          // Date label below
          context.save();
          context.translate(x + barWidth / 2, height - padding + 15);
          context.rotate(-Math.PI / 4);
          context.textAlign = 'right';
          context.font = '11px Arial';
          context.fillText(dates[index], 0, 0);
          context.restore();
        });
        
        // Y-axis label
        context.save();
        context.translate(15, height / 2);
        context.rotate(-Math.PI / 2);
        context.textAlign = 'center';
        context.font = 'bold 12px Arial';
        context.fillStyle = textColor;
        context.fillText('Energy (kWh)', 0, 0);
        context.restore();
        
        energyHistoryLoaded = true;  // Mark as loaded
        
      } catch (error) {
        console.error('Error loading energy history:', error);
      }
    }
    
    // Redraw chart on window resize (debounced)
    let resizeTimeout;
    window.addEventListener('resize', () => {
      if (!energyHistoryLoaded) return;
      clearTimeout(resizeTimeout);
      resizeTimeout = setTimeout(() => {
        loadEnergyHistory();
      }, 250);  // Debounce 250ms to avoid excessive redraws
    });
    
    // Start auto-refresh intervals
    setInterval(loadData, 10000);  // Poll every 10 seconds to reduce memory churn
    setInterval(loadEnergyHistory, 60000);  // Update energy chart every minute
  </script>
</body>
</html>
)html");
}

void TigoWebServer::get_node_table_html(PSRAMString& html) {
  html.append(R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - Node Table</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .theme-toggle, .github-link { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-flex; align-items: center; }
    .theme-toggle:hover, .github-link:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: all 0.2s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1400px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; }
    .card h2 { color: #2c3e50; margin-bottom: 1.5rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    table { width: 100%; border-collapse: collapse; }
    thead { background: #34495e; color: white; }
    th { padding: 1rem; text-align: left; font-weight: 600; }
    td { padding: 1rem; border-bottom: 1px solid #ecf0f1; transition: background-color 0.3s; }
    tbody tr:hover { background: #f8f9fa; }
    .badge { display: inline-block; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; font-weight: 600; }
    .badge-success { background: #27ae60; color: white; }
    .badge-warning { background: #f39c12; color: white; }
    .badge-info { background: #3498db; color: white; }
    .code { font-family: monospace; background: #f8f9fa; padding: 0.25rem 0.5rem; border-radius: 4px; transition: background-color 0.3s; }
    .cca-info { color: #16a085; font-weight: 500; }
    .hierarchy { color: #95a5a6; font-size: 0.85rem; margin-top: 0.25rem; }
    button:hover { opacity: 0.8; }
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1c2833; }
    body.dark-mode .card { background: #2d2d2d; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .card h2 { color: #e0e0e0; }
    body.dark-mode thead { background: #1e1e1e; }
    body.dark-mode td { border-bottom-color: #444; color: #e0e0e0; }
    body.dark-mode tbody tr:hover { background: #3a3a3a; }
    body.dark-mode .code { background: #3a3a3a; color: #e0e0e0; }
    body.dark-mode .hierarchy { color: #b0b0b0; }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
        <a href="https://github.com/RAR/esphome-tigomonitor" target="_blank" class="github-link" title="View on GitHub">
          <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
        </a>
        <button class="theme-toggle" onclick="toggleTheme()" id="theme-toggle">🌙</button>
      </div>
    </div>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/nodes" class="active">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="card">
      <h2>Node Table</h2>
      <div style="margin-bottom: 1.5rem; display: flex; gap: 1rem; flex-wrap: wrap;">
        <button onclick="exportNodeTable()" style="padding: 0.75rem 1.5rem; background-color: #27ae60; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 0.875rem; font-weight: 600;">
          📥 Export Node Table
        </button>
        <button onclick="document.getElementById('import-file').click()" style="padding: 0.75rem 1.5rem; background-color: #3498db; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 0.875rem; font-weight: 600;">
          📤 Import Node Table
        </button>
        <input type="file" id="import-file" accept=".json" style="display: none;" onchange="importNodeTable(event)">
        <span id="import-status" style="align-self: center; font-size: 0.875rem; color: #27ae60; display: none;"></span>
      </div>
      <table>
        <thead>
          <tr>
            <th>Address</th>
            <th>Device Name / Barcode</th>
            <th>Location</th>
            <th>Sensor Index</th>
            <th>Action</th>
          </tr>
        </thead>
        <tbody id="node-table-body">
          <tr><td colspan="5" style="text-align:center;">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>
  
  <script>
    // API Token configuration
    const API_TOKEN = ")html" + api_token_ + R"html(";
    
    // Fetch wrapper that includes authorization header if token is set
    async function apiFetch(url, options = {}) {
      if (API_TOKEN) {
        options.headers = options.headers || {};
        options.headers['Authorization'] = 'Bearer ' + API_TOKEN;
      }
      return fetch(url, options);
    }
    
    // Dark mode
    let darkMode = localStorage.getItem('darkMode') === 'true';
    
    function toggleTheme() {
      darkMode = !darkMode;
      localStorage.setItem('darkMode', darkMode);
      applyTheme();
    }
    
    function applyTheme() {
      if (darkMode) {
        document.body.classList.add('dark-mode');
        document.getElementById('theme-toggle').textContent = '☀️';
      } else {
        document.body.classList.remove('dark-mode');
        document.getElementById('theme-toggle').textContent = '🌙';
      }
    }
    
    // Apply theme on load
    applyTheme();
    
    async function exportNodeTable() {
      try {
        const response = await apiFetch('/api/nodes');
        const data = await response.json();
        
        // Create downloadable JSON file
        const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `node-table-${new Date().toISOString().split('T')[0]}.json`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      } catch (error) {
        console.error('Error exporting node table:', error);
        alert('Failed to export node table: ' + error.message);
      }
    }
    
    async function importNodeTable(event) {
      const file = event.target.files[0];
      if (!file) return;
      
      const statusEl = document.getElementById('import-status');
      
      try {
        const text = await file.text();
        const data = JSON.parse(text);
        
        // Validate data structure
        if (!data.nodes || !Array.isArray(data.nodes)) {
          throw new Error('Invalid node table format: missing "nodes" array');
        }
        
        // Confirm import
        if (!confirm(`Import ${data.nodes.length} node(s)? This will replace the current node table.`)) {
          event.target.value = ''; // Reset file input
          return;
        }
        
        // Send to server
        const response = await apiFetch('/api/nodes/import', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(data)
        });
        
        if (response.ok) {
          const result = await response.json();
          statusEl.textContent = `✅ Successfully imported ${result.imported || data.nodes.length} node(s)`;
          statusEl.style.color = '#27ae60';
          statusEl.style.display = 'inline';
          
          // Reload table
          await loadData();
          
          // Hide status after 5 seconds
          setTimeout(() => {
            statusEl.style.display = 'none';
          }, 5000);
        } else {
          const error = await response.json();
          throw new Error(error.message || 'Import failed');
        }
      } catch (error) {
        console.error('Error importing node table:', error);
        statusEl.textContent = `❌ Import failed: ${error.message}`;
        statusEl.style.color = '#e74c3c';
        statusEl.style.display = 'inline';
        
        setTimeout(() => {
          statusEl.style.display = 'none';
        }, 5000);
      }
      
      // Reset file input
      event.target.value = '';
    }
    
    async function deleteNode(addr) {
      if (!confirm('Are you sure you want to delete node with address ' + addr + '?')) {
        return;
      }
      
      try {
        const response = await apiFetch('/api/nodes/delete?addr=' + encodeURIComponent(addr), {
          method: 'POST'
        });
        
        if (response.ok) {
          // Reload the table
          await loadData();
        } else {
          alert('Failed to delete node');
        }
      } catch (error) {
        console.error('Error deleting node:', error);
        alert('Error deleting node: ' + error.message);
      }
    }
    
    async function loadData() {
      try {
        const response = await apiFetch('/api/nodes');
        const data = await response.json();
        
        const tbody = document.getElementById('node-table-body');
        tbody.innerHTML = data.nodes.map(node => {
          // Build device name/barcode cell
          let deviceInfo = '';
          if (node.cca_validated && node.cca_label) {
            deviceInfo = `<span class="cca-info">${node.cca_label}</span>`;
            if (node.long_address) {
              deviceInfo += `<div class="hierarchy">Barcode: <span class="code">${node.long_address}</span></div>`;
            } else if (node.frame09_barcode) {
              deviceInfo += `<div class="hierarchy">Barcode: <span class="code">${node.frame09_barcode}</span></div>`;
            }
          } else {
            deviceInfo = `<span class="code">${node.long_address || node.frame09_barcode || '-'}</span>`;
          }
          
          // Build location cell
          let location = '';
          if (node.cca_validated) {
            if (node.cca_inverter && node.cca_string) {
              location = `<div class="hierarchy">${node.cca_inverter} → ${node.cca_string}</div>`;
            } else if (node.cca_string) {
              location = `<div class="hierarchy">${node.cca_string}</div>`;
            }
            if (node.cca_channel) {
              location += `<div class="hierarchy" style="font-size:0.75rem; margin-top:0.125rem;">Channel: ${node.cca_channel}</div>`;
            }
          }
          if (!location) {
            location = '<span style="color:#bdc3c7;">-</span>';
          }
          
          return `
          <tr>
            <td><span class="code">${node.addr}</span></td>
            <td>${deviceInfo}</td>
            <td>${location}</td>
            <td>
              ${node.sensor_index >= 0 ? 
                `<span class="badge badge-success">Tigo ${node.sensor_index + 1}</span>` : 
                `<span class="badge badge-warning">Unassigned</span>`}
              ${node.cca_validated ? '<span class="badge badge-info" style="margin-left:0.5rem;">CCA ✓</span>' : ''}
            </td>
            <td style="text-align:center;">
              <button onclick="deleteNode('${node.addr}')" style="padding: 0.5rem 1rem; background-color: #e74c3c; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 0.875rem;">
                🗑️ Delete
              </button>
            </td>
          </tr>
        `}).join('');
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    loadData();
    setInterval(loadData, 10000);
  </script>
</body>
</html>
)html");
}

void TigoWebServer::get_esp_status_html(PSRAMString& html) {
  html.append(R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - ESP32 Status</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s; }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .temp-toggle, .theme-toggle, .github-link { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-flex; align-items: center; }
    .temp-toggle:hover, .theme-toggle:hover, .github-link:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: background-color 0.3s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 1.5rem; transition: background-color 0.3s, box-shadow 0.3s; }
    .card h2 { color: #2c3e50; margin-bottom: 1.5rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; transition: color 0.3s; }
    .info-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1rem; }
    .info-item { background: #f8f9fa; padding: 1.5rem; border-radius: 4px; transition: background-color 0.3s; }
    .info-item h3 { color: #7f8c8d; font-size: 0.875rem; margin-bottom: 0.5rem; text-transform: uppercase; transition: color 0.3s; }
    .info-item .value { font-size: 1.5rem; font-weight: bold; color: #2c3e50; transition: color 0.3s; }
    .progress-bar { width: 100%; height: 20px; background: #ecf0f1; border-radius: 10px; overflow: hidden; margin-top: 0.5rem; transition: background-color 0.3s; }
    .progress-fill { height: 100%; background: #27ae60; transition: width 0.3s, background-color 0.3s; }
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1c2833; }
    body.dark-mode .card { background: #2c2c2c; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .card h2 { color: #e0e0e0; border-bottom-color: #5dade2; }
    body.dark-mode .info-item { background: #1e1e1e; }
    body.dark-mode .info-item h3 { color: #a0a0a0; }
    body.dark-mode .info-item .value { color: #e0e0e0; }
    body.dark-mode .progress-bar { background: #3a3a3a; }
    body.dark-mode .progress-fill { background: #45a87d; }
    
    /* Warning banner styles */
    .warning-banner { background: #fff3cd; border: 2px solid #ffc107; border-radius: 8px; padding: 1.5rem; margin-bottom: 1.5rem; display: none; }
    .warning-banner.show { display: block; }
    .warning-banner h3 { color: #856404; margin-bottom: 1rem; font-size: 1.25rem; }
    .warning-banner p { color: #856404; margin-bottom: 0.5rem; line-height: 1.5; }
    .warning-banner code { background: #fff; padding: 0.2rem 0.5rem; border-radius: 3px; font-family: 'Courier New', monospace; font-size: 0.875rem; }
    .warning-banner ul { margin-left: 1.5rem; margin-top: 0.5rem; }
    .warning-banner li { margin-bottom: 0.25rem; }
    body.dark-mode .warning-banner { background: #4a3c1a; border-color: #ffc107; }
    body.dark-mode .warning-banner h3, body.dark-mode .warning-banner p { color: #ffc107; }
    body.dark-mode .warning-banner code { background: #2c2c2c; color: #e0e0e0; }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
        <a href="https://github.com/RAR/esphome-tigomonitor" target="_blank" class="github-link" title="View on GitHub"><svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg></a>
        <button class="temp-toggle" onclick="toggleTempUnit()" id="temp-toggle">°F</button>
        <button class="theme-toggle" onclick="toggleTheme()">🌙</button>
      </div>
    </div>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/nodes">Node Table</a>
      <a href="/status" class="active">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="warning-banner" id="psram-warning">
      <h3>⚠️ PSRAM Not Configured</h3>
      <p><strong>Your board may have PSRAM, but it's not enabled in your configuration.</strong></p>
      <p>PSRAM is <strong>required for 15+ devices</strong> to prevent memory exhaustion and socket creation failures.</p>
      <p id="psram-config-instructions"></p>
    </div>
    
    <div class="card">
      <h2>System Information</h2>
      <div class="info-grid">
        <div class="info-item">
          <h3>Uptime</h3>
          <div class="value" id="uptime">--</div>
        </div>
        <div class="info-item">
          <h3>ESPHome Version</h3>
          <div class="value" id="version">--</div>
        </div>
        <div class="info-item">
          <h3>Compiled</h3>
          <div class="value" id="compile-time">--</div>
        </div>
        <div class="info-item">
          <h3>Active Tasks</h3>
          <div class="value" id="task-count">--</div>
        </div>
        <div class="info-item">
          <h3>Temperature</h3>
          <div class="value" id="internal-temp">--</div>
        </div>
        <div class="info-item">
          <h3>Minimum Free Heap</h3>
          <div class="value" id="min-heap">--</div>
        </div>
        <div class="info-item">
          <h3>Minimum Free PSRAM</h3>
          <div class="value" id="min-psram">--</div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>Memory</h2>
      <div class="info-grid">
        <div class="info-item">
          <h3>Internal RAM</h3>
          <div class="value" id="heap-free">--</div>
          <div class="progress-bar">
            <div class="progress-fill" id="heap-progress"></div>
          </div>
        </div>
        <div class="info-item">
          <h3>PSRAM</h3>
          <div class="value" id="psram-free">--</div>
          <div class="progress-bar">
            <div class="progress-fill" id="psram-progress"></div>
          </div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>Network</h2>
      <div class="info-grid">
        <div class="info-item">
          <h3>Connection Status</h3>
          <div class="value" id="network-status">--</div>
        </div>
        <div class="info-item">
          <h3>WiFi SSID</h3>
          <div class="value" id="wifi-ssid">--</div>
        </div>
        <div class="info-item">
          <h3>WiFi Signal (RSSI)</h3>
          <div class="value" id="wifi-rssi">--</div>
        </div>
        <div class="info-item">
          <h3>IP Address</h3>
          <div class="value" id="ip-address">--</div>
        </div>
        <div class="info-item">
          <h3>MAC Address</h3>
          <div class="value" id="mac-address">--</div>
        </div>
        <div class="info-item">
          <h3>Active Sockets</h3>
          <div class="value" id="socket-count">--</div>
          <div class="progress-bar">
            <div class="progress-fill" id="socket-progress"></div>
          </div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>UART Diagnostics</h2>
      <div class="info-grid">
        <div class="info-item">
          <h3>Invalid Checksums</h3>
          <div class="value" id="invalid-checksum">--</div>
        </div>
        <div class="info-item">
          <h3>Missed Frames</h3>
          <div class="value" id="missed-frames">--</div>
          <div style="font-size: 0.9em; color: #7f8c8d; margin-top: 0.5rem;">
            <span id="missed-frames-rate">--</span>
          </div>
        </div>
        <div class="info-item">
          <h3>Total Frames</h3>
          <div class="value" id="total-frames">--</div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>Actions</h2>
      <div style="display: flex; gap: 1rem; flex-wrap: wrap;">
        <button onclick="toggleBacklight()" id="backlight-toggle" style="padding: 12px 24px; background-color: #9b59b6; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold;">
          💡 Toggle Backlight
        </button>
        <button onclick="restartESP()" style="padding: 12px 24px; background-color: #e74c3c; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold;">
          🔄 Restart ESP32
        </button>
        <button onclick="resetPeakPower()" style="padding: 12px 24px; background-color: #f39c12; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold;">
          ⚡ Reset Peak Power
        </button>
        <button onclick="resetNodeTable()" style="padding: 12px 24px; background-color: #c0392b; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold;">
          🗑️ Reset All Node Data
        </button>
      </div>
      <div id="backlight-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
      <div id="restart-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
      <div id="reset-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
      <div id="node-reset-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
    </div>
  </div>
  
  <script>
    // API Token configuration
    const API_TOKEN = ")html" + api_token_ + R"html(";
    
    // Fetch wrapper that includes authorization header if token is set
    async function apiFetch(url, options = {}) {
      if (API_TOKEN) {
        options.headers = options.headers || {};
        options.headers['Authorization'] = 'Bearer ' + API_TOKEN;
      }
      return fetch(url, options);
    }
    
    // Dark mode support
    let darkMode = localStorage.getItem('darkMode') === 'true';
    
    // Temperature unit toggle
    let useFahrenheit = localStorage.getItem('tempUnit') === 'F';
    
    function celsiusToFahrenheit(celsius) {
      return (celsius * 9/5) + 32;
    }
    
    function formatTemperature(celsius) {
      if (useFahrenheit) {
        return celsiusToFahrenheit(celsius).toFixed(1);
      }
      return celsius.toFixed(1);
    }
    
    function getTempUnit() {
      return useFahrenheit ? '°F' : '°C';
    }
    
    function toggleTempUnit() {
      useFahrenheit = !useFahrenheit;
      localStorage.setItem('tempUnit', useFahrenheit ? 'F' : 'C');
      document.getElementById('temp-toggle').textContent = useFahrenheit ? '°C' : '°F';
      loadData(); // Refresh display with new units
    }
    
    function toggleTheme() {
      darkMode = !darkMode;
      localStorage.setItem('darkMode', darkMode);
      applyTheme();
    }
    
    async function toggleBacklight() {
      const messageDiv = document.getElementById('backlight-message');
      try {
        const response = await apiFetch('/api/backlight', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'state=toggle'
        });
        const data = await response.json();
        if (data.success) {
          messageDiv.style.display = 'block';
          messageDiv.style.backgroundColor = '#d4edda';
          messageDiv.style.color = '#155724';
          messageDiv.style.border = '1px solid #c3e6cb';
          messageDiv.textContent = `Backlight turned ${data.state.toUpperCase()}`;
          setTimeout(() => { messageDiv.style.display = 'none'; }, 3000);
        } else {
          messageDiv.style.display = 'block';
          messageDiv.style.backgroundColor = '#f8d7da';
          messageDiv.style.color = '#721c24';
          messageDiv.style.border = '1px solid #f5c6cb';
          messageDiv.textContent = `Error: ${data.error || 'Failed to toggle backlight'}`;
        }
      } catch (error) {
        console.error('Error toggling backlight:', error);
        messageDiv.style.display = 'block';
        messageDiv.style.backgroundColor = '#f8d7da';
        messageDiv.style.color = '#721c24';
        messageDiv.style.border = '1px solid #f5c6cb';
        messageDiv.textContent = `Error: ${error.message}`;
      }
    }
    
    function applyTheme() {
      if (darkMode) {
        document.body.classList.add('dark-mode');
        document.querySelector('.theme-toggle').textContent = '☀️';
      } else {
        document.body.classList.remove('dark-mode');
        document.querySelector('.theme-toggle').textContent = '🌙';
      }
    }
    
    // Apply theme and temp toggle on page load
    applyTheme();
    document.getElementById('temp-toggle').textContent = useFahrenheit ? '°C' : '°F';
    
    function formatBytes(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }
    
    async function loadData() {
      try {
        const response = await apiFetch('/api/status');
        const data = await response.json();
        
        document.getElementById('uptime').textContent = 
          `${data.uptime_days}d ${data.uptime_hours}h ${data.uptime_mins}m`;
        document.getElementById('version').textContent = data.esphome_version;
        document.getElementById('compile-time').textContent = data.compilation_time;
        
        const heapUsedPct = ((data.total_heap - data.free_heap) / data.total_heap * 100);
        document.getElementById('heap-free').textContent = 
          `${formatBytes(data.free_heap)} / ${formatBytes(data.total_heap)}`;
        document.getElementById('heap-progress').style.width = heapUsedPct + '%';
        
        if (data.total_psram > 0) {
          const psramUsedPct = ((data.total_psram - data.free_psram) / data.total_psram * 100);
          document.getElementById('psram-free').textContent = 
            `${formatBytes(data.free_psram)} / ${formatBytes(data.total_psram)}`;
          document.getElementById('psram-progress').style.width = psramUsedPct + '%';
          // Hide PSRAM warning if PSRAM is available
          document.getElementById('psram-warning').classList.remove('show');
        } else {
          document.getElementById('psram-free').textContent = 'Not available';
          document.getElementById('psram-progress').style.width = '0%';
          
          // Show PSRAM warning with chip-specific instructions
          const warningBanner = document.getElementById('psram-warning');
          const configInstructions = document.getElementById('psram-config-instructions');
          
          // Try to detect chip type from compilation info
          const compileInfo = data.compilation_time || '';
          let chipType = 'unknown';
          
          // Check build info or make educated guess
          // Note: We could add chip type to the API response for more accuracy
          if (compileInfo.includes('S3') || navigator.userAgent.includes('S3')) {
            chipType = 's3';
          } else if (compileInfo.includes('P4') || navigator.userAgent.includes('P4')) {
            chipType = 'p4';
          }
          
          if (chipType === 's3') {
            configInstructions.innerHTML = 
              '<p><strong>For ESP32-S3 (AtomS3R, etc.):</strong></p>' +
              '<p>Add to your YAML: <code>board_build.arduino.memory_type: qio_opi</code> and <code>-DBOARD_HAS_PSRAM</code></p>' +
              '<p>See <code>boards/esp32s3-atoms3r.yaml</code> for complete example.</p>';
          } else if (chipType === 'p4') {
            configInstructions.innerHTML = 
              '<p><strong>For ESP32-P4:</strong></p>' +
              '<p>Add to your YAML: <code>psram: { mode: hex, speed: 200MHz }</code></p>' +
              '<p>See <code>boards/esp32p4-evboard.yaml</code> for complete example.</p>';
          } else {
            configInstructions.innerHTML = 
              '<p>If your board has PSRAM, check the <code>boards/</code> folder for configuration examples.</p>';
          }
          
          warningBanner.classList.add('show');
        }
        
        // Display system information
        document.getElementById('task-count').textContent = data.task_count || '--';
        document.getElementById('internal-temp').textContent = 
          data.internal_temp !== undefined ? `${formatTemperature(data.internal_temp)}${getTempUnit()}` : '--';
        document.getElementById('min-heap').textContent = formatBytes(data.min_free_heap || 0);
        document.getElementById('invalid-checksum').textContent = data.invalid_checksum || 0;
        document.getElementById('missed-frames').textContent = data.missed_frames || 0;
        document.getElementById('total-frames').textContent = data.total_frames || 0;
        
        // Calculate and display miss rate
        if (data.total_frames > 0) {
          const totalAttempts = data.total_frames + data.missed_frames;
          const missRate = (data.missed_frames / totalAttempts * 100).toFixed(3);
          document.getElementById('missed-frames-rate').textContent = `${missRate}% miss rate`;
        } else {
          document.getElementById('missed-frames-rate').textContent = '--';
        }
        
        // Display network information
        document.getElementById('network-status').textContent = 
          data.network_connected ? '✓ Connected' : '✗ Disconnected';
        document.getElementById('network-status').style.color = 
          data.network_connected ? '#4caf50' : '#f44336';
        document.getElementById('wifi-ssid').textContent = data.wifi_ssid || 'N/A';
        document.getElementById('wifi-rssi').textContent = 
          data.wifi_rssi !== 0 ? `${data.wifi_rssi} dBm` : 'N/A';
        document.getElementById('ip-address').textContent = data.ip_address || 'N/A';
        document.getElementById('mac-address').textContent = data.mac_address || 'N/A';
        
        // Display socket usage
        const socketUsedPct = data.max_sockets > 0 ? 
          (data.active_sockets / data.max_sockets * 100) : 0;
        document.getElementById('socket-count').textContent = 
          `${data.active_sockets} / ${data.max_sockets}`;
        document.getElementById('socket-progress').style.width = socketUsedPct + '%';
        document.getElementById('socket-progress').style.backgroundColor = 
          socketUsedPct > 75 ? '#f44336' : (socketUsedPct > 50 ? '#ff9800' : '#4caf50');
        
        if (data.total_psram > 0) {
          document.getElementById('min-psram').textContent = formatBytes(data.min_free_psram || 0);
        } else {
          document.getElementById('min-psram').textContent = 'Not available';
        }
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    async function restartESP() {
      const messageDiv = document.getElementById('restart-message');
      
      if (!confirm('Are you sure you want to restart the ESP32? This will interrupt monitoring briefly.')) {
        return;
      }
      
      messageDiv.style.display = 'block';
      messageDiv.style.backgroundColor = '#3498db';
      messageDiv.style.color = 'white';
      messageDiv.textContent = 'Restarting ESP32... Please wait 10-15 seconds.';
      
      try {
        const response = await apiFetch('/api/restart', { method: 'POST' });
        if (response.ok) {
          messageDiv.style.backgroundColor = '#27ae60';
          messageDiv.textContent = 'Restart command sent! The ESP32 is rebooting. Page will reload in 15 seconds...';
          
          // Reload page after 15 seconds to reconnect
          setTimeout(() => {
            window.location.reload();
          }, 15000);
        } else {
          throw new Error('Failed to restart');
        }
      } catch (error) {
        messageDiv.style.backgroundColor = '#e74c3c';
        messageDiv.textContent = 'Error: Failed to send restart command. Please try again.';
        console.error('Restart error:', error);
      }
    }
    
    async function resetPeakPower() {
      const messageDiv = document.getElementById('reset-message');
      
      if (!confirm('Are you sure you want to reset all peak power values? This will clear the historical maximum power readings for all devices.')) {
        return;
      }
      
      messageDiv.style.display = 'block';
      messageDiv.style.backgroundColor = '#3498db';
      messageDiv.style.color = 'white';
      messageDiv.textContent = 'Resetting peak power values...';
      
      try {
        const response = await apiFetch('/api/reset_peak_power', { method: 'POST' });
        if (response.ok) {
          messageDiv.style.backgroundColor = '#27ae60';
          messageDiv.textContent = 'Peak power values have been reset successfully!';
          
          // Hide message after 5 seconds
          setTimeout(() => {
            messageDiv.style.display = 'none';
          }, 5000);
        } else {
          throw new Error('Failed to reset peak power');
        }
      } catch (error) {
        messageDiv.style.backgroundColor = '#e74c3c';
        messageDiv.textContent = 'Error: Failed to reset peak power. Please try again.';
        console.error('Reset error:', error);
      }
    }
    
    async function resetNodeTable() {
      const messageDiv = document.getElementById('node-reset-message');
      
      if (!confirm('⚠️ WARNING: This will delete ALL node data including device mappings, barcodes, and CCA labels!\n\nThe system will need to rediscover all devices and you will need to sync from CCA again.\n\nAre you absolutely sure you want to continue?')) {
        return;
      }
      
      messageDiv.style.display = 'block';
      messageDiv.style.backgroundColor = '#e67e22';
      messageDiv.style.color = 'white';
      messageDiv.textContent = 'Resetting all node data...';
      
      try {
        const response = await apiFetch('/api/reset_node_table', { method: 'POST' });
        if (response.ok) {
          messageDiv.style.backgroundColor = '#27ae60';
          messageDiv.textContent = 'All node data has been reset! System will restart in 3 seconds...';
          
          // Wait 3 seconds then restart
          setTimeout(() => {
            window.location.href = '/';
          }, 3000);
        } else {
          throw new Error('Failed to reset node table');
        }
      } catch (error) {
        messageDiv.style.backgroundColor = '#e74c3c';
        messageDiv.textContent = 'Error: Failed to reset node data. Please try again.';
        console.error('Reset node table error:', error);
      }
    }
    
    // Log polling system (instead of WebSocket)
    
    loadData();
    setInterval(loadData, 10000);  // Poll every 10 seconds to reduce memory churn
  </script>
</body>
</html>
)html");
}

void TigoWebServer::get_yaml_config_html(PSRAMString& html) {
  html.append(R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - YAML Configuration</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s; }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .theme-toggle, .github-link { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-flex; align-items: center; }
    .theme-toggle:hover, .github-link:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: background-color 0.3s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; }
    .card h2 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; transition: color 0.3s; }
    .info { background: #e8f5e9; border-left: 4px solid #27ae60; padding: 1rem; margin-bottom: 1.5rem; border-radius: 4px; transition: background-color 0.3s, border-color 0.3s; }
    .sensor-selection { margin-bottom: 1.5rem; padding: 1.5rem; background: #f8f9fa; border-radius: 8px; transition: background-color 0.3s; }
    .sensor-selection h3 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.125rem; transition: color 0.3s; }
    .checkbox-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 0.75rem; margin-bottom: 1rem; }
    .checkbox-grid label { display: flex; align-items: center; cursor: pointer; padding: 0.5rem; background: white; border-radius: 4px; transition: background-color 0.2s; }
    .checkbox-grid label:hover { background: #e3f2fd; }
    .checkbox-grid input[type="checkbox"] { margin-right: 0.5rem; cursor: pointer; width: 18px; height: 18px; }
    .selection-buttons { display: flex; gap: 0.5rem; }
    .select-btn { background: #f0f0f0; color: #333; border: 1px solid #ccc; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .select-btn:hover { background: #e0e0e0; border-color: #999; }
    .code-block { background: #2c3e50; color: #ecf0f1; padding: 1.5rem; border-radius: 4px; font-family: monospace; white-space: pre-wrap; word-wrap: break-word; max-height: 600px; overflow-y: auto; transition: background-color 0.3s, color 0.3s; }
    .copy-btn { background: #3498db; color: white; border: none; padding: 0.75rem 1.5rem; border-radius: 4px; cursor: pointer; font-size: 1rem; margin-top: 1rem; transition: background-color 0.3s; }
    .copy-btn:hover { background: #2980b9; }
    .copy-btn:active { background: #1c5a85; }
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1c2833; }
    body.dark-mode .card { background: #2c2c2c; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .card h2 { color: #e0e0e0; border-bottom-color: #5dade2; }
    body.dark-mode .info { background: #1e3a1e; border-left-color: #45a87d; color: #b8e6c9; }
    body.dark-mode .sensor-selection { background: #1e1e1e; }
    body.dark-mode .sensor-selection h3 { color: #e0e0e0; }
    body.dark-mode .checkbox-grid label { background: #2c2c2c; color: #e0e0e0; }
    body.dark-mode .checkbox-grid label:hover { background: #1a3a52; }
    body.dark-mode .select-btn { background: #2c2c2c; color: #e0e0e0; border-color: #444; }
    body.dark-mode .select-btn:hover { background: #3a3a3a; border-color: #666; }
    body.dark-mode .code-block { background: #1e1e1e; color: #d4d4d4; }
    body.dark-mode .copy-btn { background: #5dade2; }
    body.dark-mode .copy-btn:hover { background: #4a9fd8; }
    body.dark-mode .copy-btn:active { background: #3a8fc8; }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
        <a href="https://github.com/RAR/esphome-tigomonitor" target="_blank" class="github-link" title="View on GitHub">
          <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
        </a>
        <button class="theme-toggle" onclick="toggleTheme()">🌙</button>
      </div>
    </div>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/nodes">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml" class="active">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="card">
      <h2>YAML Configuration</h2>
      <div class="info">
        <strong>Instructions:</strong> Select the sensors you want to include in the generated configuration, then copy it to your ESPHome YAML file.
        Devices: <strong id="device-count">--</strong>
      </div>
      
      <div class="sensor-selection">
        <h3>Per-Device Sensors:</h3>
        <div class="checkbox-grid">
          <label><input type="checkbox" checked id="sel-power" onchange="updateYAML()"> Power (W)</label>
          <label><input type="checkbox" checked id="sel-peak_power" onchange="updateYAML()"> Peak Power (W)</label>
          <label><input type="checkbox" checked id="sel-voltage_in" onchange="updateYAML()"> Input Voltage (V)</label>
          <label><input type="checkbox" checked id="sel-voltage_out" onchange="updateYAML()"> Output Voltage (V)</label>
          <label><input type="checkbox" checked id="sel-current_in" onchange="updateYAML()"> Input Current (A)</label>
          <label><input type="checkbox" checked id="sel-temperature" onchange="updateYAML()"> Temperature (°C)</label>
          <label><input type="checkbox" checked id="sel-rssi" onchange="updateYAML()"> RSSI (dBm)</label>
          <label><input type="checkbox" id="sel-duty_cycle" onchange="updateYAML()"> Duty Cycle (%)</label>
          <label><input type="checkbox" id="sel-efficiency" onchange="updateYAML()"> Efficiency (%)</label>
          <label><input type="checkbox" id="sel-power_factor" onchange="updateYAML()"> Power Factor</label>
          <label><input type="checkbox" id="sel-load_factor" onchange="updateYAML()"> Load Factor</label>
          <label><input type="checkbox" id="sel-barcode" onchange="updateYAML()"> Barcode</label>
        </div>
        <div class="selection-buttons">
          <button class="select-btn" onclick="selectAllDevice()">Select All</button>
          <button class="select-btn" onclick="selectDefaultDevice()">Default Sensors</button>
          <button class="select-btn" onclick="selectNoneDevice()">Deselect All</button>
        </div>
        
        <h3 style="margin-top: 1.5rem;">Hub-Level Sensors:</h3>
        <div class="checkbox-grid">
          <label><input type="checkbox" id="sel-power_sum" onchange="updateYAML()"> Total Power (W)</label>
          <label><input type="checkbox" id="sel-energy_sum" onchange="updateYAML()"> Total Energy (kWh)</label>
          <label><input type="checkbox" id="sel-device_count" onchange="updateYAML()"> Device Count</label>
          <label><input type="checkbox" id="sel-invalid_checksum" onchange="updateYAML()"> Invalid Checksum Count</label>
          <label><input type="checkbox" id="sel-missed_frame" onchange="updateYAML()"> Missed Frame Count</label>
          <label><input type="checkbox" id="sel-internal_ram_free" onchange="updateYAML()"> Free Internal RAM</label>
          <label><input type="checkbox" id="sel-internal_ram_min" onchange="updateYAML()"> Min Free Internal RAM</label>
          <label><input type="checkbox" id="sel-psram_free" onchange="updateYAML()"> Free PSRAM</label>
          <label><input type="checkbox" id="sel-stack_free" onchange="updateYAML()"> Free Stack</label>
        </div>
        <div class="selection-buttons">
          <button class="select-btn" onclick="selectAllHub()">Select All</button>
          <button class="select-btn" onclick="selectNoneHub()">Deselect All</button>
        </div>
      </div>
      
      <pre class="code-block" id="yaml-content">Loading...</pre>
      <button class="copy-btn" onclick="copyToClipboard()">Copy to Clipboard</button>
    </div>
  </div>
  
  <script>
    // API Token configuration
    const API_TOKEN = ")html" + api_token_ + R"html(";
    
    // Fetch wrapper that includes authorization header if token is set
    async function apiFetch(url, options = {}) {
      if (API_TOKEN) {
        options.headers = options.headers || {};
        options.headers['Authorization'] = 'Bearer ' + API_TOKEN;
      }
      return fetch(url, options);
    }
    
    // Dark mode support
    let darkMode = localStorage.getItem('darkMode') === 'true';
    
    function toggleTheme() {
      darkMode = !darkMode;
      localStorage.setItem('darkMode', darkMode);
      applyTheme();
    }
    
    function applyTheme() {
      if (darkMode) {
        document.body.classList.add('dark-mode');
        document.querySelector('.theme-toggle').textContent = '☀️';
      } else {
        document.body.classList.remove('dark-mode');
        document.querySelector('.theme-toggle').textContent = '🌙';
      }
    }
    
    // Apply theme on page load
    applyTheme();
    
    function getSelectedSensors() {
      const deviceSensors = ['power', 'peak_power', 'voltage_in', 'voltage_out', 'current_in', 
                       'temperature', 'rssi', 'duty_cycle', 'efficiency', 'power_factor', 
                       'load_factor', 'barcode'];
      return deviceSensors.filter(s => document.getElementById('sel-' + s).checked);
    }
    
    function getSelectedHubSensors() {
      const hubSensors = ['power_sum', 'energy_sum', 'device_count', 'invalid_checksum', 
                          'missed_frame', 'internal_ram_free', 'internal_ram_min', 
                          'psram_free', 'stack_free'];
      return hubSensors.filter(s => document.getElementById('sel-' + s).checked);
    }
    
    function selectAllDevice() {
      const sensors = ['power', 'peak_power', 'voltage_in', 'voltage_out', 'current_in', 
                       'temperature', 'rssi', 'duty_cycle', 'efficiency', 'power_factor', 
                       'load_factor', 'barcode'];
      sensors.forEach(s => document.getElementById('sel-' + s).checked = true);
      updateYAML();
    }
    
    function selectDefaultDevice() {
      const defaultSensors = ['power', 'peak_power', 'voltage_in', 'voltage_out', 'current_in', 
                              'temperature', 'rssi'];
      const allSensors = ['power', 'peak_power', 'voltage_in', 'voltage_out', 'current_in', 
                          'temperature', 'rssi', 'duty_cycle', 'efficiency', 'power_factor', 
                          'load_factor', 'barcode'];
      allSensors.forEach(s => {
        document.getElementById('sel-' + s).checked = defaultSensors.includes(s);
      });
      updateYAML();
    }
    
    function selectNoneDevice() {
      const sensors = ['power', 'peak_power', 'voltage_in', 'voltage_out', 'current_in', 
                       'temperature', 'rssi', 'duty_cycle', 'efficiency', 'power_factor', 
                       'load_factor', 'barcode'];
      sensors.forEach(s => document.getElementById('sel-' + s).checked = false);
      updateYAML();
    }
    
    function selectAllHub() {
      const hubSensors = ['power_sum', 'energy_sum', 'device_count', 'invalid_checksum', 
                          'missed_frame', 'internal_ram_free', 'internal_ram_min', 
                          'psram_free', 'stack_free'];
      hubSensors.forEach(s => document.getElementById('sel-' + s).checked = true);
      updateYAML();
    }
    
    function selectNoneHub() {
      const hubSensors = ['power_sum', 'energy_sum', 'device_count', 'invalid_checksum', 
                          'missed_frame', 'internal_ram_free', 'internal_ram_min', 
                          'psram_free', 'stack_free'];
      hubSensors.forEach(s => document.getElementById('sel-' + s).checked = false);
      updateYAML();
    }
    
    async function loadData() {
      updateYAML();
    }
    
    async function updateYAML() {
      try {
        const selectedSensors = getSelectedSensors();
        const selectedHubSensors = getSelectedHubSensors();
        const sensorParams = selectedSensors.join(',');
        const hubParams = selectedHubSensors.join(',');
        const response = await apiFetch('/api/yaml?sensors=' + sensorParams + '&hub_sensors=' + hubParams);
        const data = await response.json();
        
        document.getElementById('yaml-content').textContent = data.yaml;
        document.getElementById('device-count').textContent = data.device_count;
      } catch (error) {
        console.error('Error loading data:', error);
        document.getElementById('yaml-content').textContent = 'Error loading configuration';
      }
    }
    
    function copyToClipboard() {
      const text = document.getElementById('yaml-content').textContent;
      navigator.clipboard.writeText(text).then(() => {
        const btn = document.querySelector('.copy-btn');
        const originalText = btn.textContent;
        btn.textContent = 'Copied!';
        setTimeout(() => {
          btn.textContent = originalText;
        }, 2000);
      });
    }
    
    loadData();
  </script>
</body>
</html>
)html");
}

void TigoWebServer::get_cca_info_html(PSRAMString& html) {
  html.append(R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - CCA Information</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s; }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .theme-toggle, .github-link { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; text-decoration: none; display: inline-flex; align-items: center; }
    .theme-toggle:hover, .github-link:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: background-color 0.3s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 1.5rem; transition: background-color 0.3s, box-shadow 0.3s; }
    .card h2 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; transition: color 0.3s; }
    .info-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 1rem; }
    .info-item { padding: 1rem; background: #f8f9fa; border-radius: 4px; transition: background-color 0.3s; }
    .info-label { font-size: 0.875rem; color: #7f8c8d; margin-bottom: 0.25rem; text-transform: uppercase; transition: color 0.3s; }
    .info-value { font-size: 1.125rem; font-weight: 600; color: #2c3e50; transition: color 0.3s; }
    .badge { display: inline-block; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; font-weight: bold; }
    .badge-success { background: #27ae60; color: white; }
    .badge-warning { background: #f39c12; color: white; }
    .badge-error { background: #e74c3c; color: white; }
    .loading { text-align: center; padding: 2rem; color: #7f8c8d; transition: color 0.3s; }
    .error { background: #e74c3c; color: white; padding: 1rem; border-radius: 4px; }
    .code-block { background: #2c3e50; color: #ecf0f1; padding: 1rem; border-radius: 4px; font-family: monospace; white-space: pre-wrap; word-wrap: break-word; max-height: 400px; overflow-y: auto; font-size: 0.875rem; transition: background-color 0.3s, color 0.3s; }
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1c2833; }
    body.dark-mode .card { background: #2c2c2c; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .card h2 { color: #e0e0e0; border-bottom-color: #5dade2; }
    body.dark-mode .info-item { background: #1e1e1e; }
    body.dark-mode .info-label { color: #a0a0a0; }
    body.dark-mode .info-value { color: #e0e0e0; }
    body.dark-mode .loading { color: #a0a0a0; }
    body.dark-mode .code-block { background: #1e1e1e; color: #d4d4d4; }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
        <a href="https://github.com/RAR/esphome-tigomonitor" target="_blank" class="github-link" title="View on GitHub">
          <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
        </a>
        <button class="theme-toggle" onclick="toggleTheme()">🌙</button>
      </div>
    </div>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/nodes">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca" class="active">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="card">
      <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
        <h2 style="margin: 0;">CCA Connection Status</h2>
        <button onclick="refreshCCA()" style="padding: 8px 16px; background-color: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px;">
          🔄 Refresh
        </button>
      </div>
      <div class="info-grid">
        <div class="info-item">
          <div class="info-label">CCA IP Address</div>
          <div class="info-value" id="cca-ip">--</div>
        </div>
        <div class="info-item">
          <div class="info-label">Last Sync</div>
          <div class="info-value" id="last-sync">--</div>
        </div>
        <div class="info-item">
          <div class="info-label">Connection Status</div>
          <div class="info-value" id="connection-status">
            <span class="badge badge-warning">Checking...</span>
          </div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>CCA Device Information</h2>
      <div id="device-info" class="loading">Loading CCA device information...</div>
    </div>
  </div>
  
  <script>
    // API Token configuration
    const API_TOKEN = ")html" + api_token_ + R"html(";
    
    // Fetch wrapper that includes authorization header if token is set
    async function apiFetch(url, options = {}) {
      if (API_TOKEN) {
        options.headers = options.headers || {};
        options.headers['Authorization'] = 'Bearer ' + API_TOKEN;
      }
      return fetch(url, options);
    }
    
    // Dark mode support
    let darkMode = localStorage.getItem('darkMode') === 'true';
    
    function toggleTheme() {
      darkMode = !darkMode;
      localStorage.setItem('darkMode', darkMode);
      applyTheme();
    }
    
    function applyTheme() {
      if (darkMode) {
        document.body.classList.add('dark-mode');
        document.querySelector('.theme-toggle').textContent = '☀️';
      } else {
        document.body.classList.remove('dark-mode');
        document.querySelector('.theme-toggle').textContent = '🌙';
      }
    }
    
    // Apply theme on page load
    applyTheme();
    
    function formatTime(seconds) {
      if (!seconds || seconds === 0 || seconds > 4294967) return 'Never'; // > ~49 days indicates invalid/never
      if (seconds < 60) return seconds + ' seconds ago';
      if (seconds < 3600) return Math.floor(seconds / 60) + ' minutes ago';
      if (seconds < 86400) return Math.floor(seconds / 3600) + ' hours ago';
      return Math.floor(seconds / 86400) + ' days ago';
    }
    
    async function loadData() {
      try {
        const response = await apiFetch('/api/cca');
        const data = await response.json();
        
        // Update connection info
        document.getElementById('cca-ip').textContent = data.cca_ip || 'Not configured';
        document.getElementById('last-sync').textContent = formatTime(data.last_sync_seconds_ago);
        
        // Parse device info
        let deviceInfo = null;
        try {
          deviceInfo = JSON.parse(data.device_info);
        } catch (e) {
          console.error('Failed to parse device info:', e);
        }
        
        if (deviceInfo && !deviceInfo.error) {
          document.getElementById('connection-status').innerHTML = '<span class="badge badge-success">Connected</span>';
          
          // Build device info grid - display all available fields
          let html = '<div class="info-grid">';
          
          // Helper function to format timestamp
          function formatTimestamp(ts) {
            if (!ts) return 'Never';
            const date = new Date(ts * 1000); // Unix timestamp to JS Date
            return date.toLocaleString();
          }
          
          // Field labels and special handling
          const fieldLabels = {
            'serial': 'Serial Number',
            'sw_version': 'Software Version',
            'sysid': 'System ID',
            'kernel_version': 'Kernel Version',
            'discovery': 'Discovery Mode',
            'fw_config_status': 'Firmware Config Status',
            'UTS': 'Uptime',
            'uts': 'Uptime',
            'sysconfig_ts': 'Last Cloud Config Sync',
            'hw_version': 'Hardware Version',
            'model': 'Model',
            'mac_address': 'MAC Address',
            'ip_address': 'IP Address',
            'subnet_mask': 'Subnet Mask',
            'gateway': 'Gateway',
            'dns': 'DNS Server',
            'ntp_server': 'NTP Server',
            'timezone': 'Timezone',
            'cpu_load': 'CPU Load',
            'memory_total': 'Total Memory',
            'memory_free': 'Free Memory',
            'disk_total': 'Total Disk',
            'disk_free': 'Free Disk',
            'temperature': 'Temperature',
            'panel_count': 'Panel Count',
            'optimizer_count': 'Optimizer Count',
            'inverter_count': 'Inverter Count',
            'string_count': 'String Count',
            'cloud_connected': 'Cloud Connected',
            'last_cloud_sync': 'Last Cloud Sync',
            'api_version': 'API Version'
          };
          
          // Process status array separately if it exists
          let statusArray = [];
          if (deviceInfo.status && Array.isArray(deviceInfo.status)) {
            statusArray = deviceInfo.status;
          }
          
          for (const [key, value] of Object.entries(deviceInfo)) {
            // Skip the status array itself - we'll expand it below
            if (key === 'status' && Array.isArray(value)) continue;
            // Skip the code field - it's an internal status code not useful to display
            if (key === 'code') continue;
            // Skip UTS/uts field - it's unreliable and doesn't represent actual uptime
            if (key === 'UTS' || key === 'uts') continue;
            
            const label = fieldLabels[key] || key.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
            let displayValue = value;
            
            // Special formatting for certain fields
            if (key === 'sysconfig_ts') {
              displayValue = formatTimestamp(value);
            } else if (key === 'discovery') {
              displayValue = value ? '<span class="badge badge-success">Active</span>' : '<span class="badge badge-warning">Inactive</span>';
            } else if (key === 'cloud_connected') {
              displayValue = value ? '<span class="badge badge-success">Yes</span>' : '<span class="badge badge-error">No</span>';
            } else if (typeof value === 'boolean') {
              displayValue = value ? '<span class="badge badge-success">Yes</span>' : '<span class="badge badge-warning">No</span>';
            } else if (typeof value === 'object') {
              displayValue = JSON.stringify(value);
            }
            
            html += '<div class="info-item"><div class="info-label">' + label + '</div><div class="info-value">' + displayValue + '</div></div>';
          }
          
          // Add status items if they exist
          // Status codes: 0 = OK/Green, 2 = Warning/Yellow, -1 = N/A/Gray, others = Error/Red
          for (const statusItem of statusArray) {
            const label = statusItem.name || 'Status';
            
            // Skip status items that duplicate main fields (S/N, S/W)
            if (label.startsWith('S/N:') || label.startsWith('S/W:')) {
              continue;
            }
            
            let badge = '';
            
            if (statusItem.status === 0) {
              badge = '<span class="badge badge-success">OK</span>';
            } else if (statusItem.status === 2) {
              badge = '<span class="badge badge-warning">Warning</span>';
            } else if (statusItem.status === -1) {
              badge = '<span style="background: #95a5a6; color: white; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; font-weight: bold; display: inline-block;">N/A</span>';
            } else {
              badge = '<span class="badge badge-error">Error</span>';
            }
            
            html += '<div class="info-item"><div class="info-label">' + label + '</div><div class="info-value">' + badge + '</div></div>';
          }
          
          html += '</div>';
          document.getElementById('device-info').innerHTML = html;
        } else {
          const errorMsg = deviceInfo && deviceInfo.error ? deviceInfo.error : 'Unknown error';
          document.getElementById('connection-status').innerHTML = '<span class="badge badge-error">Error</span>';
          document.getElementById('device-info').innerHTML = '<div class="error">Failed to retrieve CCA information: ' + errorMsg + '</div>';
        }
        
      } catch (error) {
        console.error('Error loading data:', error);
        document.getElementById('connection-status').innerHTML = '<span class="badge badge-error">Failed</span>';
        document.getElementById('device-info').innerHTML = '<div class="error">Error loading CCA information</div>';
      }
    }
    
    async function refreshCCA() {
      // Show loading state
      document.getElementById('connection-status').innerHTML = '<span class="badge badge-warning">Refreshing...</span>';
      document.getElementById('device-info').innerHTML = '<div class="loading">Refreshing CCA device information...</div>';
      
      try {
        // Trigger a fresh query by calling the sync API
        const syncResponse = await apiFetch('/api/cca/refresh');
        if (!syncResponse.ok) {
          throw new Error('Refresh request failed');
        }
        
        // Wait a moment for the query to complete
        await new Promise(resolve => setTimeout(resolve, 2000));
        
        // Load the updated data
        await loadData();
      } catch (error) {
        console.error('Error refreshing CCA data:', error);
        document.getElementById('connection-status').innerHTML = '<span class="badge badge-error">Refresh Failed</span>';
        document.getElementById('device-info').innerHTML = '<div class="error">Failed to refresh CCA information: ' + error.message + '</div>';
      }
    }
    
    loadData();
    setInterval(loadData, 30000); // Refresh every 30 seconds
  </script>
</body>
</html>
)html");
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

