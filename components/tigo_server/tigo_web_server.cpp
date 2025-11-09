#include "tigo_web_server.h"

#ifdef USE_ESP_IDF

#include "esphome/core/application.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"
#include "esphome/core/time.h"
#include "esphome/components/network/util.h"
#include "esphome/components/logger/logger.h"
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
  json_buffer.append(server->build_devices_json());
  
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
  json_buffer.append(server->build_overview_json());
  
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
  json_buffer.append(server->build_node_table_json());
  
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
  json_buffer.append(server->build_strings_json());
  
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
  json_buffer.append(server->build_inverters_json());
  
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
  json_buffer.append(server->build_esp_status_json());
  
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
  
  PSRAMString json_buffer;
  json_buffer.append(server->build_yaml_json());
  
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
  json_buffer.append(server->build_cca_info_json());
  
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
    if ((item = cJSON_GetObjectItem(node_obj, "frame09_barcode")) && cJSON_IsString(item)) {
      node.frame09_barcode = item->valuestring;
    }
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
    if (!node.addr.empty() && (!node.long_address.empty() || !node.frame09_barcode.empty())) {
      nodes.push_back(node);
      ESP_LOGD(TAG, "Imported node %zu: addr=%s, barcode=%s", nodes.size(), 
               node.addr.c_str(), 
               !node.long_address.empty() ? node.long_address.c_str() : node.frame09_barcode.c_str());
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

std::string TigoWebServer::build_devices_json() {
  std::string json = "{\"devices\":[";
  
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
      // Prefer long_address (16-char Frame 27), fallback to frame09_barcode (6-char)
      dwn.barcode = !node.long_address.empty() ? node.long_address : node.frame09_barcode;
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
    if (!first) json += ",";
    first = false;
    
    const std::string &device_name = dwn.name;
    const std::string &string_label = dwn.string_label;
    
    char buffer[600];
    
    if (dwn.has_runtime_data && dwn.device != nullptr) {
      // Device has runtime data - show actual values
      const auto &device = *dwn.device;
      float power = device.voltage_out * device.current_in;
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
    
    json += buffer;
  }
  
  json += "]}";
  return json;
}

std::string TigoWebServer::build_overview_json() {
  const auto &devices = parent_->get_devices();
  
  float total_power = 0.0f;
  float total_current = 0.0f;
  float avg_efficiency = 0.0f;
  float avg_temp = 0.0f;
  int active_devices = 0;
  
  for (const auto &device : devices) {
    float power = device.voltage_out * device.current_in;
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
  
  return std::string(buffer);
}

std::string TigoWebServer::build_strings_json() {
  const auto &strings = parent_->get_strings();
  
  ESP_LOGD(TAG, "Building strings JSON - found %d strings", strings.size());
  
  std::string json = "{\"strings\":[";
  
  bool first = true;
  
  for (const auto &pair : strings) {
    if (!first) json += ",";
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
    
    json += buffer;
  }
  
  json += "]}";
  return json;
}

std::string TigoWebServer::build_inverters_json() {
  const auto &inverters = parent_->get_inverters();
  const auto &strings = parent_->get_strings();
  
  ESP_LOGD(TAG, "Building inverters JSON - found %d inverters", inverters.size());
  
  std::string json = "{\"inverters\":[";
  
  bool first_inv = true;
  for (const auto &inverter : inverters) {
    if (!first_inv) json += ",";
    first_inv = false;
    
    ESP_LOGD(TAG, "Inverter: %s, devices: %d/%d, power: %.0fW", 
             inverter.name.c_str(),
             inverter.active_device_count, 
             inverter.total_device_count,
             inverter.total_power);
    
    // Build MPPT labels array
    std::string mppt_labels_json = "[";
    bool first_mppt = true;
    for (const auto &mppt : inverter.mppt_labels) {
      if (!first_mppt) mppt_labels_json += ",";
      first_mppt = false;
      mppt_labels_json += "\"" + mppt + "\"";
    }
    mppt_labels_json += "]";
    
    // Build strings array for this inverter
    std::string strings_json = "[";
    bool first_str = true;
    for (const auto &mppt_label : inverter.mppt_labels) {
      for (const auto &string_pair : strings) {
        const auto &string_data = string_pair.second;
        if (string_data.inverter_label == mppt_label) {
          if (!first_str) strings_json += ",";
          first_str = false;
          
          char buffer[512];
          snprintf(buffer, sizeof(buffer),
            "{\"label\":\"%s\",\"mppt\":\"%s\",\"total_power\":%.1f,\"peak_power\":%.1f,"
            "\"active_devices\":%d,\"total_devices\":%d}",
            string_data.string_label.c_str(), string_data.inverter_label.c_str(),
            string_data.total_power, string_data.peak_power,
            string_data.active_device_count, string_data.total_device_count);
          strings_json += buffer;
        }
      }
    }
    strings_json += "]";
    
    // Build the inverter JSON object (don't use snprintf to avoid truncation)
    json += "{\"name\":\"" + inverter.name + "\",";
    json += "\"mppts\":" + mppt_labels_json + ",";
    json += "\"total_power\":" + std::to_string(inverter.total_power) + ",";
    json += "\"peak_power\":" + std::to_string(inverter.peak_power) + ",";
    json += "\"active_devices\":" + std::to_string(inverter.active_device_count) + ",";
    json += "\"total_devices\":" + std::to_string(inverter.total_device_count) + ",";
    json += "\"strings\":" + strings_json + "}";
  }
  
  json += "]}";
  return json;
}

std::string TigoWebServer::build_node_table_json() {
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
    cJSON_AddStringToObject(node_obj, "frame09_barcode", node.frame09_barcode.c_str());
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
  std::string result(json_str);
  
  cJSON_free(json_str);
  cJSON_Delete(root);
  
  // Reset to default allocators
  if (using_psram) {
    cJSON_InitHooks(NULL);
  }
  
  return result;
}

std::string TigoWebServer::build_esp_status_json() {
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
  uint32_t missed_packets = parent_->get_missed_packet_count();
  
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
    "\"invalid_checksum\":%u,\"missed_packets\":%u,"
    "\"network_connected\":%s,\"wifi_rssi\":%d,\"wifi_ssid\":\"%s\",\"ip_address\":\"%s\",\"mac_address\":\"%s\","
    "\"active_sockets\":%d,\"max_sockets\":%d}",
    free_heap, total_heap, free_psram, total_psram,
    min_free_heap, min_free_psram,
    uptime_sec, uptime_days, uptime_hours, uptime_mins,
    ESPHOME_VERSION, __DATE__, __TIME__,
    (unsigned int)task_count, internal_temp,
    invalid_checksum, missed_packets,
    network_connected ? "true" : "false", wifi_rssi, ssid.c_str(), ip_address.c_str(), mac_address.c_str(),
    active_sockets, max_sockets);
  
  return std::string(buffer);
}

std::string TigoWebServer::build_yaml_json() {
  std::string yaml_text;
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
  
  yaml_text = "sensor:\n";
  
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
      } else if (!node.frame09_barcode.empty()) {
        barcode_comment = " - Frame09: " + node.frame09_barcode;
      }
    }
    
    yaml_text += "  # " + device_name + " (discovered" + barcode_comment + ")\n";
    yaml_text += "  - platform: tigo_monitor\n";
    yaml_text += "    tigo_monitor_id: tigo_hub\n";
    yaml_text += "    address: \"" + node.addr + "\"\n";
    yaml_text += "    name: \"" + device_name + "\"\n";
    yaml_text += "    power: {}\n";
    yaml_text += "    peak_power: {}\n";
    yaml_text += "    voltage_in: {}\n";
    yaml_text += "    voltage_out: {}\n";
    yaml_text += "    current_in: {}\n";
    yaml_text += "    temperature: {}\n";
    yaml_text += "    rssi: {}\n";
    yaml_text += "\n";
  }
  
  // Escape for JSON - convert newlines to \n
  std::string json = "{\"yaml\":\"";
  for (char c : yaml_text) {
    if (c == '"') json += "\\\"";
    else if (c == '\\') json += "\\\\";
    else if (c == '\n') json += "\\n";
    else if (c == '\r') json += "\\r";
    else if (c == '\t') json += "\\t";
    else json += c;
  }
  json += "\",\"device_count\":" + std::to_string(assigned_nodes.size()) + "}";
  
  return json;
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
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; transition: background-color 0.3s, color 0.3s; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header-top { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    .header h1 { font-size: 1.5rem; margin: 0; }
    .header-controls { display: flex; gap: 0.5rem; }
    .temp-toggle, .theme-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .temp-toggle:hover, .theme-toggle:hover { background: rgba(255,255,255,0.2); }
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
    
    /* Dark mode styles */
    body.dark-mode { background: #1a1a1a; color: #e0e0e0; }
    body.dark-mode .header { background: #1e1e1e; }
    body.dark-mode .stat-card, body.dark-mode .device-card { background: #2d2d2d; box-shadow: 0 2px 4px rgba(0,0,0,0.3); }
    body.dark-mode .stat-card h3, body.dark-mode .metric-label, body.dark-mode .device-subtitle { color: #b0b0b0; }
    body.dark-mode .stat-card .value, body.dark-mode .metric-value, body.dark-mode .device-title { color: #e0e0e0; }
    body.dark-mode .metric { background: #3a3a3a; }
    body.dark-mode .device-header { border-bottom-color: #444; }
    body.dark-mode .loading { color: #b0b0b0; }
    body.dark-mode .string-summary { box-shadow: 0 4px 6px rgba(0,0,0,0.5); }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div class="header-controls">
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
    
    // Initialize toggle buttons
    let invertersData = { inverters: [] }; // Cache inverter config (static, only changes on reboot)
    
    document.addEventListener('DOMContentLoaded', () => {
      applyTheme();
      document.getElementById('temp-toggle').textContent = useFahrenheit ? '°C' : '°F';
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
    
    loadData();
    setInterval(loadData, 5000);
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
    .theme-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .theme-toggle:hover { background: rgba(255,255,255,0.2); }
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
    body.dark-mode .header { background: #1e1e1e; }
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
      <button class="theme-toggle" onclick="toggleTheme()" id="theme-toggle">🌙</button>
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
    .header-top { display: flex; justify-content: space-between; align-items: center; }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .theme-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .theme-toggle:hover { background: rgba(255,255,255,0.2); }
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
    .temp-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; margin-left: 1rem; }
    .temp-toggle:hover { background: rgba(255,255,255,0.2); }
  </style>
</head>
<body>
  <div class="header">
    <div class="header-top">
      <h1>🌞 Tigo Solar Monitor</h1>
      <div>
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
          <h3>Missed Packets</h3>
          <div class="value" id="missed-packets">--</div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2>Actions</h2>
      <div style="display: flex; gap: 1rem; flex-wrap: wrap;">
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
      <div id="restart-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
      <div id="reset-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
      <div id="node-reset-message" style="margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none;"></div>
    </div>
    
    <div class="card">
      <h2>System Logs</h2>
      <div style="margin-bottom: 1rem; display: flex; gap: 0.5rem; flex-wrap: wrap;">
        <button id="log-pause-btn" onclick="toggleLogPause()" style="padding: 8px 16px; background-color: #27ae60; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px;">
          ▶️ Resume
        </button>
        <button onclick="clearLogs()" style="padding: 8px 16px; background-color: #95a5a6; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px;">
          🗑️ Clear
        </button>
        <button onclick="toggleAutoScroll()" id="autoscroll-btn" style="padding: 8px 16px; background-color: #27ae60; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px;">
          📜 Auto-scroll: ON
        </button>
        <button onclick="downloadLogs()" style="padding: 8px 16px; background-color: #8e44ad; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px;">
          💾 Download
        </button>
        <select id="log-level-filter" onchange="filterLogsByLevel()" style="padding: 8px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px;">
          <option value="all">All Levels</option>
          <option value="ERROR">ERROR</option>
          <option value="WARN">WARN</option>
          <option value="INFO">INFO</option>
          <option value="DEBUG">DEBUG</option>
          <option value="VERBOSE">VERBOSE</option>
        </select>
        <input type="text" id="log-search" onkeyup="searchLogs()" placeholder="Search logs..." style="padding: 8px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px; flex: 1; min-width: 200px;">
      </div>
      <div id="log-console" style="background: #1e1e1e; color: #d4d4d4; padding: 1rem; border-radius: 4px; font-family: 'Courier New', monospace; font-size: 12px; height: 400px; overflow-y: auto; white-space: pre-wrap; word-wrap: break-word;"></div>
      <div id="ws-status" style="margin-top: 0.5rem; font-size: 12px; color: #7f8c8d;">
        <span id="ws-status-text">⏸️ Log streaming paused - click Resume to start</span>
      </div>
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
        } else {
          document.getElementById('psram-free').textContent = 'Not available';
          document.getElementById('psram-progress').style.width = '0%';
        }
        
        // Display system information
        document.getElementById('task-count').textContent = data.task_count || '--';
        document.getElementById('internal-temp').textContent = 
          data.internal_temp !== undefined ? `${formatTemperature(data.internal_temp)}${getTempUnit()}` : '--';
        document.getElementById('min-heap').textContent = formatBytes(data.min_free_heap || 0);
        document.getElementById('invalid-checksum').textContent = data.invalid_checksum || 0;
        document.getElementById('missed-packets').textContent = data.missed_packets || 0;
        
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
    let logPaused = true;  // Start paused by default to reduce load
    let autoScroll = true;
    let allLogs = [];
    let currentFilter = 'all';
    let searchTerm = '';
    let lastLogIndex = 0;
    let logPollInterval = null;
    
    async function fetchLogs() {
      if (logPaused) return;
      
      try {
        const response = await apiFetch(`/api/logs?start=${lastLogIndex}`);
        const data = await response.json();
        
        // Check if log streaming is disabled (no PSRAM)
        if (data.error) {
          document.getElementById('ws-status-text').textContent = '⚠️ ' + data.error;
          document.getElementById('ws-status-text').style.color = '#e67e22';
          
          // Disable the resume button
          const btn = document.getElementById('pause-log-btn');
          btn.disabled = true;
          btn.textContent = '🚫 Not Available';
          btn.style.backgroundColor = '#95a5a6';
          
          // Stop polling
          if (logPollInterval) {
            clearInterval(logPollInterval);
            logPollInterval = null;
          }
          return;
        }
        
        if (data.logs && data.logs.length > 0) {
          allLogs.push(...data.logs);
          
          // Keep only last 1000 logs
          if (allLogs.length > 1000) {
            allLogs = allLogs.slice(-1000);
          }
          
          lastLogIndex = data.current_index;
          renderLogs();
        }
        
        document.getElementById('ws-status-text').textContent = '✅ Connected (polling)';
        document.getElementById('ws-status-text').style.color = '#27ae60';
      } catch (error) {
        console.error('Failed to fetch logs:', error);
        document.getElementById('ws-status-text').textContent = '❌ Connection error';
        document.getElementById('ws-status-text').style.color = '#e74c3c';
      }
    }
    
    async function loadInitialLogs() {
      try {
        const response = await apiFetch('/api/logs?start=0');
        const data = await response.json();
        
        allLogs = data.logs || [];
        lastLogIndex = data.current_index || 0;
        renderLogs();
        
        document.getElementById('ws-status-text').textContent = '✅ Connected (polling)';
        document.getElementById('ws-status-text').style.color = '#27ae60';
      } catch (error) {
        console.error('Failed to load initial logs:', error);
        document.getElementById('ws-status-text').textContent = '❌ Failed to load logs';
        document.getElementById('ws-status-text').style.color = '#e74c3c';
      }
    }
    
    function renderLogs() {
      const console = document.getElementById('log-console');
      
      // Filter logs
      let filteredLogs = allLogs;
      
      if (currentFilter !== 'all') {
        filteredLogs = filteredLogs.filter(log => log.level === currentFilter);
      }
      
      if (searchTerm) {
        const term = searchTerm.toLowerCase();
        filteredLogs = filteredLogs.filter(log => 
          log.tag.toLowerCase().includes(term) || 
          log.message.toLowerCase().includes(term)
        );
      }
      
      // Render logs
      let html = '';
      filteredLogs.forEach(log => {
        const time = new Date(log.timestamp).toLocaleTimeString();
        const color = getLogColor(log.level);
        html += `<span style="color: ${color};">[${time}] [${log.level}] [${log.tag}] ${log.message}</span>\n`;
      });
      
      console.innerHTML = html || '<span style="color: #7f8c8d;">No logs to display</span>';
      
      // Auto-scroll
      if (autoScroll && !logPaused) {
        console.scrollTop = console.scrollHeight;
      }
    }
    
    function getLogColor(level) {
      switch(level) {
        case 'ERROR': return '#e74c3c';
        case 'WARN': return '#f39c12';
        case 'INFO': return '#3498db';
        case 'DEBUG': return '#95a5a6';
        case 'VERBOSE': return '#7f8c8d';
        default: return '#d4d4d4';
      }
    }
    
    function toggleLogPause() {
      logPaused = !logPaused;
      const btn = document.getElementById('log-pause-btn');
      btn.textContent = logPaused ? '▶️ Resume' : '⏸️ Pause';
      btn.style.backgroundColor = logPaused ? '#27ae60' : '#3498db';
      
      // When resuming, load initial logs and start polling if not already started
      if (!logPaused) {
        if (allLogs.length === 0) {
          loadInitialLogs();
        }
        if (!logPollInterval) {
          logPollInterval = setInterval(fetchLogs, 1000);  // Poll every second
        }
        document.getElementById('ws-status-text').textContent = '✅ Connected (polling)';
        document.getElementById('ws-status-text').style.color = '#27ae60';
      } else {
        // When paused, update status
        document.getElementById('ws-status-text').textContent = '⏸️ Log streaming paused';
        document.getElementById('ws-status-text').style.color = '#95a5a6';
      }
    }
    
    async function clearLogs() {
      try {
        await apiFetch('/api/logs/clear', { method: 'POST' });
        allLogs = [];
        lastLogIndex = 0;
        renderLogs();
      } catch (error) {
        console.error('Failed to clear logs:', error);
      }
    }
    
    function toggleAutoScroll() {
      autoScroll = !autoScroll;
      const btn = document.getElementById('autoscroll-btn');
      btn.textContent = autoScroll ? '📜 Auto-scroll: ON' : '📜 Auto-scroll: OFF';
      btn.style.backgroundColor = autoScroll ? '#27ae60' : '#95a5a6';
    }
    
    function filterLogsByLevel() {
      currentFilter = document.getElementById('log-level-filter').value;
      renderLogs();
    }
    
    function searchLogs() {
      searchTerm = document.getElementById('log-search').value;
      renderLogs();
    }
    
    function downloadLogs() {
      const content = allLogs.map(log => {
        const time = new Date(log.timestamp).toISOString();
        return `[${time}] [${log.level}] [${log.tag}] ${log.message}`;
      }).join('\n');
      
      const blob = new Blob([content], { type: 'text/plain' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `esp32-logs-${new Date().toISOString().replace(/:/g, '-')}.txt`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    }
    
    // Initialize log system - but don't start polling yet (paused by default)
    // Load initial logs on demand when user clicks Resume
    // logPollInterval will be started by toggleLogPause() when user resumes
    
    // Check on page load if log streaming is available
    async function checkLogAvailability() {
      try {
        const response = await apiFetch('/api/logs/status');
        const data = await response.json();
        if (!data.enabled) {
          // Log streaming disabled
          document.getElementById('ws-status-text').textContent = '⚠️ Log streaming not available - requires PSRAM';
          document.getElementById('ws-status-text').style.color = '#e67e22';
          
          const btn = document.getElementById('pause-log-btn');
          btn.disabled = true;
          btn.textContent = '🚫 Not Available';
          btn.style.backgroundColor = '#95a5a6';
        }
      } catch (error) {
        console.error('Failed to check log availability:', error);
      }
    }
    checkLogAvailability();
    
    loadData();
    setInterval(loadData, 5000);
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
    .header-top { display: flex; justify-content: space-between; align-items: center; }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .theme-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .theme-toggle:hover { background: rgba(255,255,255,0.2); }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; transition: background-color 0.3s; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transition: background-color 0.3s, box-shadow 0.3s; }
    .card h2 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; transition: color 0.3s; }
    .info { background: #e8f5e9; border-left: 4px solid #27ae60; padding: 1rem; margin-bottom: 1.5rem; border-radius: 4px; transition: background-color 0.3s, border-color 0.3s; }
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
      <button class="theme-toggle" onclick="toggleTheme()">🌙</button>
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
        <strong>Instructions:</strong> Copy the configuration below and add it to your ESPHome YAML file. 
        This configuration is automatically generated based on discovered devices.
        Devices: <strong id="device-count">--</strong>
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
    
    async function loadData() {
      try {
        const response = await apiFetch('/api/yaml');
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
    .header-top { display: flex; justify-content: space-between; align-items: center; }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .theme-toggle { background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.3); color: white; padding: 0.5rem 1rem; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: all 0.2s; }
    .theme-toggle:hover { background: rgba(255,255,255,0.2); }
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
      <button class="theme-toggle" onclick="toggleTheme()">🌙</button>
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
          
          // Helper function to format uptime from milliseconds
          function formatUptime(ms) {
            const seconds = Math.floor(ms / 1000);
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;
            
            let parts = [];
            if (days > 0) parts.push(days + 'd');
            if (hours > 0) parts.push(hours + 'h');
            if (minutes > 0) parts.push(minutes + 'm');
            if (secs > 0 || parts.length === 0) parts.push(secs + 's');
            return parts.join(' ');
          }
          
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
            
            const label = fieldLabels[key] || key.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
            let displayValue = value;
            
            // Special formatting for certain fields
            if (key === 'UTS' || key === 'uts') {
              displayValue = formatUptime(value);
            } else if (key === 'sysconfig_ts') {
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

std::string TigoWebServer::build_cca_info_json() {
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
  
  std::string json = "{";
  json += "\"cca_ip\":\"" + parent_->get_cca_ip() + "\",";
  json += "\"last_sync_seconds_ago\":" + std::to_string(seconds_ago) + ",";
  json += "\"device_info\":";
  
  // Embed the device info JSON (already a JSON string)
  const std::string &device_info = parent_->get_cca_device_info();
  if (device_info.empty()) {
    json += "\"{}\"";
  } else {
    // Escape the JSON string for embedding
    json += "\"";
    for (char c : device_info) {
      if (c == '"') json += "\\\"";
      else if (c == '\\') json += "\\\\";
      else if (c == '\n') json += "\\n";
      else if (c == '\r') json += "\\r";
      else if (c == '\t') json += "\\t";
      else json += c;
    }
    json += "\"";
  }
  
  json += "}";
  return json;
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

