#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include <vector>
#include <map>
#include <set>
#include <string>
#include <limits>
#include <new>

#ifdef USE_ESP_IDF
#include <esp_heap_caps.h>

// Forward declare PSRAM allocation functions
namespace esphome {
namespace tigo_monitor {
  void* psram_malloc_impl(size_t size);
  void psram_free_impl(void* ptr);
}
}

// STL-compatible allocator for PSRAM usage
template<typename T>
class PSRAMAllocator {
public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  template<typename U>
  struct rebind {
    using other = PSRAMAllocator<U>;
  };

  PSRAMAllocator() noexcept = default;
  template<typename U>
  PSRAMAllocator(const PSRAMAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
      return nullptr;  // Can't throw - exceptions disabled
    
    void* ptr = esphome::tigo_monitor::psram_malloc_impl(n * sizeof(T));
    return static_cast<T*>(ptr);
  }

  void deallocate(T* p, std::size_t) noexcept {
    esphome::tigo_monitor::psram_free_impl(p);
  }

  template<typename U>
  bool operator==(const PSRAMAllocator<U>&) const noexcept { return true; }
  
  template<typename U>
  bool operator!=(const PSRAMAllocator<U>&) const noexcept { return false; }
};

// Type aliases for PSRAM-backed containers
template<typename T>
using psram_vector = std::vector<T, PSRAMAllocator<T>>;

template<typename Key, typename T>
using psram_map = std::map<Key, T, std::less<Key>, PSRAMAllocator<std::pair<const Key, T>>>;

template<typename T>
using psram_set = std::set<T, std::less<T>, PSRAMAllocator<T>>;

// PSRAM-backed string using std::basic_string with PSRAMAllocator
using psram_string = std::basic_string<char, std::char_traits<char>, PSRAMAllocator<char>>;
#endif

namespace esphome {
namespace tigo_monitor {

static const uint16_t CRC_POLYNOMIAL = 0x8408;  // Reversed polynomial (0x1021 reflected)
static const size_t CRC_TABLE_SIZE = 256;

struct DeviceData {
  std::string pv_node_id;
  std::string addr;
  float voltage_in;
  float voltage_out;
  uint8_t duty_cycle;
  float current_in;
  float temperature;
  std::string slot_counter;
  int rssi;
  std::string barcode;
  std::string firmware_version;
  float efficiency;
  float power_factor;
  float load_factor;
  bool changed = false;
  unsigned long last_update = 0;
  float peak_power = 0.0f;  // Historical peak power (watts)
};

struct NodeTableData {
  std::string long_address;    // Frame 27: 16-character long address (PRIMARY and ONLY barcode source)
  std::string addr;            // 4-character short address
  std::string checksum;        // CRC checksum
  int sensor_index = -1;       // ESPHome sensor index (-1 = unassigned)
  bool is_persistent = false;  // Whether this mapping should be saved to flash
  
  // CCA-sourced metadata (optional, populated via HTTP query)
  std::string cca_label;          // Friendly name from CCA (e.g., "East Roof Panel 3")
  std::string cca_string_label;   // Parent string label (e.g., "String 1")
  std::string cca_inverter_label; // Parent MPPT label (e.g., "MPPT 1" - CCA calls it "Inverter")
  std::string cca_channel;        // CCA channel identifier
  std::string cca_object_id;      // CCA's internal object ID (string type)
  bool cca_validated = false;     // True if matched with CCA configuration
};

struct StringData {
  std::string string_label;       // String name from CCA (e.g., "String 1")
  std::string inverter_label;     // Parent MPPT name (called "Inverter" in CCA)
  std::vector<std::string> device_addrs; // Device addresses in this string
  float total_power = 0.0f;
  float total_current = 0.0f;
  float avg_voltage_in = 0.0f;
  float avg_voltage_out = 0.0f;
  float avg_temperature = 0.0f;
  float avg_efficiency = 0.0f;
  float min_efficiency = 100.0f;
  float max_efficiency = 0.0f;
  float peak_power = 0.0f;        // Historical peak power for this string
  int active_device_count = 0;
  int total_device_count = 0;
  unsigned long last_update = 0;
};

struct InverterData {
  std::string name;               // User-defined inverter name
  std::vector<std::string> mppt_labels;  // List of MPPT labels assigned to this inverter
  float total_power = 0.0f;
  float peak_power = 0.0f;
  float total_energy = 0.0f;
  int active_device_count = 0;
  int total_device_count = 0;
};


class TigoMonitorComponent : public PollingComponent, public uart::UARTDevice {
 public:
  TigoMonitorComponent() = default;
  
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override;

  // Device name helper
  std::string get_device_name(const DeviceData &device);
  
