#include "tigo_monitor.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"
#include "esphome/components/network/util.h"
#include <cstring>
#include <numeric>

#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

#ifdef USE_ESP_IDF
#include "esp_http_client.h"
#include <esp_heap_caps.h>
#include "cJSON.h"
#endif

namespace esphome {
namespace tigo_monitor {

static const char *const TAG = "tigo_monitor";

#ifdef USE_ESP_IDF
// Helper function to allocate from PSRAM if available, falls back to regular heap
static void* psram_malloc(size_t size) {
  void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr) {
    // Only log large allocations to reduce spam
    if (size > 1024) {
      ESP_LOGD(TAG, "Allocated %zu bytes from PSRAM", size);
    }
    return ptr;
  }
  // Fallback to regular heap
  ptr = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
  if (ptr && size > 1024) {
    ESP_LOGW(TAG, "Allocated %zu bytes from regular heap (PSRAM unavailable)", size);
  }
  return ptr;
}

static void* psram_calloc(size_t count, size_t size) {
  void* ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr) {
    ESP_LOGV(TAG, "Allocated %zu bytes from PSRAM (calloc)", count * size);
    return ptr;
  }
  // Fallback to regular heap
  ptr = heap_caps_calloc(count, size, MALLOC_CAP_DEFAULT);
  if (ptr) {
    ESP_LOGV(TAG, "Allocated %zu bytes from regular heap (calloc, PSRAM unavailable)", count * size);
  }
  return ptr;
}

// Export for PSRAMAllocator in header
void* psram_malloc_impl(size_t size) {
  return psram_malloc(size);
}

void psram_free_impl(void* ptr) {
  heap_caps_free(ptr);
}

// Custom allocator hooks for cJSON to use PSRAM
static void* cjson_malloc_psram(size_t size) {
  return psram_malloc(size);
}

static void cjson_free_psram(void* ptr) {
  heap_caps_free(ptr);
}

// Helper functions for incoming_data_ buffer (works with both string and vector<char>)
template<typename BufferType>
inline size_t buffer_size(const BufferType& buf);

template<>
inline size_t buffer_size<std::string>(const std::string& buf) {
  return buf.size();
}

template<>
inline size_t buffer_size<psram_vector<char>>(const psram_vector<char>& buf) {
  return buf.size();
}

template<typename BufferType>
inline void buffer_clear(BufferType& buf);

template<>
inline void buffer_clear<std::string>(std::string& buf) {
  buf.clear();
}

template<>
inline void buffer_clear<psram_vector<char>>(psram_vector<char>& buf) {
  buf.clear();
}

template<typename BufferType>
inline void buffer_append(BufferType& buf, char c);

template<>
inline void buffer_append<std::string>(std::string& buf, char c) {
  buf += c;
}

template<>
inline void buffer_append<psram_vector<char>>(psram_vector<char>& buf, char c) {
  buf.push_back(c);
}

template<typename BufferType>
inline size_t buffer_find(const BufferType& buf, const char* pattern, size_t pattern_len);

template<>
inline size_t buffer_find<std::string>(const std::string& buf, const char* pattern, size_t pattern_len) {
  return buf.find(pattern);
}

template<>
inline size_t buffer_find<psram_vector<char>>(const psram_vector<char>& buf, const char* pattern, size_t pattern_len) {
  if (buf.size() < pattern_len) return std::string::npos;
  for (size_t i = 0; i <= buf.size() - pattern_len; i++) {
    if (memcmp(&buf[i], pattern, pattern_len) == 0) {
      return i;
    }
  }
  return std::string::npos;
}

template<typename BufferType>
inline std::string buffer_substr(const BufferType& buf, size_t pos, size_t len);

template<>
inline std::string buffer_substr<std::string>(const std::string& buf, size_t pos, size_t len) {
  return buf.substr(pos, len);
}

template<>
inline std::string buffer_substr<psram_vector<char>>(const psram_vector<char>& buf, size_t pos, size_t len) {
  if (pos >= buf.size()) return "";
  size_t actual_len = std::min(len, buf.size() - pos);
  return std::string(&buf[pos], actual_len);
}

template<typename BufferType>
inline void buffer_erase_prefix(BufferType& buf, size_t pos);

template<>
inline void buffer_erase_prefix<std::string>(std::string& buf, size_t pos) {
  buf = buf.substr(pos);
}

template<>
inline void buffer_erase_prefix<psram_vector<char>>(psram_vector<char>& buf, size_t pos) {
  if (pos < buf.size()) {
    buf.erase(buf.begin(), buf.begin() + pos);
  }
}

#endif

// Tigo CRC table - copied from original Arduino code
const uint8_t TigoMonitorComponent::tigo_crc_table_[256] = {
  0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,
  0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,
  0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,
  0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,
  0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,
  0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,
  0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4,0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,
  0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,
  0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,
  0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,
  0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,
  0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,
  0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,
  0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,
  0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,
  0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4
};

void TigoMonitorComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Tigo Server...");
  
#ifdef USE_ESP_IDF
  // Log PSRAM availability
  size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (psram_size > 0) {
    ESP_LOGI(TAG, "PSRAM available: %zu KB total, %zu KB free - using PSRAM for device/node storage", 
             psram_size / 1024, psram_free / 1024);
  } else {
    ESP_LOGW(TAG, "No PSRAM detected - using regular heap (may limit device capacity)");
  }
#endif

  generate_crc_table();
  devices_.reserve(number_of_devices_);
  node_table_.reserve(number_of_devices_);
  
#ifdef USE_ESP_IDF
  // Pre-allocate PSRAM buffer for incoming serial data (16KB capacity)
  incoming_data_.reserve(16384);
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psram_free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  
  // Check stack watermark for this task
  UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGI(TAG, "Pre-allocated 16KB PSRAM buffer for incoming serial data");
  ESP_LOGI(TAG, "Memory after allocation - Internal: %zu KB free, PSRAM: %zu KB free", 
           internal_free / 1024, psram_free_after / 1024);
  ESP_LOGI(TAG, "Stack high water mark: %u bytes free (monitor for stack overflow)", 
           stack_high_water * sizeof(StackType_t));
#endif
  
  load_node_table();
  load_energy_data();
  load_daily_energy_history();
  
  // Check if we have existing CCA data and rebuild string groups
  bool has_cca_data = false;
  int cca_nodes = 0;
  for (const auto &node : node_table_) {
    if (!node.cca_string_label.empty() && node.cca_validated) {
      has_cca_data = true;
      cca_nodes++;
      ESP_LOGD(TAG, "Loaded node %s with CCA data: label='%s', string='%s', validated=%d",
               node.addr.c_str(), node.cca_label.c_str(), node.cca_string_label.c_str(), node.cca_validated);
    }
  }
  if (has_cca_data) {
    ESP_LOGI(TAG, "Found existing CCA data in %d nodes - rebuilding string groups", cca_nodes);
    rebuild_string_groups();
  } else {
    ESP_LOGI(TAG, "No CCA data found in node table - UI will show barcodes until CCA sync occurs");
  }
  
  // Initialize night mode tracking
  last_data_received_ = millis();
  last_zero_publish_ = 0;
  in_night_mode_ = false;
  if (night_mode_sensor_ != nullptr) {
    night_mode_sensor_->publish_state(false);
  }
  
  // Initialize UART diagnostics
  invalid_checksum_count_ = 0;
  missed_frame_count_ = 0;
  if (invalid_checksum_sensor_ != nullptr) {
    invalid_checksum_sensor_->publish_state(0);
  }
  if (missed_frame_sensor_ != nullptr) {
    missed_frame_sensor_->publish_state(0);
  }
  
  // Register shutdown callback to save persistent data before OTA/reboot
  App.register_component(this);
  
  // Query CCA on boot if IP is configured and sync_cca_on_startup is enabled
  if (!cca_ip_.empty() && sync_cca_on_startup_) {
    ESP_LOGI(TAG, "CCA IP configured: %s - will sync configuration on boot", cca_ip_.c_str());
    // Delay sync to allow WiFi to connect and stabilize (15 seconds after setup)
    this->set_timeout("cca_sync", 15000, [this]() { this->sync_from_cca(); });
  } else if (!cca_ip_.empty() && !sync_cca_on_startup_) {
    ESP_LOGI(TAG, "CCA IP configured: %s - automatic sync disabled (use 'Sync from CCA' button)", cca_ip_.c_str());
  }
}
  

void TigoMonitorComponent::loop() {
  process_serial_data();
  
#ifdef USE_ESP_IDF
  // Periodic heap and stack monitoring (every 60 seconds) to detect memory leaks/stack issues
  static unsigned long last_heap_check = 0;
  unsigned long now = millis();
  if (now - last_heap_check > 60000) {
    last_heap_check = now;
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    UBaseType_t stack_watermark = uxTaskGetStackHighWaterMark(NULL);
    size_t stack_free_bytes = stack_watermark * sizeof(StackType_t);
    
    ESP_LOGD(TAG, "Heap: Internal %zu KB free (%zu KB min), PSRAM %zu KB free, Buffer: %zu bytes",
             internal_free / 1024, internal_min / 1024, psram_free / 1024, buffer_size(incoming_data_));
    ESP_LOGD(TAG, "Stack: %u bytes free (warning if < 512 bytes)", stack_free_bytes);
    
    // Log packet statistics
    uint32_t total_attempts = total_frames_processed_ + missed_frame_count_;
    if (total_attempts > 0) {
      float miss_rate = (missed_frame_count_ * 100.0f) / total_attempts;
      ESP_LOGI(TAG, "Frame stats: %u processed, %u missed (%.2f%% miss rate), %u invalid checksums",
               total_frames_processed_, missed_frame_count_, miss_rate, invalid_checksum_count_);
    }
    
    // Publish memory sensors to Home Assistant
    if (internal_ram_free_sensor_ != nullptr) {
      internal_ram_free_sensor_->publish_state(internal_free / 1024.0f);  // KB
    }
    if (internal_ram_min_sensor_ != nullptr) {
      internal_ram_min_sensor_->publish_state(internal_min / 1024.0f);  // KB
    }
    if (psram_free_sensor_ != nullptr) {
      psram_free_sensor_->publish_state(psram_free / 1024.0f);  // KB
    }
    if (stack_free_sensor_ != nullptr) {
      stack_free_sensor_->publish_state(stack_free_bytes);  // bytes
    }
    
    // Warn if stack is getting low
    if (stack_free_bytes < 512) {
      ESP_LOGW(TAG, "⚠️ Stack running low! Only %u bytes free - potential stack overflow risk", stack_free_bytes);
    }
    
    // Warn if internal RAM is getting low
    if (internal_min < 50000) {
      ESP_LOGW(TAG, "⚠️ Internal RAM minimum reached %zu KB - potential heap exhaustion",
               internal_min / 1024);
    }
  }
#endif
}

void TigoMonitorComponent::update() {
  // This is called every polling interval
  check_midnight_reset();
  publish_sensor_data();
}

void TigoMonitorComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Tigo Server:");
  ESP_LOGCONFIG(TAG, "  Update interval: %lums", this->get_update_interval());
  ESP_LOGCONFIG(TAG, "  Max devices: %d", number_of_devices_);
  ESP_LOGCONFIG(TAG, "  Multi-sensor platform with manual configuration");
  check_uart_settings(38400);
}

void TigoMonitorComponent::on_shutdown() {
  ESP_LOGI(TAG, "System shutdown detected - saving persistent data to flash...");
  save_persistent_data();
}

float TigoMonitorComponent::get_setup_priority() const {
  // Setup after network components (WiFi, API) to ensure they're ready for CCA sync
  // AFTER_WIFI priority is around 250, which is after WiFi (600) and API (600)
  return setup_priority::AFTER_WIFI;
}

#ifdef USE_BUTTON
void TigoYamlGeneratorButton::press_action() {
  ESP_LOGI("tigo_button", "Generating YAML configuration...");
  if (this->tigo_monitor_ != nullptr) {
    this->tigo_monitor_->generate_sensor_yaml();
  } else {
    ESP_LOGE("tigo_button", "Tigo server component not set!");
  }
}

void TigoDeviceMappingsButton::press_action() {
  ESP_LOGI("tigo_button", "Printing device mappings...");
  if (this->tigo_monitor_ != nullptr) {
    this->tigo_monitor_->print_device_mappings();
  } else {
    ESP_LOGE("tigo_button", "Tigo server component not set!");
  }
}

void TigoResetNodeTableButton::press_action() {
  ESP_LOGI("tigo_button", "Resetting node table...");
  if (this->tigo_monitor_ != nullptr) {
    this->tigo_monitor_->reset_node_table();
  } else {
    ESP_LOGE("tigo_button", "Tigo server component not set!");
  }
}

void TigoSyncFromCCAButton::press_action() {
  ESP_LOGI("tigo_button", "Syncing from CCA...");
  if (this->tigo_monitor_ != nullptr) {
    this->tigo_monitor_->sync_from_cca();
  } else {
    ESP_LOGE("tigo_button", "Tigo server component not set!");
  }
}
#endif

