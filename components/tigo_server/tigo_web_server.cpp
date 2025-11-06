#include "tigo_web_server.h"

#ifdef USE_ESP_IDF

#include "esphome/core/application.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"
#include <esp_heap_caps.h>
#include <esp_system.h>

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
    ESP_LOGW(TAG, "PSRAM not available - large buffers will use regular heap");
    ESP_LOGW(TAG, "If your ESP32 has PSRAM, ensure it's properly configured in the YAML");
  }
  
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port_;
  config.ctrl_port = port_ + 1;
  config.max_uri_handlers = 16;
  config.stack_size = 8192;
  
  if (httpd_start(&server_, &config) == ESP_OK) {
    ESP_LOGI(TAG, "Web server started successfully");
    
    // Register HTML page handlers
    httpd_uri_t dashboard_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = dashboard_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &dashboard_uri);
    
    httpd_uri_t overview_uri = {
      .uri = "/overview",
      .method = HTTP_GET,
      .handler = overview_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &overview_uri);
    
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
    
    ESP_LOGI(TAG, "All routes registered");
  } else {
    ESP_LOGE(TAG, "Failed to start web server");
  }
}

tigo_monitor::TigoMonitorComponent *TigoWebServer::get_parent_from_req(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  return server->parent_;
}

// ===== HTML Page Handlers =====

esp_err_t TigoWebServer::dashboard_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  // Use PSRAM for large HTML content
  PSRAMString html;
  std::string content = server->get_dashboard_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::overview_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString html;
  std::string content = server->get_overview_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::node_table_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString html;
  std::string content = server->get_node_table_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::esp_status_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString html;
  std::string content = server->get_esp_status_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::yaml_config_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString html;
  std::string content = server->get_yaml_config_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

// ===== API Handlers (JSON) =====

esp_err_t TigoWebServer::api_devices_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_devices_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_overview_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_overview_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_node_table_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_node_table_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_esp_status_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_esp_status_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_yaml_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_yaml_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::cca_info_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString html;
  std::string content = server->get_cca_info_html();
  html.append(content);
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t TigoWebServer::api_cca_info_handler(httpd_req_t *req) {
  TigoWebServer *server = static_cast<TigoWebServer *>(req->user_ctx);
  
  PSRAMString json_buffer;
  std::string json = server->build_cca_info_json();
  json_buffer.append(json);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_buffer.c_str(), json_buffer.length());
  return ESP_OK;
}

// ===== JSON Builders =====

std::string TigoWebServer::build_devices_json() {
  std::string json = "{\"devices\":[";
  
  const auto &devices = parent_->get_devices();
  bool first = true;
  
  for (const auto &device : devices) {
    if (!first) json += ",";
    first = false;
    
    char buffer[512];
    float power = device.voltage_out * device.current_in;
    unsigned long data_age_ms = millis() - device.last_update;
    
    snprintf(buffer, sizeof(buffer),
      "{\"addr\":\"%s\",\"barcode\":\"%s\",\"voltage_in\":%.2f,\"voltage_out\":%.2f,"
      "\"current\":%.3f,\"power\":%.1f,\"temperature\":%.1f,\"rssi\":%d,"
      "\"duty_cycle\":%u,\"efficiency\":%.2f,\"data_age_ms\":%lu}",
      device.addr.c_str(), device.barcode.c_str(), device.voltage_in, device.voltage_out,
      device.current_in, power, device.temperature, device.rssi,
      device.duty_cycle, device.efficiency, data_age_ms);
    
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
  
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
    "{\"total_power\":%.1f,\"total_current\":%.3f,\"avg_efficiency\":%.2f,"
    "\"avg_temperature\":%.1f,\"active_devices\":%d,\"max_devices\":%d}",
    total_power, total_current, avg_efficiency, avg_temp,
    active_devices, parent_->get_number_of_devices());
  
  return std::string(buffer);
}