  // Manual sensor registration (for advanced users who want specific configurations)
  void add_voltage_in_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->voltage_in_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered voltage_in sensor for address: %s", address.c_str());
  }
  void add_voltage_out_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->voltage_out_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered voltage_out sensor for address: %s", address.c_str());
  }
  void add_current_in_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->current_in_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered current_in sensor for address: %s", address.c_str());
  }
  void add_temperature_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->temperature_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered temperature sensor for address: %s", address.c_str());
  }
  void add_power_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->power_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered power sensor for address: %s", address.c_str());
  }
  void add_peak_power_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->peak_power_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered peak_power sensor for address: %s", address.c_str());
  }
  void add_power_sum_sensor(sensor::Sensor *sensor) { 
    this->power_sum_sensor_ = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered power sum sensor");
  }
  void add_energy_sum_sensor(sensor::Sensor *sensor) { 
    this->energy_sum_sensor_ = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered energy sum sensor");
  }
  void add_device_count_sensor(sensor::Sensor *sensor) { 
    this->device_count_sensor_ = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered device count sensor");
  }
  void add_invalid_checksum_sensor(sensor::Sensor *sensor) {
    this->invalid_checksum_sensor_ = sensor;
    ESP_LOGCONFIG("tigo_monitor", "Registered invalid checksum sensor");
  }
  void add_missed_packet_sensor(sensor::Sensor *sensor) {
    this->missed_packet_sensor_ = sensor;
    ESP_LOGCONFIG("tigo_monitor", "Registered missed packet sensor");
  }
  void add_rssi_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->rssi_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered rssi sensor for address: %s", address.c_str());
  }
  void add_barcode_sensor(const std::string &address, text_sensor::TextSensor *sensor) { 
    this->barcode_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered barcode sensor for address: %s", address.c_str());
  }
  void add_duty_cycle_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->duty_cycle_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered duty_cycle sensor for address: %s", address.c_str());
  }
  void add_firmware_version_sensor(const std::string &address, text_sensor::TextSensor *sensor) { 
    this->firmware_version_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered firmware_version sensor for address: %s", address.c_str());
  }
  void add_efficiency_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->efficiency_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered efficiency sensor for address: %s", address.c_str());
  }
  void add_power_factor_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->power_factor_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered power_factor sensor for address: %s", address.c_str());
  }
  void add_load_factor_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->load_factor_sensors_[address] = sensor; 
    ESP_LOGCONFIG("tigo_monitor", "Registered load_factor sensor for address: %s", address.c_str());
  }
  void add_tigo_sensor(const std::string &address, sensor::Sensor *sensor) {
    this->power_sensors_[address] = sensor;
  }
  void add_night_mode_sensor(binary_sensor::BinarySensor *sensor) {
    this->night_mode_sensor_ = sensor;
    ESP_LOGCONFIG("tigo_monitor", "Registered night mode binary sensor");
  }

  // Configuration
  void set_number_of_devices(int count) { number_of_devices_ = count; }
  void set_cca_ip(const std::string &ip) { cca_ip_ = ip; }
  void set_sync_cca_on_startup(bool sync) { sync_cca_on_startup_ = sync; }
  void add_inverter(const std::string &name, const std::vector<std::string> &mppt_labels);
  
  // Public getters for web server access
#ifdef USE_ESP_IDF
  const psram_vector<DeviceData>& get_devices() const { return devices_; }
  const psram_vector<NodeTableData>& get_node_table() const { return node_table_; }
  const psram_map<std::string, StringData>& get_strings() const { return strings_; }
  const psram_vector<InverterData>& get_inverters() const { return inverters_; }
#else
  const std::vector<DeviceData>& get_devices() const { return devices_; }
  const std::vector<NodeTableData>& get_node_table() const { return node_table_; }
  const std::map<std::string, StringData>& get_strings() const { return strings_; }
  const std::vector<InverterData>& get_inverters() const { return inverters_; }
#endif
  int get_number_of_devices() const { return number_of_devices_; }
  const std::string& get_cca_ip() const { return cca_ip_; }
  bool get_sync_cca_on_startup() const { return sync_cca_on_startup_; }
#ifdef USE_ESP_IDF
  std::string get_cca_device_info() const { return std::string(cca_device_info_.begin(), cca_device_info_.end()); }
#else
  const std::string& get_cca_device_info() const { return cca_device_info_; }
#endif
  unsigned long get_last_cca_sync_time() const { return last_cca_sync_time_; }
  float get_total_energy_kwh() const { return total_energy_kwh_; }
  uint32_t get_invalid_checksum_count() const { return invalid_checksum_count_; }
  uint32_t get_missed_packet_count() const { return missed_packet_count_; }
  
  // Public methods for web server access
  void reset_peak_power();  // Reset all peak power values to 0
  void reset_total_energy();  // Reset total energy to 0
  void check_midnight_reset();  // Check if midnight has passed and reset configured sensors
  void set_reset_at_midnight(bool reset) { this->reset_at_midnight_ = reset; }
  