void TigoMonitorComponent::process_serial_data() {
#ifdef USE_ESP_IDF
  // Increase processing limit for high-throughput systems with display
  // Display updates can delay UART processing, so process more per loop
  const size_t MAX_BYTES_PER_LOOP = 4096;  // Process max 4KB per loop iteration (was 2KB)
  size_t bytes_processed = 0;
#endif
  
  while (available()) {
#ifdef USE_ESP_IDF
    // Yield to watchdog if we've processed too much data
    if (bytes_processed >= MAX_BYTES_PER_LOOP) {
      ESP_LOGV(TAG, "Yielding after processing %zu bytes", bytes_processed);
      yield();  // Let other tasks run
      return;   // Continue next loop
    }
    bytes_processed++;
#endif
    
    char incoming_byte = read();
    
#ifdef USE_ESP_IDF
    // Safety check: ensure we have capacity before append
    if (incoming_data_.size() >= 16384) {
      ESP_LOGW(TAG, "Buffer at max capacity before append, clearing!");
      buffer_clear(incoming_data_);
      frame_started_ = false;
      missed_frame_count_++;
      continue;  // Skip this byte
    }
    
    buffer_append(incoming_data_, incoming_byte);
    
    // Check if frame starts
    if (!frame_started_ && buffer_find(incoming_data_, "\x7E\x08", 2) != std::string::npos) {
      missed_frame_count_++;
      if (missed_frame_sensor_ != nullptr) {
        missed_frame_sensor_->publish_state(missed_frame_count_);
      }
      // Log buffer size and state to help diagnose if this is a buffer overflow or sync issue
      ESP_LOGW(TAG, "Packet missed! Found END before START (buffer: %zu bytes, available: %zu)", 
               buffer_size(incoming_data_), available());
    }
    
    if (!frame_started_ && buffer_find(incoming_data_, "\x7E\x07", 2) != std::string::npos) {
      // Start of a new frame detected
      frame_started_ = true;
      size_t start_pos = buffer_find(incoming_data_, "\x7E\x07", 2);
      buffer_erase_prefix(incoming_data_, start_pos);  // Keep only from start delimiter
    }
    // Check if frame ends
    else if (frame_started_ && buffer_find(incoming_data_, "\x7E\x08", 2) != std::string::npos) {
      // End of frame detected
      frame_started_ = false;
      size_t end_pos = buffer_find(incoming_data_, "\x7E\x08", 2);
      
      // Safety check: ensure end_pos is valid
      if (end_pos < 2 || end_pos > incoming_data_.size()) {
        ESP_LOGW(TAG, "Invalid frame end position: %zu (buffer size: %zu)", end_pos, incoming_data_.size());
        buffer_clear(incoming_data_);
        missed_frame_count_++;
        continue;
      }
      
      // Extract frame without start and end delimiters
      std::string frame = buffer_substr(incoming_data_, 2, end_pos - 2);
      buffer_clear(incoming_data_); // Clear buffer for next frame
      
      // Process the frame with safety checks
      if (!frame.empty() && frame.length() < 10000) {
        ESP_LOGV(TAG, "Processing frame of %zu bytes", frame.length());
        process_frame(frame);
      } else if (frame.length() >= 10000) {
        ESP_LOGW(TAG, "Frame too large (%zu bytes), skipping", frame.length());
        missed_frame_count_++;
      }
    }
    
    // Reset if buffer grows too large (safety mechanism)
    // Large buffer for high-throughput systems (P4 with 32MB PSRAM can handle this easily)
    if (buffer_size(incoming_data_) > 16384) {
      buffer_clear(incoming_data_);
      frame_started_ = false;
      ESP_LOGW(TAG, "Buffer too small, resetting! (>16KB bytes)");
      missed_frame_count_++;
    }
#else
    // Fallback to standard string operations
    incoming_data_ += incoming_byte;
    
    // Check if frame starts
    if (!frame_started_ && incoming_data_.find("\x7E\x08") != std::string::npos) {
      missed_frame_count_++;
      if (missed_frame_sensor_ != nullptr) {
        missed_frame_sensor_->publish_state(missed_frame_count_);
      }
      ESP_LOGW(TAG, "Frame missed!");
    }
    
    if (!frame_started_ && incoming_data_.find("\x7E\x07") != std::string::npos) {
      // Start of a new frame detected
      frame_started_ = true;
      size_t start_pos = incoming_data_.find("\x7E\x07");
      incoming_data_ = incoming_data_.substr(start_pos);  // Keep only from start delimiter
    }
    // Check if frame ends
    else if (frame_started_ && incoming_data_.find("\x7E\x08") != std::string::npos) {
      // End of frame detected
      frame_started_ = false;
      size_t end_pos = incoming_data_.find("\x7E\x08");
      
      // Extract frame without start and end delimiters
      std::string frame = incoming_data_.substr(2, end_pos - 2);
      incoming_data_.clear(); // Clear buffer for next frame
      
      // Process the frame
      process_frame(frame);
    }
    
    // Reset if buffer grows too large (safety mechanism)
    if (incoming_data_.length() > 16384) {
      incoming_data_.clear();
      frame_started_ = false;
      ESP_LOGW(TAG, "Buffer too small, resetting! (>16KB bytes)");
      missed_frame_count_++;
    }
#endif
  }
}

void TigoMonitorComponent::process_frame(const std::string &frame) {
  // Note: processed_frame and hex_frame are allocated in PSRAM via helper functions
  // (remove_escape_sequences and frame_to_hex_string use psram_string internally)
  // This saves 2-4KB of internal RAM per frame processed
  
  // Increment total frames processed counter for diagnostics
  total_frames_processed_++;
  
  std::string processed_frame = remove_escape_sequences(frame);
  
  if (!verify_checksum(processed_frame)) {
    invalid_checksum_count_++;
    if (invalid_checksum_sensor_ != nullptr) {
      invalid_checksum_sensor_->publish_state(invalid_checksum_count_);
    }
    ESP_LOGW(TAG, "Invalid checksum for frame: %s", frame_to_hex_string(processed_frame).c_str());
    return;
  }
  
  // Remove checksum (last 2 bytes)
  processed_frame = processed_frame.substr(0, processed_frame.length() - 2);
  std::string hex_frame = frame_to_hex_string(processed_frame);
  
  if (hex_frame.length() < 10) {
    ESP_LOGW(TAG, "Frame too short: %s", hex_frame.c_str());
    return;
  }
  
  std::string segment = hex_frame.substr(4, 4); // Get type segment
  
  if (segment == "0149") {
    // Power data frame
    int start_payload = 8 + calculate_header_length(hex_frame.substr(8, 4));
    std::string payload = hex_frame.substr(start_payload);
    
    size_t pos = 0;
    while (pos < payload.length()) {
      if (pos + 14 > payload.length()) {
        ESP_LOGW(TAG, "Incomplete packet, aborting");
        break;
      }
      
      std::string type = payload.substr(pos, 2);
      std::string length_hex = payload.substr(pos + 12, 2);
      int length = std::stoi(length_hex, nullptr, 16);
      
      int packet_length_chars = length * 2 + 14;
      if (pos + packet_length_chars > payload.length()) {
        ESP_LOGW(TAG, "Incomplete packet, aborting at pos %zu", pos);
        break;
      }
      
      std::string packet = payload.substr(pos, packet_length_chars);
      
      if (type == "31") {
        process_power_frame(packet);
      } else if (type == "09") {
        process_09_frame(packet);
      } else if (type != "07" && type != "18") {
        ESP_LOGD(TAG, "Unknown packet type: %s, packet: %s", type.c_str(), packet.c_str());
      }
      
      pos += packet_length_chars;
    }
  } else if (segment == "0B10" || segment == "0B0F") {
    // Command request or response
    std::string type = hex_frame.substr(14, 2);
    if (type == "27") {
      process_27_frame(hex_frame, 18);  // Pass offset instead of creating substring
    }
    // Handle other command types as needed
  } else if (segment == "0148") {
    // Receive request packet
    // ESP_LOGD(TAG, "Receive request packet");
  } else {
    ESP_LOGD(TAG, "Unknown frame type: %s", hex_frame.c_str());
  }
}

void TigoMonitorComponent::process_power_frame(const std::string &frame) {
  DeviceData data;
  
  // Parse frame according to original Arduino logic
  data.addr = frame.substr(2, 4);
  data.pv_node_id = frame.substr(6, 4);
  
  ESP_LOGD(TAG, "Processing power frame for device addr: %s", data.addr.c_str());
  
  // Voltage In (scale by 0.05)
  int voltage_in_raw = std::stoi(frame.substr(14, 3), nullptr, 16);
  data.voltage_in = voltage_in_raw * 0.05f;
  
  // Voltage Out (scale by 0.10)
  int voltage_out_raw = std::stoi(frame.substr(17, 3), nullptr, 16);
  data.voltage_out = voltage_out_raw * 0.10f;
  
  // Duty Cycle
  data.duty_cycle = std::stoi(frame.substr(20, 2), nullptr, 16);
  
  // Current In (scale by 0.005)
  int current_in_raw = std::stoi(frame.substr(22, 3), nullptr, 16);
  data.current_in = current_in_raw * 0.005f;
  
  // Temperature (scale by 0.1) - handle signed 12-bit value in two's complement
  int temperature_raw = std::stoi(frame.substr(25, 3), nullptr, 16);
  // Convert from 12-bit two's complement to signed value
  if (temperature_raw & 0x800) {  // Check sign bit (bit 11)
    temperature_raw = temperature_raw - 0x1000;  // Convert to negative
  }
  data.temperature = temperature_raw * 0.1f;
  
  // Slot Counter
  data.slot_counter = frame.substr(34, 4);
  
  // RSSI
  data.rssi = std::stoi(frame.substr(38, 2), nullptr, 16);
  
  // Calculate additional sensor values
  // Efficiency calculation (output power / input power * 100)
  float input_power = data.voltage_in * data.current_in;
  float output_power = data.voltage_out * data.current_in;
  if (input_power > 0.0f) {
    data.efficiency = (output_power / input_power) * 100.0f;
  } else {
    data.efficiency = 0.0f;
  }
  
  // Power factor (assuming unity for DC systems, can be customized)
  data.power_factor = 1.0f;
  
  // Load factor (duty cycle as decimal)
  data.load_factor = data.duty_cycle / 100.0f;
  
  // Firmware version (placeholder - would need to be extracted from other frames if available)
  data.firmware_version = "unknown";
  
  data.changed = true;
  data.last_update = millis();
  
  // Find barcode from Frame 27 data only
  NodeTableData* node = find_node_by_addr(data.addr);
  if (node != nullptr && !node->long_address.empty()) {
    data.barcode = node->long_address;
    ESP_LOGD(TAG, "Using Frame 27 long address as barcode for device %s: %s", data.addr.c_str(), data.barcode.c_str());
  } else {
    // No Frame 27 long address available yet
    data.barcode = "";
    ESP_LOGD(TAG, "No Frame 27 long address available for device %s", data.addr.c_str());
  }
  
  update_device_data(data);
}

void TigoMonitorComponent::process_09_frame(const std::string &frame) {
  std::string addr = frame.substr(14, 4);
  std::string node_id = frame.substr(18, 4);  
  std::string barcode = frame.substr(40, 6);
  
  ESP_LOGD(TAG, "Frame 09 - Device Identity (IGNORED): addr=%s, node_id=%s, barcode=%s", 
           addr.c_str(), node_id.c_str(), barcode.c_str());
  ESP_LOGD(TAG, "Frame 09 barcodes are ignored - only Frame 27 (16-char) barcodes are used");
  
  // Frame 09 data is now completely ignored to prevent duplicate entries
  // Only Frame 27 long addresses (16-char) are used for device identification
}

void TigoMonitorComponent::process_27_frame(const std::string &hex_frame, size_t offset) {
  // Parse number of entries from the frame starting at offset
  if (offset + 4 > hex_frame.length()) {
    ESP_LOGW(TAG, "Frame 27 too short");
    return;
  }
  
  int num_entries = std::stoi(hex_frame.substr(offset, 4), nullptr, 16);
  ESP_LOGI(TAG, "Frame 27 received, entries: %d", num_entries);
  
  size_t pos = offset + 4;  // Start after the entry count
  bool table_changed = false;
  
  for (int i = 0; i < num_entries && pos + 20 <= hex_frame.length(); i++) {
    std::string long_addr = hex_frame.substr(pos, 16);
    std::string addr = hex_frame.substr(pos + 16, 4);
    pos += 20;
    
    ESP_LOGI(TAG, "Frame 27 - Device Identity: addr=%s, long_addr=%s", 
             addr.c_str(), long_addr.c_str());
    
    // Find or create node table entry
    NodeTableData* node = find_node_by_addr(addr);
    if (node != nullptr) {
      // Update existing node with Frame 27 long address
      if (node->long_address != long_addr) {
        node->long_address = long_addr;
        ESP_LOGI(TAG, "Updated Frame 27 long address for node %s: %s", addr.c_str(), long_addr.c_str());
        table_changed = true;
      }
    } else {
      // Create new node table entry for Frame 27 data
      if (node_table_.size() < number_of_devices_) {
        NodeTableData new_node;
        new_node.addr = addr;
        new_node.long_address = long_addr;
        new_node.checksum = std::string(1, compute_tigo_crc4(addr));
        new_node.sensor_index = -1;  // Will be assigned when device becomes active
        new_node.is_persistent = true;
        node_table_.push_back(new_node);
        ESP_LOGI(TAG, "Created new node entry for Frame 27: addr=%s, long_addr=%s", addr.c_str(), long_addr.c_str());
        table_changed = true;
      }
    }
    
    // Also update existing device if already discovered
    DeviceData* device = find_device_by_addr(addr);
    if (device != nullptr) {
      if (device->barcode != long_addr) {
        device->barcode = long_addr;
        ESP_LOGI(TAG, "Updated existing device %s with Frame 27 long address: %s", addr.c_str(), long_addr.c_str());
      } else {
        ESP_LOGD(TAG, "Device %s already has Frame 27 long address: %s", addr.c_str(), long_addr.c_str());
      }
      
      // Always publish the barcode sensor when Frame 27 data arrives
      auto barcode_it = barcode_sensors_.find(addr);
      if (barcode_it != barcode_sensors_.end()) {
        barcode_it->second->publish_state(long_addr);
        ESP_LOGD(TAG, "Published Frame 27 long address for %s: %s", addr.c_str(), long_addr.c_str());
      }
    } else {
      ESP_LOGD(TAG, "Frame 27 data for %s received before power data - long address stored in node table", addr.c_str());
    }
  }
  
  if (table_changed) {
    save_node_table();
  }
}