std::string TigoWebServer::build_node_table_json() {
  std::string json = "{\"nodes\":[";
  
  const auto &node_table = parent_->get_node_table();
  bool first = true;
  
  for (const auto &node : node_table) {
    if (!first) json += ",";
    first = false;
    
    char buffer[1024];
    snprintf(buffer, sizeof(buffer),
      "{\"addr\":\"%s\",\"long_address\":\"%s\",\"frame09_barcode\":\"%s\","
      "\"sensor_index\":%d,\"checksum\":\"%s\","
      "\"cca_validated\":%s,\"cca_label\":\"%s\",\"cca_string\":\"%s\",\"cca_inverter\":\"%s\",\"cca_channel\":\"%s\"}",
      node.addr.c_str(), node.long_address.c_str(), node.frame09_barcode.c_str(),
      node.sensor_index, node.checksum.c_str(),
      node.cca_validated ? "true" : "false",
      node.cca_label.c_str(), node.cca_string_label.c_str(),
      node.cca_inverter_label.c_str(), node.cca_channel.c_str());
    
    json += buffer;
  }
  
  json += "]}";
  return json;
}

std::string TigoWebServer::build_esp_status_json() {
  char buffer[1024];
  
  // Get heap info
  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  
  // Get uptime
  uint32_t uptime_sec = millis() / 1000;
  uint32_t uptime_days = uptime_sec / 86400;
  uint32_t uptime_hours = (uptime_sec % 86400) / 3600;
  uint32_t uptime_mins = (uptime_sec % 3600) / 60;
  
  snprintf(buffer, sizeof(buffer),
    "{\"free_heap\":%zu,\"total_heap\":%zu,\"free_psram\":%zu,\"total_psram\":%zu,"
    "\"uptime_sec\":%u,\"uptime_days\":%u,\"uptime_hours\":%u,\"uptime_mins\":%u,"
    "\"esphome_version\":\"%s\",\"compilation_time\":\"%s %s\"}",
    free_heap, total_heap, free_psram, total_psram,
    uptime_sec, uptime_days, uptime_hours, uptime_mins,
    ESPHOME_VERSION, __DATE__, __TIME__);
  
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

std::string TigoWebServer::get_dashboard_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - Dashboard</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1400px; margin: 2rem auto; padding: 0 1rem; }
    .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; margin-bottom: 2rem; }
    .stat-card { background: white; padding: 1.5rem; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .stat-card h3 { font-size: 0.875rem; color: #7f8c8d; margin-bottom: 0.5rem; text-transform: uppercase; }
    .stat-card .value { font-size: 2rem; font-weight: bold; color: #2c3e50; }
    .stat-card .unit { font-size: 1rem; color: #95a5a6; margin-left: 0.25rem; }
    .devices-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(350px, 1fr)); gap: 1rem; }
    .device-card { background: white; border-radius: 8px; padding: 1.5rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .device-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; border-bottom: 2px solid #ecf0f1; padding-bottom: 0.75rem; }
    .device-title { font-size: 1.125rem; font-weight: bold; color: #2c3e50; }
    .device-badge { background: #27ae60; color: white; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; }
    .device-metrics { display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.75rem; }
    .metric { display: flex; justify-content: space-between; padding: 0.5rem; background: #f8f9fa; border-radius: 4px; }
    .metric-label { color: #7f8c8d; font-size: 0.875rem; }
    .metric-value { font-weight: 600; color: #2c3e50; }
    .loading { text-align: center; padding: 2rem; color: #7f8c8d; }
    .error { background: #e74c3c; color: white; padding: 1rem; border-radius: 4px; margin-bottom: 1rem; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/" class="active">Dashboard</a>
      <a href="/overview">Overview</a>
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
        <h3>Active Devices</h3>
        <div><span class="value" id="active-devices">--</span></div>
      </div>
      <div class="stat-card">
        <h3>Avg Efficiency</h3>
        <div><span class="value" id="avg-efficiency">--</span><span class="unit">%</span></div>
      </div>
      <div class="stat-card">
        <h3>Avg Temperature</h3>
        <div><span class="value" id="avg-temp">--</span><span class="unit">°C</span></div>
      </div>
    </div>
    
    <div class="devices-grid" id="devices"></div>
  </div>
  
  <script>
    async function loadData() {
      try {
        const [devicesRes, overviewRes] = await Promise.all([
          fetch('/api/devices'),
          fetch('/api/overview')
        ]);
        
        const devicesData = await devicesRes.json();
        const overviewData = await overviewRes.json();
        
        // Update stats
        document.getElementById('total-power').textContent = overviewData.total_power.toFixed(1);
        document.getElementById('active-devices').textContent = overviewData.active_devices;
        document.getElementById('avg-efficiency').textContent = overviewData.avg_efficiency.toFixed(1);
        document.getElementById('avg-temp').textContent = overviewData.avg_temperature.toFixed(1);
        
        // Update devices
        const devicesContainer = document.getElementById('devices');
        devicesContainer.innerHTML = devicesData.devices.map(device => {
          const ageText = device.data_age_ms < 1000 ? `${device.data_age_ms}ms` : 
                          device.data_age_ms < 60000 ? `${(device.data_age_ms/1000).toFixed(1)}s` :
                          `${(device.data_age_ms/60000).toFixed(1)}m`;
          
          return `
            <div class="device-card">
              <div class="device-header">
                <div class="device-title">${device.barcode || device.addr}</div>
                <div class="device-badge">${ageText}</div>
              </div>
              <div class="device-metrics">
                <div class="metric">
                  <span class="metric-label">Power</span>
                  <span class="metric-value">${device.power.toFixed(1)} W</span>
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
                  <span class="metric-value">${device.temperature.toFixed(1)} °C</span>
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
        }).join('');
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    loadData();
    setInterval(loadData, 5000);
  </script>
</body>
</html>
)html";
}

std::string TigoWebServer::get_overview_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - Overview</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .overview-grid { display: grid; gap: 1.5rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .card h2 { color: #2c3e50; margin-bottom: 1.5rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    .metric-row { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 1rem; margin-bottom: 1rem; }
    .metric-item { background: #f8f9fa; padding: 1.5rem; border-radius: 4px; }
    .metric-item h3 { color: #7f8c8d; font-size: 0.875rem; margin-bottom: 0.5rem; text-transform: uppercase; }
    .metric-item .value { font-size: 2.5rem; font-weight: bold; color: #2c3e50; }
    .metric-item .unit { font-size: 1.25rem; color: #95a5a6; margin-left: 0.5rem; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/overview" class="active">Overview</a>
      <a href="/nodes">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="overview-grid">
      <div class="card">
        <h2>System Overview</h2>
        <div class="metric-row">
          <div class="metric-item">
            <h3>Total Power Output</h3>
            <div><span class="value" id="total-power">--</span><span class="unit">W</span></div>
          </div>
          <div class="metric-item">
            <h3>Total Current</h3>
            <div><span class="value" id="total-current">--</span><span class="unit">A</span></div>
          </div>
        </div>
        <div class="metric-row">
          <div class="metric-item">
            <h3>Average Efficiency</h3>
            <div><span class="value" id="avg-efficiency">--</span><span class="unit">%</span></div>
          </div>
          <div class="metric-item">
            <h3>Average Temperature</h3>
            <div><span class="value" id="avg-temp">--</span><span class="unit">°C</span></div>
          </div>
        </div>
        <div class="metric-row">
          <div class="metric-item">
            <h3>Active Devices</h3>
            <div><span class="value" id="active-devices">--</span><span class="unit">/ <span id="max-devices">--</span></span></div>
          </div>
        </div>
      </div>
    </div>
  </div>
  
  <script>
    async function loadData() {
      try {
        const response = await fetch('/api/overview');
        const data = await response.json();
        
        document.getElementById('total-power').textContent = data.total_power.toFixed(1);
        document.getElementById('total-current').textContent = data.total_current.toFixed(3);
        document.getElementById('avg-efficiency').textContent = data.avg_efficiency.toFixed(1);
        document.getElementById('avg-temp').textContent = data.avg_temperature.toFixed(1);
        document.getElementById('active-devices').textContent = data.active_devices;
        document.getElementById('max-devices').textContent = data.max_devices;
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    loadData();
    setInterval(loadData, 5000);
  </script>
</body>
</html>
)html";
}

std::string TigoWebServer::get_node_table_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - Node Table</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1400px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .card h2 { color: #2c3e50; margin-bottom: 1.5rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    table { width: 100%; border-collapse: collapse; }
    thead { background: #34495e; color: white; }
    th { padding: 1rem; text-align: left; font-weight: 600; }
    td { padding: 1rem; border-bottom: 1px solid #ecf0f1; }
    tbody tr:hover { background: #f8f9fa; }
    .badge { display: inline-block; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; font-weight: 600; }
    .badge-success { background: #27ae60; color: white; }
    .badge-warning { background: #f39c12; color: white; }
    .badge-info { background: #3498db; color: white; }
    .code { font-family: monospace; background: #f8f9fa; padding: 0.25rem 0.5rem; border-radius: 4px; }
    .cca-info { color: #16a085; font-weight: 500; }
    .hierarchy { color: #95a5a6; font-size: 0.85rem; margin-top: 0.25rem; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/overview">Overview</a>
      <a href="/nodes" class="active">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="card">
      <h2>Node Table</h2>
      <table>
        <thead>
          <tr>
            <th>Address</th>
            <th>Device Name / Barcode</th>
            <th>Location</th>
            <th>Sensor Index</th>
            <th>Checksum</th>
          </tr>
        </thead>
        <tbody id="node-table-body">
          <tr><td colspan="5" style="text-align:center;">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>
  
  <script>
    async function loadData() {
      try {
        const response = await fetch('/api/nodes');
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
            <td><span class="code">${node.checksum || '-'}</span></td>
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
)html";
}

std::string TigoWebServer::get_esp_status_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - ESP32 Status</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 1.5rem; }
    .card h2 { color: #2c3e50; margin-bottom: 1.5rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    .info-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1rem; }
    .info-item { background: #f8f9fa; padding: 1.5rem; border-radius: 4px; }
    .info-item h3 { color: #7f8c8d; font-size: 0.875rem; margin-bottom: 0.5rem; text-transform: uppercase; }
    .info-item .value { font-size: 1.5rem; font-weight: bold; color: #2c3e50; }
    .progress-bar { width: 100%; height: 20px; background: #ecf0f1; border-radius: 10px; overflow: hidden; margin-top: 0.5rem; }
    .progress-fill { height: 100%; background: #27ae60; transition: width 0.3s; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/overview">Overview</a>
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
      </div>
    </div>
    
    <div class="card">
      <h2>Memory</h2>
      <div class="info-grid">
        <div class="info-item">
          <h3>Heap Memory</h3>
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
  </div>
  
  <script>
    function formatBytes(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }
    
    async function loadData() {
      try {
        const response = await fetch('/api/status');
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
        }
      } catch (error) {
        console.error('Error loading data:', error);
      }
    }
    
    loadData();
    setInterval(loadData, 5000);
  </script>
</body>
</html>
)html";
}

std::string TigoWebServer::get_yaml_config_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - YAML Configuration</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .card h2 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    .info { background: #e8f5e9; border-left: 4px solid #27ae60; padding: 1rem; margin-bottom: 1.5rem; border-radius: 4px; }
    .code-block { background: #2c3e50; color: #ecf0f1; padding: 1.5rem; border-radius: 4px; font-family: monospace; white-space: pre-wrap; word-wrap: break-word; max-height: 600px; overflow-y: auto; }
    .copy-btn { background: #3498db; color: white; border: none; padding: 0.75rem 1.5rem; border-radius: 4px; cursor: pointer; font-size: 1rem; margin-top: 1rem; }
    .copy-btn:hover { background: #2980b9; }
    .copy-btn:active { background: #1c5a85; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/overview">Overview</a>
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
    async function loadData() {
      try {
        const response = await fetch('/api/yaml');
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
)html";
}

std::string TigoWebServer::get_cca_info_html() {
  return R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Tigo Monitor - CCA Information</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
    .header { background: #2c3e50; color: white; padding: 1rem 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
    .header h1 { font-size: 1.5rem; margin-bottom: 0.5rem; }
    .nav { display: flex; gap: 1rem; margin-top: 0.5rem; }
    .nav a { color: #3498db; text-decoration: none; padding: 0.5rem 1rem; background: rgba(255,255,255,0.1); border-radius: 4px; }
    .nav a:hover { background: rgba(255,255,255,0.2); }
    .nav a.active { background: #3498db; color: white; }
    .container { max-width: 1200px; margin: 2rem auto; padding: 0 1rem; }
    .card { background: white; border-radius: 8px; padding: 2rem; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 1.5rem; }
    .card h2 { color: #2c3e50; margin-bottom: 1rem; font-size: 1.5rem; border-bottom: 2px solid #3498db; padding-bottom: 0.5rem; }
    .info-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 1rem; }
    .info-item { padding: 1rem; background: #f8f9fa; border-radius: 4px; }
    .info-label { font-size: 0.875rem; color: #7f8c8d; margin-bottom: 0.25rem; text-transform: uppercase; }
    .info-value { font-size: 1.125rem; font-weight: 600; color: #2c3e50; }
    .badge { display: inline-block; padding: 0.25rem 0.75rem; border-radius: 12px; font-size: 0.75rem; font-weight: bold; }
    .badge-success { background: #27ae60; color: white; }
    .badge-warning { background: #f39c12; color: white; }
    .badge-error { background: #e74c3c; color: white; }
    .loading { text-align: center; padding: 2rem; color: #7f8c8d; }
    .error { background: #e74c3c; color: white; padding: 1rem; border-radius: 4px; }
    .code-block { background: #2c3e50; color: #ecf0f1; padding: 1rem; border-radius: 4px; font-family: monospace; white-space: pre-wrap; word-wrap: break-word; max-height: 400px; overflow-y: auto; font-size: 0.875rem; }
  </style>
</head>
<body>
  <div class="header">
    <h1>🌞 Tigo Solar Monitor</h1>
    <div class="nav">
      <a href="/">Dashboard</a>
      <a href="/overview">Overview</a>
      <a href="/nodes">Node Table</a>
      <a href="/status">ESP32 Status</a>
      <a href="/yaml">YAML Config</a>
      <a href="/cca" class="active">CCA Info</a>
    </div>
  </div>
  
  <div class="container">
    <div class="card">
      <h2>CCA Connection Status</h2>
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
    
    <div class="card">
      <h2>Raw JSON Response</h2>
      <pre class="code-block" id="json-response">Loading...</pre>
    </div>
  </div>
  
  <script>
    function formatTime(seconds) {
      if (!seconds || seconds === 0) return 'Never';
      if (seconds < 60) return seconds + ' seconds ago';
      if (seconds < 3600) return Math.floor(seconds / 60) + ' minutes ago';
      if (seconds < 86400) return Math.floor(seconds / 3600) + ' hours ago';
      return Math.floor(seconds / 86400) + ' days ago';
    }
    
    async function loadData() {
      try {
        const response = await fetch('/api/cca');
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
            if (key === 'UTS') {
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
        
        // Display raw JSON
        document.getElementById('json-response').textContent = JSON.stringify(deviceInfo || data.device_info, null, 2);
        
      } catch (error) {
        console.error('Error loading data:', error);
        document.getElementById('connection-status').innerHTML = '<span class="badge badge-error">Failed</span>';
        document.getElementById('device-info').innerHTML = '<div class="error">Error loading CCA information</div>';
        document.getElementById('json-response').textContent = 'Error: ' + error.message;
      }
    }
    
    loadData();
    setInterval(loadData, 30000); // Refresh every 30 seconds
  </script>
</body>
</html>
)html";
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

}  // namespace tigo_server
}  // namespace esphome

#endif  // USE_ESP_IDF