#ifdef USE_TIME
  void set_time_id(time::RealTimeClock *time_id) { this->time_id_ = time_id; }
#endif
  
  // Generate YAML configuration for manual setup
  void generate_sensor_yaml();
  
  // Print current device mappings to logs
  void print_device_mappings();
  
  // Reset node table (clear all persistent device mappings)
  void reset_node_table();
  
  // Remove individual node by address
  bool remove_node(uint16_t addr);
  
  // Import node table from JSON data
  bool import_node_table(const std::vector<NodeTableData>& nodes);
  
  // CCA synchronization (called by button or on boot)
  void sync_from_cca();
  
  // CCA refresh (called by web server refresh button - does both device info + config sync)
  void refresh_cca_data();
  
  // CCA device info query (called by web server)
  void query_cca_device_info();
  
 protected:
  // Frame processing
  void process_serial_data();
  void process_frame(const std::string &frame);
  std::string remove_escape_sequences(const std::string &frame);
  bool verify_checksum(const std::string &frame);
  std::string frame_to_hex_string(const std::string &frame);
  
  // Frame type handlers
  void process_power_frame(const std::string &frame);
  void process_09_frame(const std::string &frame);
  void process_27_frame(const std::string &hex_frame, size_t offset);
  int calculate_header_length(const std::string &hex_frame);
  
  // CRC functions
  void generate_crc_table();
  uint16_t compute_crc16_ccitt(const uint8_t *data, size_t length);
  char compute_tigo_crc4(const std::string &hex_string);
  
  // Device management
  void update_device_data(const DeviceData &data);
  void publish_sensor_data();
  DeviceData* find_device_by_addr(const std::string &addr);
  
  // String-level aggregation
  void update_string_data();
  void rebuild_string_groups();
  
  // Inverter-level aggregation
  void update_inverter_data();
  StringData* find_string_by_label(const std::string &label);
  
  // Unified node table management (combines Frame 27, Frame 09, and device mappings)
  void load_node_table();
  void save_node_table();
  void save_peak_power_data();
  void load_peak_power_data();
  void save_persistent_data();  // Save all persistent data (node table + peak power + energy)
  int get_next_available_sensor_index();
  NodeTableData* find_node_by_addr(const std::string &addr);
  void assign_sensor_index_to_node(const std::string &addr);
  
  // CCA HTTP query and matching
  void query_cca_config();
  void match_cca_to_uart(const std::string &json_response);
  std::string get_barcode_for_node(const NodeTableData &node);
  
  // Energy persistence
  void load_energy_data();
  void save_energy_data();
  
 private:
#ifdef USE_ESP_IDF
  // Use PSRAM-backed containers for large data structures
  psram_vector<DeviceData> devices_;
  psram_vector<NodeTableData> node_table_;  // Unified table for all device info
  psram_map<std::string, StringData> strings_;  // String-level aggregation (key = string_label)
  psram_vector<InverterData> inverters_;  // User-defined inverter groupings
  
  // Sensor maps stored in PSRAM to save internal RAM (saves ~6-10KB depending on config)
  psram_map<std::string, sensor::Sensor*> voltage_in_sensors_;
  psram_map<std::string, sensor::Sensor*> voltage_out_sensors_;
  psram_map<std::string, sensor::Sensor*> current_in_sensors_;
  psram_map<std::string, sensor::Sensor*> temperature_sensors_;
  psram_map<std::string, sensor::Sensor*> power_sensors_;
  psram_map<std::string, sensor::Sensor*> peak_power_sensors_;
  psram_map<std::string, sensor::Sensor*> rssi_sensors_;
  psram_map<std::string, text_sensor::TextSensor*> barcode_sensors_;
  psram_map<std::string, sensor::Sensor*> duty_cycle_sensors_;
  psram_map<std::string, text_sensor::TextSensor*> firmware_version_sensors_;
  psram_map<std::string, sensor::Sensor*> efficiency_sensors_;
  psram_map<std::string, sensor::Sensor*> power_factor_sensors_;
  psram_map<std::string, sensor::Sensor*> load_factor_sensors_;