void TigoMonitorComponent::update_device_data(const DeviceData &data) {
  ESP_LOGD(TAG, "Updating device data for addr: %s", data.addr.c_str());
  
  // Track when data is received
  last_data_received_ = millis();
  if (in_night_mode_) {
    ESP_LOGI(TAG, "Exiting night mode - data received from %s", data.addr.c_str());
    in_night_mode_ = false;
    if (night_mode_sensor_ != nullptr) {
      night_mode_sensor_->publish_state(false);
    }
  }
  
  // Find existing device or add new one
  DeviceData *device = find_device_by_addr(data.addr);
  if (device != nullptr) {
    // Preserve peak_power when updating device data
    float saved_peak_power = device->peak_power;
    *device = data;
    device->peak_power = saved_peak_power;
    ESP_LOGD(TAG, "Updated existing device: %s (preserved peak: %.0fW)", data.addr.c_str(), saved_peak_power);
  } else if (devices_.size() < number_of_devices_) {
    devices_.push_back(data);
    ESP_LOGI(TAG, "New device discovered: addr=%s, barcode=%s", 
             data.addr.c_str(), data.barcode.c_str());
    
    // Load saved peak power for this device
    DeviceData* new_device = &devices_.back();
    std::string pref_key = "peak_" + data.addr;
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto load = global_preferences->make_preference<float>(hash);
    float saved_peak = 0.0f;
    if (load.load(&saved_peak) && saved_peak > 0.0f) {
      new_device->peak_power = saved_peak;
      ESP_LOGI(TAG, "Restored peak power for %s: %.0fW", data.addr.c_str(), saved_peak);
    }
    
    // Track device discovery for node table management
    if (created_devices_.find(data.addr) == created_devices_.end()) {
      // Check if we have a saved sensor index in the node table
      NodeTableData* node = find_node_by_addr(data.addr);
      int sensor_index = -1;
      
      if (node != nullptr && node->sensor_index >= 0) {
        sensor_index = node->sensor_index;
        ESP_LOGI(TAG, "Restored device %s to previous index %d assignment", data.addr.c_str(), sensor_index + 1);
      } else {
        // Assign new sensor index for node table tracking
        assign_sensor_index_to_node(data.addr);
        node = find_node_by_addr(data.addr);
        if (node != nullptr) {
          sensor_index = node->sensor_index;
        }
      }
      
      // After device creation, ensure barcode is up to date from node table
      // Use Frame 27 long address as the only barcode source
      DeviceData* new_device = &devices_.back();
      if (node != nullptr && !node->long_address.empty() && new_device->barcode != node->long_address) {
        new_device->barcode = node->long_address;
        ESP_LOGI(TAG, "Applied Frame 27 long address as barcode to new device %s: %s", data.addr.c_str(), node->long_address.c_str());
      }
      
      ESP_LOGI(TAG, "Device data: %s - Vin:%.2fV, Vout:%.2fV, Curr:%.3fA, Temp:%.1f°C, Barcode:%s", 
               data.addr.c_str(), data.voltage_in, data.voltage_out, data.current_in, data.temperature, new_device->barcode.c_str());
      
      created_devices_.insert(data.addr);
    } else {
      ESP_LOGD(TAG, "Device already tracked: %s", data.addr.c_str());
    }
  } else {
    ESP_LOGW(TAG, "Maximum number of devices reached (%d)", number_of_devices_);
  }
}

DeviceData* TigoMonitorComponent::find_device_by_addr(const std::string &addr) {
  for (auto &device : devices_) {
    if (device.addr == addr) {
      return &device;
    }
  }
  return nullptr;
}

void TigoMonitorComponent::rebuild_string_groups() {
  ESP_LOGI(TAG, "Rebuilding string groups from CCA data...");
  ESP_LOGI(TAG, "Node table has %d entries", node_table_.size());
  
  // Count how many nodes have CCA data
  int cca_validated_count = 0;
  for (const auto &node : node_table_) {
    if (node.cca_validated) {
      ESP_LOGD(TAG, "Node %s: cca_validated=%d, cca_label='%s', string_label='%s'", 
               node.addr.c_str(), node.cca_validated, node.cca_label.c_str(), node.cca_string_label.c_str());
      cca_validated_count++;
    }
  }
  ESP_LOGI(TAG, "%d nodes have CCA validation", cca_validated_count);
  
  // Clear existing string data but preserve peak power
  std::map<std::string, float> saved_peaks;
  for (const auto &pair : strings_) {
    saved_peaks[pair.first] = pair.second.peak_power;
  }
  strings_.clear();
  
  // Group devices by their CCA string label
  for (const auto &node : node_table_) {
    if (!node.cca_string_label.empty() && node.cca_validated) {
      const std::string &string_label = node.cca_string_label;
      
      // Create string entry if it doesn't exist
      if (strings_.find(string_label) == strings_.end()) {
        StringData string_data;
        string_data.string_label = string_label;
        string_data.inverter_label = node.cca_inverter_label;
        strings_[string_label] = string_data;
        
        // Restore peak power if available
        if (saved_peaks.count(string_label) > 0) {
          strings_[string_label].peak_power = saved_peaks[string_label];
        }
        
        ESP_LOGI(TAG, "Created string group: %s (Inverter: %s)", 
                 string_label.c_str(), node.cca_inverter_label.c_str());
      }
      
      // Add device to string group
      strings_[string_label].device_addrs.push_back(node.addr);
      strings_[string_label].total_device_count++;
    }
  }
  
  ESP_LOGI(TAG, "String grouping complete: %d strings created", strings_.size());
  for (const auto &pair : strings_) {
    ESP_LOGI(TAG, "  %s: %d devices", pair.first.c_str(), pair.second.total_device_count);
  }
}

void TigoMonitorComponent::update_string_data() {
  if (strings_.empty()) {
    return;  // No string groups configured
  }
  
  unsigned long current_time = millis();
  
  for (auto &pair : strings_) {
    StringData &string_data = pair.second;
    
    // Reset aggregates
    string_data.total_power = 0.0f;
    string_data.total_current = 0.0f;
    float sum_voltage_in = 0.0f;
    float sum_voltage_out = 0.0f;
    float sum_temp = 0.0f;
    float sum_efficiency = 0.0f;
    string_data.min_efficiency = 100.0f;
    string_data.max_efficiency = 0.0f;
    string_data.active_device_count = 0;
    
    // Aggregate data from all devices in this string
    for (const auto &addr : string_data.device_addrs) {
      DeviceData *device = find_device_by_addr(addr);
      if (device && device->last_update > 0) {
        float device_power = device->voltage_out * device->current_in * power_calibration_;
        string_data.total_power += device_power;
        string_data.total_current += device->current_in;
        sum_voltage_in += device->voltage_in;
        sum_voltage_out += device->voltage_out;
        sum_temp += device->temperature;
        sum_efficiency += device->efficiency;
        
        if (device->efficiency < string_data.min_efficiency) {
          string_data.min_efficiency = device->efficiency;
        }
        if (device->efficiency > string_data.max_efficiency) {
          string_data.max_efficiency = device->efficiency;
        }
        
        string_data.active_device_count++;
      }
    }
    
    // Calculate averages
    if (string_data.active_device_count > 0) {
      string_data.avg_voltage_in = sum_voltage_in / string_data.active_device_count;
      string_data.avg_voltage_out = sum_voltage_out / string_data.active_device_count;
      string_data.avg_temperature = sum_temp / string_data.active_device_count;
      string_data.avg_efficiency = sum_efficiency / string_data.active_device_count;
      string_data.last_update = current_time;
      
      // Update peak power
      if (string_data.total_power > string_data.peak_power) {
        string_data.peak_power = string_data.total_power;
        ESP_LOGD(TAG, "New peak power for %s: %.0fW", 
                 string_data.string_label.c_str(), string_data.peak_power);
      }
      
      ESP_LOGD(TAG, "String %s: %.0fW from %d/%d devices (avg eff: %.1f%%)", 
               string_data.string_label.c_str(), string_data.total_power,
               string_data.active_device_count, string_data.total_device_count,
               string_data.avg_efficiency);
    } else {
      // No active devices - reset min/max efficiency
      string_data.min_efficiency = 0.0f;
      string_data.max_efficiency = 0.0f;
    }
  }
}

void TigoMonitorComponent::add_inverter(const std::string &name, const std::vector<std::string> &mppt_labels) {
  InverterData inverter;
  inverter.name = name;
  inverter.mppt_labels = mppt_labels;
  inverters_.push_back(inverter);
  ESP_LOGCONFIG(TAG, "Registered inverter '%s' with %d MPPTs", name.c_str(), mppt_labels.size());
  for (const auto &mppt : mppt_labels) {
    ESP_LOGCONFIG(TAG, "  - MPPT: %s", mppt.c_str());
  }
}

void TigoMonitorComponent::update_inverter_data() {
  // Reset all inverter aggregates
  for (auto &inverter : inverters_) {
    inverter.total_power = 0.0f;
    inverter.peak_power = 0.0f;
    inverter.total_energy = 0.0f;
    inverter.active_device_count = 0;
    inverter.total_device_count = 0;
  }
  
  // Aggregate data from strings that belong to each inverter
  for (auto &inverter : inverters_) {
    for (const auto &mppt_label : inverter.mppt_labels) {
      // Find all strings that belong to this MPPT
      for (const auto &string_pair : strings_) {
        const auto &string_data = string_pair.second;
        
        // Check if this string belongs to this MPPT
        if (string_data.inverter_label == mppt_label) {
          inverter.total_power += string_data.total_power;
          inverter.peak_power += string_data.peak_power;
          inverter.active_device_count += string_data.active_device_count;
          inverter.total_device_count += string_data.total_device_count;
        }
      }
    }
    
    ESP_LOGD(TAG, "Inverter %s: %.0fW from %d/%d devices (peak: %.0fW)", 
             inverter.name.c_str(), inverter.total_power,
             inverter.active_device_count, inverter.total_device_count,
             inverter.peak_power);
  }
}

StringData* TigoMonitorComponent::find_string_by_label(const std::string &label) {
  auto it = strings_.find(label);
  if (it != strings_.end()) {
    return &it->second;
  }
  return nullptr;
}

void TigoMonitorComponent::publish_sensor_data() {
  unsigned long current_time = millis();
  
  // Check if we should enter night mode (no data for 1 hour)
  if (last_data_received_ > 0 && !in_night_mode_ && (current_time - last_data_received_ > NO_DATA_TIMEOUT)) {
    ESP_LOGI(TAG, "Entering night mode - no data received for 1 hour");
    in_night_mode_ = true;
    last_zero_publish_ = 0;  // Reset to force immediate zero publish
    if (night_mode_sensor_ != nullptr) {
      night_mode_sensor_->publish_state(true);
    }
  }
  
  // In night mode, publish zeros every 10 minutes
  if (in_night_mode_) {
    if (last_zero_publish_ == 0 || (current_time - last_zero_publish_ >= ZERO_PUBLISH_INTERVAL)) {
      ESP_LOGI(TAG, "Night mode: Publishing zero values for all sensors");
      last_zero_publish_ = current_time;
      
      // Publish zeros for all registered devices
      for (const auto &device : devices_) {
        // Publish zero voltage input
        auto voltage_in_it = voltage_in_sensors_.find(device.addr);
        if (voltage_in_it != voltage_in_sensors_.end()) {
          voltage_in_it->second->publish_state(0.0f);
        }
        
        // Publish zero voltage output
        auto voltage_out_it = voltage_out_sensors_.find(device.addr);
        if (voltage_out_it != voltage_out_sensors_.end()) {
          voltage_out_it->second->publish_state(0.0f);
        }
        
        // Publish zero current
        auto current_in_it = current_in_sensors_.find(device.addr);
        if (current_in_it != current_in_sensors_.end()) {
          current_in_it->second->publish_state(0.0f);
        }
        
        // Publish NaN temperature (unavailable in night mode)
        auto temperature_it = temperature_sensors_.find(device.addr);
        if (temperature_it != temperature_sensors_.end()) {
          temperature_it->second->publish_state(NAN);
        }
        
        // Publish zero power
        auto power_it = power_sensors_.find(device.addr);
        if (power_it != power_sensors_.end()) {
          power_it->second->publish_state(0.0f);
        }
        
        // Publish zero RSSI
        auto rssi_it = rssi_sensors_.find(device.addr);
        if (rssi_it != rssi_sensors_.end()) {
          rssi_it->second->publish_state(0.0f);
        }
        
        // Publish zero duty cycle
        auto duty_cycle_it = duty_cycle_sensors_.find(device.addr);
        if (duty_cycle_it != duty_cycle_sensors_.end()) {
          duty_cycle_it->second->publish_state(0.0f);
        }
        
        // Publish zero efficiency
        auto efficiency_it = efficiency_sensors_.find(device.addr);
        if (efficiency_it != efficiency_sensors_.end()) {
          efficiency_it->second->publish_state(0.0f);
        }
        
        // Publish zero power factor
        auto power_factor_it = power_factor_sensors_.find(device.addr);
        if (power_factor_it != power_factor_sensors_.end()) {
          power_factor_it->second->publish_state(0.0f);
        }
        
        // Publish zero load factor
        auto load_factor_it = load_factor_sensors_.find(device.addr);
        if (load_factor_it != load_factor_sensors_.end()) {
          load_factor_it->second->publish_state(0.0f);
        }
      }
      
      // Publish zero power sum
      if (power_sum_sensor_ != nullptr) {
        power_sum_sensor_->publish_state(0.0f);
      }
      
      ESP_LOGI(TAG, "Night mode: Zero values published for %zu devices", devices_.size());
    }
    return;  // Don't publish actual data in night mode
  }
  
  // Normal mode - publish actual sensor data
  ESP_LOGD(TAG, "Publishing sensor data for %zu devices", devices_.size());
  
  for (size_t i = 0; i < devices_.size(); i++) {
    auto &device = devices_[i];
    std::string device_id = device.barcode.empty() ? ("mod#" + device.addr) : device.barcode;
    
    // Publish voltage input sensor
    auto voltage_in_it = voltage_in_sensors_.find(device.addr);
    if (voltage_in_it != voltage_in_sensors_.end()) {
      voltage_in_it->second->publish_state(device.voltage_in);
      ESP_LOGD(TAG, "Published input voltage for %s: %.2fV", device.addr.c_str(), device.voltage_in);
    }
    
    // Publish voltage output sensor
    auto voltage_out_it = voltage_out_sensors_.find(device.addr);
    if (voltage_out_it != voltage_out_sensors_.end()) {
      voltage_out_it->second->publish_state(device.voltage_out);
      ESP_LOGD(TAG, "Published output voltage for %s: %.2fV", device.addr.c_str(), device.voltage_out);
    }
    
    // Publish current sensor
    auto current_in_it = current_in_sensors_.find(device.addr);
    if (current_in_it != current_in_sensors_.end()) {
      current_in_it->second->publish_state(device.current_in);
      ESP_LOGD(TAG, "Published current for %s: %.3fA", device.addr.c_str(), device.current_in);
    }
    
    // Publish temperature sensor
    auto temperature_it = temperature_sensors_.find(device.addr);
    if (temperature_it != temperature_sensors_.end()) {
      temperature_it->second->publish_state(device.temperature);
      ESP_LOGD(TAG, "Published temperature for %s: %.1f°C", device.addr.c_str(), device.temperature);
    }
    
    // Publish power sensor (calculated)
    auto power_it = power_sensors_.find(device.addr);
    if (power_it != power_sensors_.end()) {
      float power = device.voltage_out * device.current_in * power_calibration_;
      power_it->second->publish_state(power);
      ESP_LOGD(TAG, "Published power for %s: %.0fW", device.addr.c_str(), power);
      
      // Track peak power
      if (power > device.peak_power) {
        device.peak_power = power;
        ESP_LOGD(TAG, "New peak power for %s: %.0fW", device.addr.c_str(), device.peak_power);
      }
    }
    
    // Publish peak power sensor (always publish current peak)
    auto peak_power_it = peak_power_sensors_.find(device.addr);
    if (peak_power_it != peak_power_sensors_.end()) {
      peak_power_it->second->publish_state(device.peak_power);
      ESP_LOGD(TAG, "Published peak power for %s: %.0fW", device.addr.c_str(), device.peak_power);
    }
    
    // Publish RSSI sensor
    auto rssi_it = rssi_sensors_.find(device.addr);
    if (rssi_it != rssi_sensors_.end()) {
      rssi_it->second->publish_state(device.rssi);
      ESP_LOGD(TAG, "Published RSSI for %s: %ddBm", device.addr.c_str(), device.rssi);
    }
    
    // Publish barcode text sensor
    auto barcode_it = barcode_sensors_.find(device.addr);
    if (barcode_it != barcode_sensors_.end()) {
      barcode_it->second->publish_state(device.barcode);
      ESP_LOGD(TAG, "Published barcode for %s: %s", device.addr.c_str(), device.barcode.c_str());
    }

    // Publish duty cycle sensor
    auto duty_cycle_it = duty_cycle_sensors_.find(device.addr);
    if (duty_cycle_it != duty_cycle_sensors_.end()) {
      duty_cycle_it->second->publish_state(device.duty_cycle);
      ESP_LOGD(TAG, "Published duty cycle for %s: %u%%", device.addr.c_str(), device.duty_cycle);
    }

    // Publish firmware version text sensor
    auto firmware_version_it = firmware_version_sensors_.find(device.addr);
    if (firmware_version_it != firmware_version_sensors_.end()) {
      firmware_version_it->second->publish_state(device.firmware_version);
      ESP_LOGD(TAG, "Published firmware version for %s: %s", device.addr.c_str(), device.firmware_version.c_str());
    }

    // Publish efficiency sensor
    auto efficiency_it = efficiency_sensors_.find(device.addr);
    if (efficiency_it != efficiency_sensors_.end()) {
      efficiency_it->second->publish_state(device.efficiency);
      ESP_LOGD(TAG, "Published efficiency for %s: %.2f%%", device.addr.c_str(), device.efficiency);
    }

    // Publish power factor sensor
    auto power_factor_it = power_factor_sensors_.find(device.addr);
    if (power_factor_it != power_factor_sensors_.end()) {
      power_factor_it->second->publish_state(device.power_factor);
      ESP_LOGD(TAG, "Published power factor for %s: %.3f", device.addr.c_str(), device.power_factor);
    }

    // Publish load factor sensor
    auto load_factor_it = load_factor_sensors_.find(device.addr);
    if (load_factor_it != load_factor_sensors_.end()) {
      load_factor_it->second->publish_state(device.load_factor);
      ESP_LOGD(TAG, "Published load factor for %s: %.3f", device.addr.c_str(), device.load_factor);
    }
    
    // Check if this device has a combined Tigo sensor
    auto tigo_power_it = power_sensors_.find(device.addr);
    if (tigo_power_it != power_sensors_.end() && 
        voltage_in_sensors_.find(device.addr) == voltage_in_sensors_.end()) {
      // This is a combined sensor (power sensor exists but individual sensors don't)
      float power = device.voltage_out * device.current_in * power_calibration_;
      
      // Calculate data age
      unsigned long current_time = millis();
      unsigned long data_age_ms = current_time - device.last_update;
      float data_age_seconds = data_age_ms / 1000.0f;
      
      // Format timestamp string for enhanced logging
      char timestamp_str[32];
      if (data_age_ms < 1000) {
        snprintf(timestamp_str, sizeof(timestamp_str), "%.0fms ago", (float)data_age_ms);
      } else if (data_age_ms < 60000) {
        snprintf(timestamp_str, sizeof(timestamp_str), "%.1fs ago", data_age_ms / 1000.0f);
      } else if (data_age_ms < 3600000) {
        snprintf(timestamp_str, sizeof(timestamp_str), "%.1fm ago", data_age_ms / 60000.0f);
      } else {
        snprintf(timestamp_str, sizeof(timestamp_str), "%.1fh ago", data_age_ms / 3600000.0f);
      }
      
      // Publish the power value
      tigo_power_it->second->publish_state(power);
      
      // Enhanced logging with timestamp and all metrics for potential Home Assistant template extraction
      ESP_LOGI(TAG, "TIGO_%s: power=%.0f voltage_in=%.2f voltage_out=%.2f current=%.3f temp=%.1f rssi=%d last_update=%s", 
               device.addr.c_str(), power, device.voltage_in, device.voltage_out, 
               device.current_in, device.temperature, device.rssi, timestamp_str);
               
      ESP_LOGD(TAG, "Published combined Tigo sensor for %s: %.0fW with enhanced attributes logging", device.addr.c_str());
    }


  }
  
  // Publish device count sensor if configured
  if (device_count_sensor_ != nullptr) {
    int device_count = devices_.size();
    device_count_sensor_->publish_state(device_count);
    ESP_LOGD(TAG, "Published device count: %d", device_count);
  }
  
  // Calculate and publish power sum sensor if configured
  if (power_sum_sensor_ != nullptr) {
    float total_power = 0.0f;
    int active_devices = 0;
    int online_count = 0;
    unsigned long now = millis();
    const unsigned long ONLINE_THRESHOLD = 300000;  // 5 minutes
    
    for (const auto &device : devices_) {
      float device_power = device.voltage_out * device.current_in * power_calibration_;
      total_power += device_power;
      active_devices++;
      
      // Count online devices (seen in last 5 minutes)
      if (device.last_update > 0 && (now - device.last_update) < ONLINE_THRESHOLD) {
        online_count++;
      }
    }
    
    // Cache values for fast display access (avoids iteration in display lambda)
    cached_total_power_ = total_power;
    cached_online_count_ = online_count;
    
    power_sum_sensor_->publish_state(total_power);
    ESP_LOGD(TAG, "Published power sum: %.0fW from %d devices (%d online)", total_power, active_devices, online_count);
    
    // Calculate and publish energy sum sensor if configured
    if (energy_sum_sensor_ != nullptr) {
      unsigned long current_time = millis();
      
      if (last_energy_update_ > 0) {
        // Calculate time difference in hours
        unsigned long time_diff_ms = current_time - last_energy_update_;
        float time_diff_hours = time_diff_ms / 3600000.0f;
        
        // Calculate energy increment in kWh (power in watts * time in hours / 1000)
        float energy_increment_kwh = (total_power * time_diff_hours) / 1000.0f;
        total_energy_kwh_ += energy_increment_kwh;
        
        ESP_LOGD(TAG, "Energy calculation: %.0fW for %.4fh = %.6f kWh increment, total: %.3f kWh", 
                 total_power, time_diff_hours, energy_increment_kwh, total_energy_kwh_);
      }
      
      last_energy_update_ = current_time;
      energy_sum_sensor_->publish_state(total_energy_kwh_);
      ESP_LOGD(TAG, "Published energy sum: %.3f kWh", total_energy_kwh_);
      
      // Update daily energy tracking
      update_daily_energy(total_energy_kwh_);
      
      // Save energy data at the top of each hour to reduce flash wear
      static unsigned long last_save_time = 0;
      unsigned long hours_elapsed = current_time / 3600000;  // Hours since boot
      unsigned long last_save_hour = last_save_time / 3600000;
      
      if (hours_elapsed > last_save_hour) {
        save_energy_data();
        last_save_time = current_time;
        ESP_LOGI(TAG, "Energy data saved at hour boundary");
      }
    }
  } else if (energy_sum_sensor_ != nullptr) {
    // Energy sensor configured but no power sum sensor - calculate power directly
    float total_power = 0.0f;
    
    for (const auto &device : devices_) {
      float device_power = device.voltage_out * device.current_in * power_calibration_;
      total_power += device_power;
    }
    
    unsigned long current_time = millis();
    
    if (last_energy_update_ > 0) {
      // Calculate time difference in hours
      unsigned long time_diff_ms = current_time - last_energy_update_;
      float time_diff_hours = time_diff_ms / 3600000.0f;
      
      // Calculate energy increment in kWh
      float energy_increment_kwh = (total_power * time_diff_hours) / 1000.0f;
      total_energy_kwh_ += energy_increment_kwh;
      
      ESP_LOGD(TAG, "Energy calculation: %.0fW for %.4fh = %.6f kWh increment, total: %.3f kWh", 
               total_power, time_diff_hours, energy_increment_kwh, total_energy_kwh_);
    }
    
    last_energy_update_ = current_time;
    energy_sum_sensor_->publish_state(total_energy_kwh_);
    ESP_LOGD(TAG, "Published energy sum: %.3f kWh", total_energy_kwh_);
    
    // Update daily energy tracking
    update_daily_energy(total_energy_kwh_);
    
    // Save energy data at the top of each hour to reduce flash wear
    static unsigned long last_save_time_standalone = 0;
    unsigned long hours_elapsed = current_time / 3600000;  // Hours since boot
    unsigned long last_save_hour = last_save_time_standalone / 3600000;
    
    if (hours_elapsed > last_save_hour) {
      save_energy_data();
      last_save_time_standalone = current_time;
      ESP_LOGI(TAG, "Energy data saved at hour boundary");
    }
  }
  
  // Publish saved data for nodes that have sensors but no runtime data yet
  // This handles the case where ESP32 restarts at night - we publish zeros with saved peak power
  for (const auto &node : node_table_) {
    // Only process nodes that have assigned sensor indices
    if (node.sensor_index < 0) continue;
    
    // Check if this node already has runtime data
    bool has_runtime_data = false;
    for (const auto &device : devices_) {
      if (device.addr == node.addr) {
        has_runtime_data = true;
        break;
      }
    }
    
    // Skip if we already published data for this node
    if (has_runtime_data) continue;
    
    // This node has a sensor but no runtime data - publish zeros with saved peak power
    ESP_LOGD(TAG, "Publishing saved data for node %s (no runtime data yet)", node.addr.c_str());
    
    // Try to load saved peak power for this node
    std::string pref_key = "peak_" + node.addr;
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto load = global_preferences->make_preference<float>(hash);
    float saved_peak = 0.0f;
    load.load(&saved_peak);
    
    // Publish zeros for all sensors except peak power (which uses saved value)
    auto voltage_in_it = voltage_in_sensors_.find(node.addr);
    if (voltage_in_it != voltage_in_sensors_.end()) {
      voltage_in_it->second->publish_state(0.0f);
    }
    
    auto voltage_out_it = voltage_out_sensors_.find(node.addr);
    if (voltage_out_it != voltage_out_sensors_.end()) {
      voltage_out_it->second->publish_state(0.0f);
    }
    
    auto current_in_it = current_in_sensors_.find(node.addr);
    if (current_in_it != current_in_sensors_.end()) {
      current_in_it->second->publish_state(0.0f);
    }
    
    auto temperature_it = temperature_sensors_.find(node.addr);
    if (temperature_it != temperature_sensors_.end()) {
      temperature_it->second->publish_state(NAN);  // Use NAN for unavailable temperature
    }
    
    auto power_it = power_sensors_.find(node.addr);
    if (power_it != power_sensors_.end()) {
      power_it->second->publish_state(0.0f);
    }
    
    auto peak_power_it = peak_power_sensors_.find(node.addr);
    if (peak_power_it != peak_power_sensors_.end()) {
      peak_power_it->second->publish_state(saved_peak);  // Use saved peak power
      ESP_LOGD(TAG, "Published saved peak power for %s: %.0fW", node.addr.c_str(), saved_peak);
    }
    
    auto rssi_it = rssi_sensors_.find(node.addr);
    if (rssi_it != rssi_sensors_.end()) {
      rssi_it->second->publish_state(0.0f);
    }
    
    auto barcode_it = barcode_sensors_.find(node.addr);
    if (barcode_it != barcode_sensors_.end()) {
      barcode_it->second->publish_state(node.long_address);
    }
    
    auto duty_cycle_it = duty_cycle_sensors_.find(node.addr);
    if (duty_cycle_it != duty_cycle_sensors_.end()) {
      duty_cycle_it->second->publish_state(0.0f);
    }
    
    auto firmware_version_it = firmware_version_sensors_.find(node.addr);
    if (firmware_version_it != firmware_version_sensors_.end()) {
      firmware_version_it->second->publish_state("unknown");
    }
    
    auto efficiency_it = efficiency_sensors_.find(node.addr);
    if (efficiency_it != efficiency_sensors_.end()) {
      efficiency_it->second->publish_state(0.0f);
    }
    
    auto power_factor_it = power_factor_sensors_.find(node.addr);
    if (power_factor_it != power_factor_sensors_.end()) {
      power_factor_it->second->publish_state(0.0f);
    }
    
    auto load_factor_it = load_factor_sensors_.find(node.addr);
    if (load_factor_it != load_factor_sensors_.end()) {
      load_factor_it->second->publish_state(0.0f);
    }
  }
  
  // Update string-level aggregation data
  update_string_data();
  
  // Update inverter-level aggregation if inverters are configured
  if (!inverters_.empty()) {
    update_inverter_data();
  }
}