#else
  // Fallback to standard containers on Arduino
  std::vector<DeviceData> devices_;
  std::vector<NodeTableData> node_table_;
  std::map<std::string, StringData> strings_;
  std::vector<InverterData> inverters_;
  
  std::map<std::string, sensor::Sensor*> voltage_in_sensors_;
  std::map<std::string, sensor::Sensor*> voltage_out_sensors_;
  std::map<std::string, sensor::Sensor*> current_in_sensors_;
  std::map<std::string, sensor::Sensor*> temperature_sensors_;
  std::map<std::string, sensor::Sensor*> power_sensors_;
  std::map<std::string, sensor::Sensor*> peak_power_sensors_;
  std::map<std::string, sensor::Sensor*> rssi_sensors_;
  std::map<std::string, text_sensor::TextSensor*> barcode_sensors_;
  std::map<std::string, sensor::Sensor*> duty_cycle_sensors_;
  std::map<std::string, text_sensor::TextSensor*> firmware_version_sensors_;
  std::map<std::string, sensor::Sensor*> efficiency_sensors_;
  std::map<std::string, sensor::Sensor*> power_factor_sensors_;
  std::map<std::string, sensor::Sensor*> load_factor_sensors_;
#endif
  sensor::Sensor* power_sum_sensor_ = nullptr;
  sensor::Sensor* energy_sum_sensor_ = nullptr;
  sensor::Sensor* device_count_sensor_ = nullptr;
  sensor::Sensor* invalid_checksum_sensor_ = nullptr;
  sensor::Sensor* missed_packet_sensor_ = nullptr;
  binary_sensor::BinarySensor* night_mode_sensor_ = nullptr;
  
  // Energy calculation variables
  float total_energy_kwh_ = 0.0f;
  unsigned long last_energy_update_ = 0;
  
  // UART diagnostics
  uint32_t invalid_checksum_count_ = 0;
  uint32_t missed_packet_count_ = 0;
  
  // Midnight reset tracking
  bool reset_at_midnight_ = false;  // Global flag to reset peak power and energy at midnight
  int last_reset_day_ = -1;  // Track which day we last reset (day of year)
#ifdef USE_TIME
  time::RealTimeClock *time_id_{nullptr};
#endif
  
  // Night mode / no data handling
  unsigned long last_data_received_ = 0;
  unsigned long last_zero_publish_ = 0;
  static const unsigned long NO_DATA_TIMEOUT = 3600000;  // 1 hour in milliseconds
  static const unsigned long ZERO_PUBLISH_INTERVAL = 600000;  // 10 minutes in milliseconds
  bool in_night_mode_ = false;
  
  // Persistence
  static const uint32_t DEVICE_MAPPING_HASH = 0x12345678;  // Hash for preferences key
  static const uint32_t ENERGY_DATA_HASH = 0x87654321;     // Hash for energy data key
  
#ifdef USE_ESP_IDF
  // Use PSRAM-backed buffer for incoming serial data (can grow to 16KB)
  psram_vector<char> incoming_data_;
  
  // Move large/growing data structures to PSRAM to save internal RAM
  psram_set<std::string> created_devices_;        // Device creation tracker (~4-8 bytes per device)
  psram_string cca_device_info_;                  // Cached CCA device info JSON (can be several KB)
#else
  std::string incoming_data_;
  std::set<std::string> created_devices_;
  std::string cca_device_info_;
#endif
  bool frame_started_ = false;
  uint16_t crc_table_[CRC_TABLE_SIZE];
  int number_of_devices_ = 5;
  std::string cca_ip_;  // Optional CCA IP address for HTTP queries (small, kept in internal RAM)
  bool sync_cca_on_startup_ = true;  // Whether to sync from CCA on boot (default: true)
  unsigned long last_cca_sync_time_ = 0;  // millis() of last successful CCA sync
  
  // No timing variables needed - ESPHome handles update intervals
  
  // Character mapping for Tigo CRC
  static constexpr const char* crc_char_map_ = "GHJKLMNPRSTVWXYZ";
  static const uint8_t tigo_crc_table_[256];
};

#ifdef USE_BUTTON
class TigoYamlGeneratorButton : public button::Button, public Component {
 public:
  void set_tigo_monitor(TigoMonitorComponent *server) { this->tigo_monitor_ = server; }
  void press_action() override;
 protected:
  TigoMonitorComponent *tigo_monitor_;
};

class TigoDeviceMappingsButton : public button::Button, public Component {
 public:
  void set_tigo_monitor(TigoMonitorComponent *server) { this->tigo_monitor_ = server; }
  void press_action() override;
 protected:
  TigoMonitorComponent *tigo_monitor_;
};

class TigoResetNodeTableButton : public button::Button, public Component {
 public:
  void set_tigo_monitor(TigoMonitorComponent *server) { this->tigo_monitor_ = server; }
  void press_action() override;
 protected:
  TigoMonitorComponent *tigo_monitor_;
};

class TigoSyncFromCCAButton : public button::Button, public Component {
 public:
  void set_tigo_monitor(TigoMonitorComponent *server) { this->tigo_monitor_ = server; }
  void press_action() override;
 protected:
  TigoMonitorComponent *tigo_monitor_;
};
#endif

}  // namespace tigo_monitor
}  // namespace esphome