std::string TigoMonitorComponent::remove_escape_sequences(const std::string &frame) {
#ifdef USE_ESP_IDF
  // Use PSRAM for large temporary buffer to avoid internal RAM fragmentation
  psram_string result;
  result.reserve(frame.length());
  
  for (size_t i = 0; i < frame.length(); ++i) {
    if (frame[i] == '\x7E' && i < frame.length() - 1) {
      char next_byte = frame[i + 1];
      switch (next_byte) {
        case '\x00': result.push_back('\x7E'); break; // Escaped 7E -> raw 7E
        case '\x01': result.push_back('\x24'); break; // Escaped 7E 01 -> raw 24
        case '\x02': result.push_back('\x23'); break; // Escaped 7E 02 -> raw 23
        case '\x03': result.push_back('\x25'); break; // Escaped 7E 03 -> raw 25
        case '\x04': result.push_back('\xA4'); break; // Escaped 7E 04 -> raw A4
        case '\x05': result.push_back('\xA3'); break; // Escaped 7E 05 -> raw A3
        case '\x06': result.push_back('\xA5'); break; // Escaped 7E 06 -> raw A5
        default:
          result.push_back(frame[i]);
          result.push_back(next_byte);
          break;
      }
      i++; // Skip next byte
    } else {
      result.push_back(frame[i]);
    }
  }
  // Convert back to std::string for compatibility with existing code
  return std::string(result.begin(), result.end());
#else
  std::string result;
  result.reserve(frame.length());
  
  for (size_t i = 0; i < frame.length(); ++i) {
    if (frame[i] == '\x7E' && i < frame.length() - 1) {
      char next_byte = frame[i + 1];
      switch (next_byte) {
        case '\x00': result.push_back('\x7E'); break; // Escaped 7E -> raw 7E
        case '\x01': result.push_back('\x24'); break; // Escaped 7E 01 -> raw 24
        case '\x02': result.push_back('\x23'); break; // Escaped 7E 02 -> raw 23
        case '\x03': result.push_back('\x25'); break; // Escaped 7E 03 -> raw 25
        case '\x04': result.push_back('\xA4'); break; // Escaped 7E 04 -> raw A4
        case '\x05': result.push_back('\xA3'); break; // Escaped 7E 05 -> raw A3
        case '\x06': result.push_back('\xA5'); break; // Escaped 7E 06 -> raw A5
        default:
          result.push_back(frame[i]);
          result.push_back(next_byte);
          break;
      }
      i++; // Skip next byte
    } else {
      result.push_back(frame[i]);
    }
  }
  return result;
#endif
}

bool TigoMonitorComponent::verify_checksum(const std::string &frame) {
  if (frame.length() < 2) {
    ESP_LOGV(TAG, "Frame too short for checksum verification: %zu bytes", frame.length());
    return false;
  }
  
  // Safety check: prevent potential out-of-bounds access
  if (frame.length() > 10000) {
    ESP_LOGW(TAG, "Frame suspiciously large: %zu bytes, rejecting", frame.length());
    return false;
  }
  
  std::string checksum_str = frame.substr(frame.length() - 2);
  uint16_t extracted_checksum = (static_cast<uint8_t>(checksum_str[0]) << 8) | 
                                static_cast<uint8_t>(checksum_str[1]);
  
  uint16_t computed_checksum = compute_crc16_ccitt(
    reinterpret_cast<const uint8_t*>(frame.c_str()), 
    frame.length() - 2
  );
  
  return extracted_checksum == computed_checksum;
}

std::string TigoMonitorComponent::frame_to_hex_string(const std::string &frame) {
#ifdef USE_ESP_IDF
  // Use PSRAM for hex conversion buffer to save internal RAM
  // Hex strings can be 2KB+ for large frames
  psram_string hex_str;
  hex_str.reserve(frame.length() * 2);
  
  for (unsigned char byte : frame) {
    char hex_chars[3];
    sprintf(hex_chars, "%02X", byte);
    hex_str.push_back(hex_chars[0]);
    hex_str.push_back(hex_chars[1]);
  }
  // Convert back to std::string for compatibility with existing code
  return std::string(hex_str.begin(), hex_str.end());
#else
  std::string hex_str;
  hex_str.reserve(frame.length() * 2);
  
  for (unsigned char byte : frame) {
    char hex_chars[3];
    sprintf(hex_chars, "%02X", byte);
    hex_str.push_back(hex_chars[0]);
    hex_str.push_back(hex_chars[1]);
  }
  return hex_str;
#endif
}

int TigoMonitorComponent::calculate_header_length(const std::string &hex_frame) {
  // Convert from little-endian: swap bytes
  std::string low_byte = hex_frame.substr(0, 2);
  std::string high_byte = hex_frame.substr(2, 2);
  std::string status_hex = low_byte + high_byte;
  
  unsigned int status = std::stoul(status_hex, nullptr, 16);
  int length = 2; // Status word is always 2 bytes
  
  // Check bits according to original logic
  if ((status & (1 << 0)) == 0) length += 1;
  if ((status & (1 << 1)) == 0) length += 1;
  if ((status & (1 << 2)) == 0) length += 2;
  if ((status & (1 << 3)) == 0) length += 2;
  if ((status & (1 << 4)) == 0) length += 1;
  length += 1; // Bit 5
  length += 2; // Bit 6
  
  return length * 2; // Convert to hex characters
}

void TigoMonitorComponent::generate_crc_table() {
  for (uint16_t i = 0; i < CRC_TABLE_SIZE; ++i) {
    uint16_t crc = i;
    for (uint8_t j = 8; j > 0; --j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ CRC_POLYNOMIAL;
      } else {
        crc >>= 1;
      }
    }
    crc_table_[i] = crc;
  }
}

uint16_t TigoMonitorComponent::compute_crc16_ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0x8408; // Initial value
  for (size_t i = 0; i < length; i++) {
    uint8_t index = (crc ^ data[i]) & 0xFF;
    crc = (crc >> 8) ^ crc_table_[index];
  }
  crc = (crc >> 8) | (crc << 8);
  return crc;
}

char TigoMonitorComponent::compute_tigo_crc4(const std::string &hex_string) {
  uint8_t crc = 0x2;
  for (size_t i = 0; i < hex_string.length(); i += 2) {
    if (i + 1 >= hex_string.length()) break;
    
    std::string byte_str = hex_string.substr(i, 2);
    uint8_t byte_val = std::stoul(byte_str, nullptr, 16);
    crc = tigo_crc_table_[byte_val ^ (crc << 4)];
  }
  return crc_char_map_[crc];
}



void TigoMonitorComponent::generate_sensor_yaml() {
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "=== COPY THE FOLLOWING YAML CONFIGURATION ===");
  ESP_LOGI(TAG, "# Add this to your ESPHome YAML file under 'sensor:'");
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "sensor:");
  
  // First, generate configs for nodes with assigned sensor indices
  std::vector<NodeTableData> assigned_nodes;
  for (const auto& node : node_table_) {
    if (node.sensor_index >= 0) {
      assigned_nodes.push_back(node);
    }
  }
  
  if (!assigned_nodes.empty()) {
    // Sort by sensor index
    std::sort(assigned_nodes.begin(), assigned_nodes.end(), 
              [](const auto& a, const auto& b) { return a.sensor_index < b.sensor_index; });
    
    ESP_LOGI(TAG, "  # Discovered devices with actual addresses:");
    for (const auto& node : assigned_nodes) {
      std::string index_str = std::to_string(node.sensor_index + 1);
      
      // Build barcode comment from Frame 27 data
      std::string barcode_comment = "";
      if (!node.long_address.empty()) {
        barcode_comment = " - Frame27: " + node.long_address;
      }
      
      ESP_LOGI(TAG, "  # Tigo Device %s (discovered%s)", index_str.c_str(), barcode_comment.c_str());
      ESP_LOGI(TAG, "  - platform: tigo_server");
      ESP_LOGI(TAG, "    tigo_monitor_id: tigo_hub");
      ESP_LOGI(TAG, "    address: \"%s\"", node.addr.c_str());
      ESP_LOGI(TAG, "    name: \"Tigo Device %s\"", index_str.c_str());
      ESP_LOGI(TAG, "    power: {}");
      ESP_LOGI(TAG, "    voltage_in: {}");
      ESP_LOGI(TAG, "    voltage_out: {}");
      ESP_LOGI(TAG, "    current_in: {}");
      ESP_LOGI(TAG, "    temperature: {}");
      ESP_LOGI(TAG, "    rssi: {}");
      ESP_LOGI(TAG, "");
    }
  }
  
  // Generate placeholders for remaining devices if needed
  int discovered_count = assigned_nodes.size();
  if (discovered_count < number_of_devices_) {
    ESP_LOGI(TAG, "  # Additional device slots (update addresses when devices are discovered):");
    for (int i = discovered_count; i < number_of_devices_; i++) {
      std::string index_str = std::to_string(i + 1);
      
      ESP_LOGI(TAG, "  # Tigo Device %s (placeholder - update address when discovered)", index_str.c_str());
      ESP_LOGI(TAG, "  - platform: tigo_server");
      ESP_LOGI(TAG, "    tigo_monitor_id: tigo_hub");
      ESP_LOGI(TAG, "    address: \"device_%s\"  # CHANGE THIS to actual device address", index_str.c_str());
      ESP_LOGI(TAG, "    name: \"Tigo Device %s\"", index_str.c_str());
      ESP_LOGI(TAG, "    power: {}");
      ESP_LOGI(TAG, "    voltage_in: {}");
      ESP_LOGI(TAG, "    voltage_out: {}");
      ESP_LOGI(TAG, "    current_in: {}");
      ESP_LOGI(TAG, "    temperature: {}");
      ESP_LOGI(TAG, "    rssi: {}");
      ESP_LOGI(TAG, "");
    }
  }
  
  ESP_LOGI(TAG, "# GENERATION SUMMARY:");
  ESP_LOGI(TAG, "# - Found %d discovered devices with real addresses", discovered_count);
  ESP_LOGI(TAG, "# - Generated %d placeholder configs for remaining devices", number_of_devices_ - discovered_count);
  ESP_LOGI(TAG, "# - Total configuration slots: %d", number_of_devices_);
  ESP_LOGI(TAG, "=== END YAML CONFIGURATION ===");
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "Auto-templated YAML generated! Each device creates 6 sensors with names based on 'name' field:");
  ESP_LOGI(TAG, "- [name] Power (W), [name] Voltage In (V), [name] Voltage Out (V)");
  ESP_LOGI(TAG, "- [name] Current (A), [name] Temperature (°C), [name] RSSI (dBm)");
  ESP_LOGI(TAG, "Copy the configuration above - sensor names are auto-generated from base name!");
}

void TigoMonitorComponent::print_device_mappings() {
  ESP_LOGI(TAG, "=== UNIFIED NODE TABLE ===");
  
  // Count nodes with sensor assignments
  int assigned_count = 0;
  for (const auto& node : node_table_) {
    if (node.sensor_index >= 0) assigned_count++;
  }
  
  if (assigned_count == 0) {
    ESP_LOGI(TAG, "No devices have been assigned sensor indices yet.");
  } else {
    ESP_LOGI(TAG, "Found %d device sensor assignments:", assigned_count);
    ESP_LOGI(TAG, "");
    
    // Sort nodes by sensor index for cleaner display
    std::vector<NodeTableData> sorted_nodes;
    for (const auto& node : node_table_) {
      if (node.sensor_index >= 0) {
        sorted_nodes.push_back(node);
      }
    }
    std::sort(sorted_nodes.begin(), sorted_nodes.end(), 
              [](const auto& a, const auto& b) { return a.sensor_index < b.sensor_index; });
    
    for (const auto& node : sorted_nodes) {
      std::string info = "Device Address " + node.addr;
      
      // Add Frame 27 long address (only barcode source)
      if (!node.long_address.empty()) {
        info += " (Frame27: " + node.long_address + ")";
      }
      
      ESP_LOGI(TAG, "  Tigo %d: %s", node.sensor_index + 1, info.c_str());
    }
    ESP_LOGI(TAG, "");
  }
  
  // Show nodes without sensor assignments (discovered but not active)
  std::vector<NodeTableData> unassigned_nodes;
  for (const auto& node : node_table_) {
    if (node.sensor_index == -1) {
      unassigned_nodes.push_back(node);
    }
  }
  
  if (!unassigned_nodes.empty()) {
    ESP_LOGI(TAG, "Discovered devices without sensor assignments (%d):", unassigned_nodes.size());
    for (const auto& node : unassigned_nodes) {
      std::string info = "Device " + node.addr;
      if (!node.long_address.empty()) {
        info += " (barcode: " + node.long_address + ")";
      }
      info += " - waiting for power data";
      ESP_LOGI(TAG, "  %s", info.c_str());
    }
    ESP_LOGI(TAG, "");
  }
  
  // Show currently active devices
  if (!devices_.empty()) {
    ESP_LOGI(TAG, "Currently active devices (%d):", devices_.size());
    for (const auto& device : devices_) {
      NodeTableData* node = find_node_by_addr(device.addr);
      std::string status = "unmapped";
      if (node != nullptr && node->sensor_index >= 0) {
        status = "mapped to Tigo " + std::to_string(node->sensor_index + 1);
      }
      
      std::string name = device.barcode.empty() ? ("mod#" + device.addr) : device.barcode;
      std::string data_sources = "";
      if (node != nullptr && !node->long_address.empty()) {
        data_sources = " [Frame27]";
      }
      
      ESP_LOGI(TAG, "  Device %s (%s): %s%s", device.addr.c_str(), name.c_str(), status.c_str(), data_sources.c_str());
    }
    ESP_LOGI(TAG, "");
  }
  
  // Show next available sensor index
  int next_index = get_next_available_sensor_index();
  if (next_index >= 0) {
    ESP_LOGI(TAG, "Next available sensor index: Tigo %d", next_index + 1);
  } else {
    ESP_LOGI(TAG, "All %d sensor slots are assigned", number_of_devices_);
  }
  
  ESP_LOGI(TAG, "Total node table entries: %d", node_table_.size());
  ESP_LOGI(TAG, "=== END UNIFIED NODE TABLE ===");
}

void TigoMonitorComponent::load_node_table() {
  ESP_LOGI(TAG, "Loading persistent node table...");
  
  // Pre-allocate string buffer to avoid repeated allocations
  static std::string pref_key;
  pref_key.reserve(32);
  
  int loaded_count = 0;
  // Load node table entries up to the configured number of devices
  for (int i = 0; i < number_of_devices_; i++) {
    pref_key = "node_";
    pref_key += std::to_string(i);
    uint32_t hash = esphome::fnv1_hash(pref_key);
    
    // Use char array for ESPHome preferences
    auto restore = global_preferences->make_preference<char[256]>(hash);
    char node_data[256] = {0};
    
    if (restore.load(&node_data) && strlen(node_data) > 0) {
      // Format: "addr|long_addr|checksum|barcode|sensor_index"
      std::string node_str(node_data);
      std::vector<std::string> parts;
      size_t start = 0, end = 0;
      
      // Split by '|' delimiter
      while ((end = node_str.find('|', start)) != std::string::npos) {
        parts.push_back(node_str.substr(start, end - start));
        start = end + 1;
      }
      parts.push_back(node_str.substr(start)); // Last part
      
      // Current format (9 fields): addr|long_address|checksum|sensor_index|cca_label|cca_string|cca_inverter|cca_channel|cca_validated
      // Old format (10 fields): addr|long_address|checksum|frame09_barcode|sensor_index|cca_label|cca_string|cca_inverter|cca_channel|cca_validated
      // Legacy format (4 fields): addr|long_address|checksum|sensor_index
      if (parts.size() >= 4) {
        NodeTableData node;
        node.addr = parts[0];
        node.long_address = parts[1];
        node.checksum = parts[2];
        
        // Determine sensor index position based on format
        int sensor_idx_pos = (parts.size() >= 10) ? 4 : 3;  // Old format with frame09 at position 3
        if (sensor_idx_pos < parts.size()) {
          node.sensor_index = std::stoi(parts[sensor_idx_pos]);
          node.is_persistent = true;
        } else {
          continue;  // Skip malformed entry
        }
        
        // Load CCA fields if available
        if (parts.size() >= 10) {
          // Old format: addr|long_addr|checksum|frame09|sensor_idx|cca_label|cca_string|cca_inverter|cca_channel|cca_validated
          node.cca_label = parts[5];
          node.cca_string_label = parts[6];
          node.cca_inverter_label = parts[7];
          node.cca_channel = parts[8];
          node.cca_validated = (parts[9] == "1");
          
          // Replace "Inverter" with "MPPT" for more accurate terminology
          if (node.cca_inverter_label.find("Inverter ") == 0) {
            node.cca_inverter_label.replace(0, 9, "MPPT ");
          }
          
          ESP_LOGI(TAG, "Restored node (old format): %s -> Tigo %d (barcode: %s, string: %s, validated: %s)", 
                   node.addr.c_str(), node.sensor_index + 1, node.long_address.c_str(),
                   node.cca_string_label.c_str(), node.cca_validated ? "yes" : "no");
        } else if (parts.size() >= 9) {
          // Current format: addr|long_addr|checksum|sensor_idx|cca_label|cca_string|cca_inverter|cca_channel|cca_validated
          node.cca_label = parts[4];
          node.cca_string_label = parts[5];
          node.cca_inverter_label = parts[6];
          node.cca_channel = parts[7];
          node.cca_validated = (parts[8] == "1");
          
          // Replace "Inverter" with "MPPT" for more accurate terminology
          if (node.cca_inverter_label.find("Inverter ") == 0) {
            node.cca_inverter_label.replace(0, 9, "MPPT ");
          }
          
          ESP_LOGI(TAG, "Restored node with CCA: %s -> Tigo %d (barcode: %s, string: %s, validated: %s)", 
                   node.addr.c_str(), node.sensor_index + 1, node.long_address.c_str(),
                   node.cca_string_label.c_str(), node.cca_validated ? "yes" : "no");
        } else {
          // Old format without CCA fields - initialize to defaults
          node.cca_label = "";
          node.cca_string_label = "";
          node.cca_inverter_label = "";
          node.cca_channel = "";
          node.cca_validated = false;
          
          ESP_LOGI(TAG, "Restored node (legacy format): %s -> Tigo %d (barcode: %s)", 
                   node.addr.c_str(), node.sensor_index + 1, node.long_address.c_str());
        }
        
        node_table_.push_back(node);
        loaded_count++;
      }
    }
  }
  
  ESP_LOGI(TAG, "Loaded %d persistent node table entries (capacity: %d devices)", loaded_count, number_of_devices_);
}

void TigoMonitorComponent::save_node_table() {
  ESP_LOGD(TAG, "Saving node table to flash...");
  
  // Pre-allocate string buffer to avoid repeated allocations
  static std::string pref_key;
  pref_key.reserve(32);
  
  // Clear old entries first
  for (int i = 0; i < number_of_devices_; i++) {
    pref_key = "node_";
    pref_key += std::to_string(i);
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto save = global_preferences->make_preference<char[256]>(hash);
    char empty_data[256] = {0};
    save.save(&empty_data);
  }
  
  // Save current persistent nodes
  int i = 0;
  int saved_count = 0;
  for (const auto &node : node_table_) {
    if (i >= number_of_devices_) break; // Safety limit
    if (!node.is_persistent) continue;   // Only save persistent nodes
    
    pref_key = "node_";
    pref_key += std::to_string(i);
    uint32_t hash = esphome::fnv1_hash(pref_key);
    
    // Format: "addr|long_addr|checksum|sensor_index|cca_label|cca_string|cca_inverter|cca_channel|cca_validated"
    // Note: frame09_barcode field removed (was between checksum and sensor_index)
    std::string node_str = node.addr + "|" + 
                          node.long_address + "|" + 
                          node.checksum + "|" + 
                          std::to_string(node.sensor_index) + "|" +
                          node.cca_label + "|" +
                          node.cca_string_label + "|" +
                          node.cca_inverter_label + "|" +
                          node.cca_channel + "|" +
                          (node.cca_validated ? "1" : "0");
    
    // Copy to char array for preferences
    char node_data[256] = {0};
    strncpy(node_data, node_str.c_str(), sizeof(node_data) - 1);
    
    auto save = global_preferences->make_preference<char[256]>(hash);
    save.save(&node_data);
    saved_count++;
    i++;
  }
  
  ESP_LOGD(TAG, "Saved %d node table entries to flash", saved_count);
}

void TigoMonitorComponent::save_peak_power_data() {
  ESP_LOGD(TAG, "Saving peak power data to flash...");
  
  // Pre-allocate string buffer to avoid repeated allocations
  static std::string pref_key;
  pref_key.reserve(32);
  
  int saved_count = 0;
  for (size_t i = 0; i < devices_.size(); i++) {
    const auto &device = devices_[i];
    if (device.peak_power > 0.0f) {
      // Reuse string buffer instead of creating new string each iteration
      pref_key = "peak_";
      pref_key += device.addr;
      uint32_t hash = esphome::fnv1_hash(pref_key);
      auto save = global_preferences->make_preference<float>(hash);
      save.save(&device.peak_power);
      saved_count++;
    }
  }
  
  ESP_LOGD(TAG, "Saved %d peak power entries to flash", saved_count);
}

void TigoMonitorComponent::load_peak_power_data() {
  ESP_LOGD(TAG, "Loading peak power data from flash...");
  
  // Pre-allocate string buffer to avoid repeated allocations
  static std::string pref_key;
  pref_key.reserve(32);
  
  int loaded_count = 0;
  for (size_t i = 0; i < devices_.size(); i++) {
    auto &device = devices_[i];
    // Reuse string buffer instead of creating new string each iteration
    pref_key = "peak_";
    pref_key += device.addr;
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto load = global_preferences->make_preference<float>(hash);
    
    float saved_peak = 0.0f;
    if (load.load(&saved_peak) && saved_peak > 0.0f) {
      device.peak_power = saved_peak;
      loaded_count++;
      ESP_LOGD(TAG, "Loaded peak power for %s: %.0fW", device.addr.c_str(), saved_peak);
    }
  }
  
  ESP_LOGI(TAG, "Loaded %d peak power entries from flash", loaded_count);
}

void TigoMonitorComponent::reset_peak_power() {
  ESP_LOGI(TAG, "Resetting all peak power values...");
  
#ifdef USE_ESP_IDF
  // Log heap before reset
  size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  ESP_LOGD(TAG, "Internal heap before reset: %zu bytes", heap_before);
#endif
  
  // Pre-allocate string buffer to avoid repeated allocations
  static std::string pref_key;
  pref_key.reserve(32);
  
  int reset_count = 0;
  for (size_t i = 0; i < devices_.size(); i++) {
    auto &device = devices_[i];
    device.peak_power = 0.0f;
    
    // Clear from flash storage - reuse string buffer
    pref_key = "peak_";
    pref_key += device.addr;
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto save = global_preferences->make_preference<float>(hash);
    float zero = 0.0f;
    save.save(&zero);
    reset_count++;
  }
  
#ifdef USE_ESP_IDF
  // Log heap after reset
  size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  ESP_LOGI(TAG, "Reset %d peak power values (heap: %zu -> %zu, delta: %zd bytes)", 
           reset_count, heap_before, heap_after, (ssize_t)(heap_after - heap_before));
#else
  ESP_LOGI(TAG, "Reset %d peak power values", reset_count);
#endif
}

void TigoMonitorComponent::reset_total_energy() {
  ESP_LOGI(TAG, "Resetting total energy...");
  
  total_energy_kwh_ = 0.0f;
  
  // Publish the reset value
  if (energy_sum_sensor_ != nullptr) {
    energy_sum_sensor_->publish_state(0.0f);
  }
  
  // Save to persistent storage
  save_energy_data();
  
  ESP_LOGI(TAG, "Total energy reset to 0 kWh");
}

void TigoMonitorComponent::check_midnight_reset() {
#ifdef USE_TIME
  if (!reset_at_midnight_) {
    return;  // Reset at midnight not enabled
  }
  
  if (time_id_ == nullptr) {
    return;  // No time component configured
  }
  
  auto now = time_id_->now();
  if (!now.is_valid()) {
    return;  // Time not synced yet
  }
  
  int current_day = now.day_of_year;
  
  // Check if we've crossed into a new day
  if (last_reset_day_ != -1 && last_reset_day_ != current_day) {
    ESP_LOGI(TAG, "Midnight detected - resetting peak power and total energy (day %d -> %d)", last_reset_day_, current_day);
    
#ifdef USE_ESP_IDF
    // Log heap before midnight reset operations
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min_before = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Heap before midnight reset: %zu bytes free (%zu min)", heap_before, heap_min_before);
#endif
    
    // Reset total energy
    reset_total_energy();
    
    // Reset all peak power values
    reset_peak_power();
    
#ifdef USE_ESP_IDF
    // Log heap after midnight reset operations
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min_after = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Heap after midnight reset: %zu bytes free (%zu min)", heap_after, heap_min_after);
    ESP_LOGI(TAG, "Midnight reset impact: free delta=%zd bytes, min delta=%zd bytes",
             (ssize_t)(heap_after - heap_before), (ssize_t)(heap_min_after - heap_min_before));
#endif
  }
  
  last_reset_day_ = current_day;
#endif
}

void TigoMonitorComponent::save_persistent_data() {
  ESP_LOGI(TAG, "Saving all persistent data to flash (node table, peak power, energy, daily history)...");
  save_node_table();
  save_peak_power_data();
  save_energy_data();
  save_daily_energy_history();
  ESP_LOGI(TAG, "All persistent data saved successfully");
}

void TigoMonitorComponent::reset_node_table() {
  ESP_LOGI(TAG, "Resetting node table - clearing all persistent device mappings...");
  
  int cleared_count = node_table_.size();
  
  // Clear the in-memory node table
  node_table_.clear();
  
  // Clear all persistent storage entries
  for (int i = 0; i < number_of_devices_; i++) {
    std::string pref_key = "node_" + std::to_string(i);
    uint32_t hash = esphome::fnv1_hash(pref_key);
    auto save = global_preferences->make_preference<char[256]>(hash);
    char empty_data[256] = {0};
    save.save(&empty_data);
  }
  
  // Clear the device sensor index cache
  created_devices_.clear();
  
  ESP_LOGI(TAG, "Node table reset complete:");
  ESP_LOGI(TAG, "- Cleared %d node table entries", cleared_count);
  ESP_LOGI(TAG, "- Cleared %d persistent storage slots", number_of_devices_);
  ESP_LOGI(TAG, "- Reset device sensor index cache");
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "All device mappings have been removed. Devices will be");
  ESP_LOGI(TAG, "rediscovered and reassigned new sensor indices when they");
  ESP_LOGI(TAG, "send power data. Frame 09 and Frame 27 data will be");
  ESP_LOGI(TAG, "recollected automatically during normal operation.");
}

bool TigoMonitorComponent::remove_node(uint16_t addr) {
  ESP_LOGI(TAG, "Removing node with address: 0x%04X", addr);
  
  // Convert addr to hex string for comparison (lowercase)
  char addr_hex[5];
  snprintf(addr_hex, sizeof(addr_hex), "%04x", addr);
  std::string addr_str(addr_hex);
  
  // Find the node in the table (case-insensitive comparison)
  auto it = std::find_if(node_table_.begin(), node_table_.end(),
    [&addr_str](const NodeTableData &node) {
      // Convert both to lowercase for comparison
      std::string node_addr_lower = node.addr;
      std::transform(node_addr_lower.begin(), node_addr_lower.end(), node_addr_lower.begin(), ::tolower);
      return node_addr_lower == addr_str;
    });
  
  if (it == node_table_.end()) {
    ESP_LOGW(TAG, "Node with address 0x%04X (%s) not found in table", addr, addr_str.c_str());
    return false;
  }
  
  int sensor_index = it->sensor_index;
  
  // Remove from created_devices_ cache if it has a sensor index
  if (sensor_index >= 0) {
    std::string device_key = "tigo_" + std::to_string(sensor_index);
    created_devices_.erase(device_key);
  }
  
  // Remove from node table
  node_table_.erase(it);
  
  // Save updated node table to persistent storage
  save_node_table();
  
  ESP_LOGI(TAG, "Successfully removed node 0x%04X (sensor index: %d)", addr, sensor_index);
  return true;
}

bool TigoMonitorComponent::import_node_table(const std::vector<NodeTableData>& nodes) {
  ESP_LOGI(TAG, "Importing node table with %zu nodes", nodes.size());
  
  // Clear existing node table
  node_table_.clear();
  created_devices_.clear();
  
  // Reserve capacity to avoid reallocations during import
  node_table_.reserve(nodes.size());
  
  // Import all nodes
  for (const auto& node : nodes) {
    // Validate node data
    if (node.addr.empty()) {
      ESP_LOGW(TAG, "Skipping node with empty address");
      continue;
    }
    
    // Check for duplicate addresses
    bool duplicate = false;
    for (const auto& existing : node_table_) {
      if (existing.addr == node.addr) {
        ESP_LOGW(TAG, "Skipping duplicate node with address: %s", node.addr.c_str());
        duplicate = true;
        break;
      }
    }
    
    if (duplicate) continue;
    
    // Add node to table
    node_table_.push_back(node);
    
    ESP_LOGD(TAG, "Imported node: addr=%s, barcode=%s, sensor_index=%d, cca_label=%s",
             node.addr.c_str(), 
             node.long_address.c_str(),
             node.sensor_index,
             node.cca_label.c_str());
  }
  
  // Save imported node table to persistent storage
  save_node_table();
  
  ESP_LOGI(TAG, "Successfully imported %zu nodes", node_table_.size());
  return true;
}

int TigoMonitorComponent::get_next_available_sensor_index() {
  // Create a set of used indices from the unified node table
  std::set<int> used_indices;
  for (const auto &node : node_table_) {
    if (node.sensor_index >= 0) {
      used_indices.insert(node.sensor_index);
    }
  }
  
  // Find the first available index
  for (int i = 0; i < number_of_devices_; i++) {
    if (used_indices.find(i) == used_indices.end()) {
      return i;
    }
  }
  
  return -1; // No available indices
}

std::string TigoMonitorComponent::get_device_name(const DeviceData &device) {
  if (!device.barcode.empty() && device.barcode.length() >= 5) {
    return "Tigo " + device.barcode;
  }
  return "Tigo Module " + device.addr;
}

NodeTableData* TigoMonitorComponent::find_node_by_addr(const std::string &addr) {
  for (auto &node : node_table_) {
    if (node.addr == addr) {
      return &node;
    }
  }
  return nullptr;
}

void TigoMonitorComponent::assign_sensor_index_to_node(const std::string &addr) {
  NodeTableData* node = find_node_by_addr(addr);
  
  if (node == nullptr) {
    // Create a new node entry if it doesn't exist
    if (node_table_.size() < number_of_devices_) {
      NodeTableData new_node;
      new_node.addr = addr;
      new_node.sensor_index = -1;  // Will be assigned below
      new_node.is_persistent = true;
      node_table_.push_back(new_node);
      node = &node_table_.back();  // Point to the newly added node
      ESP_LOGI(TAG, "Created new node entry for power data device: %s", addr.c_str());
    } else {
      ESP_LOGW(TAG, "Cannot create node entry - node table is full (%d devices)", number_of_devices_);
      return;
    }
  }
  
  if (node->sensor_index == -1) {
    int index = get_next_available_sensor_index();
    if (index >= 0) {
      node->sensor_index = index;
      node->is_persistent = true;
      ESP_LOGI(TAG, "Assigned sensor index %d to device %s", index + 1, addr.c_str());
      save_node_table();
    } else {
      ESP_LOGW(TAG, "No available sensor index for device %s", addr.c_str());
    }
  }
}

void TigoMonitorComponent::load_energy_data() {
  auto restore = global_preferences->make_preference<float>(ENERGY_DATA_HASH);
  if (restore.load(&total_energy_kwh_)) {
    ESP_LOGI(TAG, "Restored total energy: %.3f kWh", total_energy_kwh_);
  } else {
    ESP_LOGI(TAG, "No previous energy data found, starting from 0 kWh");
    total_energy_kwh_ = 0.0f;
  }
}

void TigoMonitorComponent::save_energy_data() {
  auto save = global_preferences->make_preference<float>(ENERGY_DATA_HASH);
  if (save.save(&total_energy_kwh_)) {
    ESP_LOGD(TAG, "Saved energy data: %.3f kWh", total_energy_kwh_);
  } else {
    ESP_LOGW(TAG, "Failed to save energy data");
  }
}

void TigoMonitorComponent::update_daily_energy(float energy_kwh) {
#ifdef USE_TIME
  if (time_id_ == nullptr) {
    return;  // No time component
  }
  
  auto now = time_id_->now();
  if (!now.is_valid()) {
    return;  // Time not synced
  }
  
  // Generate current day key (YYYYMMDD format)
  uint32_t day_key = (now.year * 10000) + (now.month * 100) + now.day_of_month;
  
  // Check if this is a new day
  if (current_day_key_ != day_key) {
    // New day - archive yesterday's energy if we have it
    if (current_day_key_ != 0 && total_energy_kwh_ > 0.0f) {
      DailyEnergyData yesterday = DailyEnergyData::from_key(current_day_key_);
      yesterday.energy_kwh = total_energy_kwh_;
      
      // Add to history (or update if exists)
      bool found = false;
      for (auto &entry : daily_energy_history_) {
        if (entry.to_key() == current_day_key_) {
          entry.energy_kwh = total_energy_kwh_;
          found = true;
          break;
        }
      }
      if (!found) {
        daily_energy_history_.push_back(yesterday);
      }
      
      // Keep only last 7 days
      if (daily_energy_history_.size() > MAX_DAILY_HISTORY) {
        daily_energy_history_.erase(daily_energy_history_.begin());
      }
      
      ESP_LOGI(TAG, "Archived daily energy: %04d-%02d-%02d = %.3f kWh", 
               yesterday.year, yesterday.month, yesterday.day, yesterday.energy_kwh);
      
      // Save to flash
      save_daily_energy_history();
    }
    
    current_day_key_ = day_key;
  }
#endif
}

void TigoMonitorComponent::save_daily_energy_history() {
  ESP_LOGD(TAG, "Saving daily energy history to flash (%zu days)...", daily_energy_history_.size());
  
  // Pack history into a simple buffer
  // Format: count (1 byte) + array of [key (4 bytes) + energy (4 bytes)]
  static const size_t MAX_BUFFER_SIZE = 1 + (MAX_DAILY_HISTORY * 8);
  uint8_t buffer[MAX_BUFFER_SIZE] = {0};
  
  size_t count = std::min(daily_energy_history_.size(), MAX_DAILY_HISTORY);
  buffer[0] = static_cast<uint8_t>(count);
  
  size_t offset = 1;
  for (size_t i = 0; i < count; i++) {
    uint32_t key = daily_energy_history_[i].to_key();
    float energy = daily_energy_history_[i].energy_kwh;
    
    // Store key (4 bytes)
    memcpy(&buffer[offset], &key, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    // Store energy (4 bytes)
    memcpy(&buffer[offset], &energy, sizeof(float));
    offset += sizeof(float);
  }
  
  uint32_t hash = esphome::fnv1_hash("daily_energy_history");
  auto save = global_preferences->make_preference<uint8_t[MAX_BUFFER_SIZE]>(hash);
  if (save.save(&buffer)) {
    ESP_LOGD(TAG, "Saved %zu daily energy entries to flash", count);
  } else {
    ESP_LOGW(TAG, "Failed to save daily energy history");
  }
}

void TigoMonitorComponent::load_daily_energy_history() {
  ESP_LOGD(TAG, "Loading daily energy history from flash...");
  
  static const size_t MAX_BUFFER_SIZE = 1 + (MAX_DAILY_HISTORY * 8);
  uint8_t buffer[MAX_BUFFER_SIZE] = {0};
  
  uint32_t hash = esphome::fnv1_hash("daily_energy_history");
  auto load = global_preferences->make_preference<uint8_t[MAX_BUFFER_SIZE]>(hash);
  
  if (!load.load(&buffer)) {
    ESP_LOGD(TAG, "No saved daily energy history found");
    return;
  }
  
  size_t count = buffer[0];
  if (count > MAX_DAILY_HISTORY) {
    ESP_LOGW(TAG, "Invalid daily energy history count: %zu", count);
    return;
  }
  
  daily_energy_history_.clear();
  size_t offset = 1;
  
  for (size_t i = 0; i < count; i++) {
    uint32_t key;
    float energy;
    
    // Load key (4 bytes)
    memcpy(&key, &buffer[offset], sizeof(uint32_t));
    offset += sizeof(uint32_t);
    
    // Load energy (4 bytes)
    memcpy(&energy, &buffer[offset], sizeof(float));
    offset += sizeof(float);
    
    DailyEnergyData entry = DailyEnergyData::from_key(key);
    entry.energy_kwh = energy;
    daily_energy_history_.push_back(entry);
    
    ESP_LOGD(TAG, "Loaded: %04d-%02d-%02d = %.3f kWh", 
             entry.year, entry.month, entry.day, entry.energy_kwh);
  }
  
  ESP_LOGI(TAG, "Loaded %zu daily energy entries from flash", count);
}

std::vector<DailyEnergyData> TigoMonitorComponent::get_daily_energy_history() const {
  return daily_energy_history_;
}

// ========== CCA HTTP Query Functions ==========

void TigoMonitorComponent::sync_from_cca() {
  if (cca_ip_.empty()) {
    ESP_LOGW(TAG, "Cannot sync from CCA - no IP address configured");
    return;
  }
  
  ESP_LOGI(TAG, "Syncing device configuration from CCA at %s...", cca_ip_.c_str());
  query_cca_config();
}

void TigoMonitorComponent::refresh_cca_data() {
  if (cca_ip_.empty()) {
    ESP_LOGW(TAG, "Cannot refresh CCA data - no IP address configured");
    return;
  }
  
  ESP_LOGI(TAG, "Refreshing CCA data from %s...", cca_ip_.c_str());
  
  // First query device info
  query_cca_device_info();
  
  // Wait longer to ensure first HTTP connection is fully cleaned up and memory freed
  delay(1000);
  
  // Then query config and match devices
  query_cca_config();
}

std::string TigoMonitorComponent::get_barcode_for_node(const NodeTableData &node) {
  // Only use Frame 27 long address (16-char barcode)
  // Frame 09 barcodes are ignored to prevent duplicate entries
  if (!node.long_address.empty() && node.long_address.length() == 16) {
    return node.long_address;
  }
  return "";
}

#ifdef USE_ESP_IDF
void TigoMonitorComponent::query_cca_config() {
  // Check if network is available before attempting connection
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Network not connected, skipping CCA config query");
    return;
  }
  
  std::string url = "http://" + cca_ip_ + "/cgi-bin/summary_config";
  ESP_LOGI(TAG, "Querying CCA configuration from: %s", url.c_str());
  
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 5000;  // Reduced timeout to fail faster
  config.buffer_size = 4096;  // Larger buffer for chunked responses
  config.is_async = false;
  config.keep_alive_enable = false;  // Force connection close after request
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return;
  }
  
  // Set authorization header (Tigo:$olar)
  esp_http_client_set_header(client, "Authorization", "Basic VGlnbzokb2xhcg==");
  
  esp_err_t err = esp_http_client_open(client, 0);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return;
  }
  
  int content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  
  ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
  
  if (status_code == 200) {
    // Allocate read buffer from PSRAM if available (larger buffer = fewer reads)
    const size_t buffer_size = 4096;
    char* buffer = static_cast<char*>(psram_malloc(buffer_size));
    if (!buffer) {
      ESP_LOGE(TAG, "Failed to allocate HTTP read buffer");
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return;
    }
    
    // Read response in chunks (handles both known and chunked content-length)
    std::string response;
    response.reserve(content_length > 0 ? content_length : 8192);  // Pre-allocate
    int read_len;
    
    while ((read_len = esp_http_client_read(client, buffer, buffer_size)) > 0) {
      response.append(buffer, read_len);
    }
    
    heap_caps_free(buffer);  // Free PSRAM buffer
    
    if (read_len < 0) {
      ESP_LOGE(TAG, "Failed to read HTTP response");
    } else if (response.empty()) {
      ESP_LOGW(TAG, "CCA returned empty response");
    } else {
      ESP_LOGI(TAG, "Received %d bytes from CCA", response.length());
      ESP_LOGD(TAG, "CCA Response: %s", response.c_str());
      match_cca_to_uart(response);
    }
  } else {
    ESP_LOGW(TAG, "CCA returned status %d", status_code);
  }
  
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

void TigoMonitorComponent::match_cca_to_uart(const std::string &json_response) {
  // Set custom allocators for cJSON to use PSRAM
  cJSON_Hooks hooks;
  hooks.malloc_fn = cjson_malloc_psram;
  hooks.free_fn = cjson_free_psram;
  cJSON_InitHooks(&hooks);
  
  cJSON *root = cJSON_Parse(json_response.c_str());
  if (root == NULL) {
    ESP_LOGE(TAG, "Failed to parse CCA JSON response");
    // Reset to default allocators
    cJSON_InitHooks(NULL);
    return;
  }
  
  int matched_count = 0;
  int cca_panel_count = 0;
  
  // CCA returns array of objects with hierarchical structure
  // type: 2 = Panel, 3 = String, 4 = Inverter
  if (!cJSON_IsArray(root)) {
    ESP_LOGE(TAG, "CCA response is not an array");
    cJSON_Delete(root);
    return;
  }
  
  // Build a map of object_id -> object for quick lookup
  // Note: IDs are STRINGS in the CCA API, not numbers
  std::map<std::string, cJSON*> objects;
  int array_size = cJSON_GetArraySize(root);
  for (int i = 0; i < array_size; i++) {
    cJSON *obj = cJSON_GetArrayItem(root, i);
    cJSON *id = cJSON_GetObjectItem(obj, "id");
    if (id && cJSON_IsString(id)) {
      objects[id->valuestring] = obj;
    }
  }
  
  // Process all objects to find panels (type 2)
  for (int i = 0; i < array_size; i++) {
    cJSON *obj = cJSON_GetArrayItem(root, i);
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    
    if (type && cJSON_IsNumber(type) && type->valueint == 2) {
      // This is a panel
      cca_panel_count++;
      
      cJSON *serial = cJSON_GetObjectItem(obj, "serial");
      cJSON *label = cJSON_GetObjectItem(obj, "label");
      cJSON *channel = cJSON_GetObjectItem(obj, "channel");
      cJSON *object_id = cJSON_GetObjectItem(obj, "id");
      cJSON *parent_id = cJSON_GetObjectItem(obj, "parent");
      
      if (!serial || !cJSON_IsString(serial)) continue;
      
      std::string cca_serial = serial->valuestring;
      std::string cca_label_str = (label && cJSON_IsString(label)) ? label->valuestring : "";
      std::string cca_channel_str = (channel && cJSON_IsString(channel)) ? channel->valuestring : "";
      std::string cca_obj_id = (object_id && cJSON_IsString(object_id)) ? object_id->valuestring : "";
      
      // Find parent string and inverter
      std::string string_label = "";
      std::string inverter_label = "";
      
      if (parent_id && cJSON_IsString(parent_id)) {
        std::string parent = parent_id->valuestring;
        if (objects.count(parent) > 0) {
          cJSON *parent_obj = objects[parent];
          cJSON *parent_type = cJSON_GetObjectItem(parent_obj, "type");
          cJSON *parent_label = cJSON_GetObjectItem(parent_obj, "label");
          
          if (parent_type && cJSON_IsNumber(parent_type) && parent_type->valueint == 3) {
            // Parent is a string
            string_label = (parent_label && cJSON_IsString(parent_label)) ? parent_label->valuestring : "";
            
            // Find grandparent (inverter)
            cJSON *grandparent_id = cJSON_GetObjectItem(parent_obj, "parent");
            if (grandparent_id && cJSON_IsString(grandparent_id)) {
              std::string gp = grandparent_id->valuestring;
              if (objects.count(gp) > 0) {
                cJSON *gp_obj = objects[gp];
                cJSON *gp_label = cJSON_GetObjectItem(gp_obj, "label");
                inverter_label = (gp_label && cJSON_IsString(gp_label)) ? gp_label->valuestring : "";
              }
            }
          }
        }
      }
      
      // Try to match with UART-discovered nodes
      bool matched = false;
      for (auto &node : node_table_) {
        std::string uart_barcode = get_barcode_for_node(node);
        if (uart_barcode.empty()) continue;
        
        // Match: CCA serial should contain or equal UART barcode
        // (CCA might have longer format like "ABC123456-123")
        if (cca_serial.find(uart_barcode) != std::string::npos ||
            uart_barcode.find(cca_serial) != std::string::npos) {
          
          // Replace "Inverter" with "MPPT" for more accurate terminology
          if (inverter_label.find("Inverter ") == 0) {
            inverter_label.replace(0, 9, "MPPT ");
          }
          
          node.cca_label = cca_label_str;
          node.cca_string_label = string_label;
          node.cca_inverter_label = inverter_label;
          node.cca_channel = cca_channel_str;
          node.cca_object_id = cca_obj_id;
          node.cca_validated = true;
          
          ESP_LOGI(TAG, "Matched UART device %s (%s) with CCA panel '%s' (String: %s, MPPT: %s)",
                   node.addr.c_str(), uart_barcode.c_str(), cca_label_str.c_str(),
                   string_label.c_str(), inverter_label.c_str());
          
          matched = true;
          matched_count++;
          break;
        }
      }
      
      if (!matched) {
        ESP_LOGW(TAG, "CCA panel '%s' (serial: %s) not found in UART discovered devices",
                 cca_label_str.c_str(), cca_serial.c_str());
      }
    }
  }
  
  ESP_LOGI(TAG, "CCA Sync complete: %d CCA panels found, %d matched with UART devices",
           cca_panel_count, matched_count);
  
  // Save updated node table with CCA metadata
  if (matched_count > 0) {
    save_node_table();
    last_cca_sync_time_ = millis();
    
    // Rebuild string groups based on updated CCA data
    rebuild_string_groups();
  }
  
  cJSON_Delete(root);
  
  // Reset cJSON to default allocators
  cJSON_InitHooks(NULL);
}

void TigoMonitorComponent::query_cca_device_info() {
  // Check if network is available before attempting connection
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Network not connected, skipping CCA device info query");
    cca_device_info_ = "{\"error\":\"Network not connected\"}";
    return;
  }
  
  std::string url = "http://" + cca_ip_ + "/cgi-bin/mobile_api?cmd=DEVICE_INFO";
  ESP_LOGI(TAG, "Querying CCA device info from: %s", url.c_str());
  
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 5000;  // Reduced timeout to fail faster
  config.buffer_size = 2048;
  config.is_async = false;
  config.keep_alive_enable = false;  // Force connection close after request
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    cca_device_info_ = "{\"error\":\"Client init failed\"}";
    return;
  }
  
  // Set authorization header (Tigo:$olar)
  esp_http_client_set_header(client, "Authorization", "Basic VGlnbzokb2xhcg==");
  
  esp_err_t err = esp_http_client_open(client, 0);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection for device info: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    cca_device_info_ = "{\"error\":\"Failed to connect to CCA\"}";
    return;
  }
  
  int content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  
  ESP_LOGI(TAG, "CCA Device Info HTTP GET Status = %d, content_length = %d", status_code, content_length);
  
  if (status_code == 200) {
    // Allocate read buffer from PSRAM if available
    const size_t buffer_size = 2048;
    char* buffer = static_cast<char*>(psram_malloc(buffer_size));
    if (!buffer) {
      ESP_LOGE(TAG, "Failed to allocate HTTP read buffer");
      cca_device_info_ = "{\"error\":\"Memory allocation failed\"}";
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return;
    }
    
    // Read response in chunks
    std::string response;
    response.reserve(content_length > 0 ? content_length : 2048);  // Pre-allocate
    int read_len;
    
    while ((read_len = esp_http_client_read(client, buffer, buffer_size)) > 0) {
      response.append(buffer, read_len);
    }
    
    heap_caps_free(buffer);  // Free PSRAM buffer
    
    if (read_len < 0) {
      ESP_LOGE(TAG, "Failed to read CCA device info response");
      cca_device_info_ = "{\"error\":\"Failed to read response\"}";
    } else if (response.empty()) {
      ESP_LOGW(TAG, "CCA returned empty device info");
      cca_device_info_ = "{\"error\":\"Empty response\"}";
    } else {
      ESP_LOGI(TAG, "Received %d bytes of device info from CCA", response.length());
      ESP_LOGD(TAG, "CCA Device Info: %s", response.c_str());
      cca_device_info_ = response;
    }
  } else {
    ESP_LOGW(TAG, "CCA device info returned status %d", status_code);
    char error_buf[128];
    snprintf(error_buf, sizeof(error_buf), "{\"error\":\"HTTP %d\"}", status_code);
    cca_device_info_ = error_buf;
  }
  
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

#else
// Fallback if not using ESP-IDF
void TigoMonitorComponent::query_cca_config() {
  ESP_LOGW(TAG, "CCA sync requires ESP-IDF framework - feature not available");
}

void TigoMonitorComponent::query_cca_device_info() {
  ESP_LOGW(TAG, "CCA device info query requires ESP-IDF framework - feature not available");
  cca_device_info_ = "{\"error\":\"ESP-IDF required\"}";
}

void TigoMonitorComponent::match_cca_to_uart(const std::string &json_response) {
  // Not implemented for Arduino framework
}
#endif

// ========== Fast Display Helper Methods ==========

int TigoMonitorComponent::get_online_device_count() const {
  // Return cached value updated during publish_sensor_data()
  return cached_online_count_;
}

float TigoMonitorComponent::get_total_power() const {
  // Return cached value updated during publish_sensor_data()
  return cached_total_power_;
}

}  // namespace tigo_monitor
}  // namespace esphome