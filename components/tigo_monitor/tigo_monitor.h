#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include <vector>
#include <map>
#include <set>
#include <string>

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
  std::string long_address;    // Frame 27: 16-character long address (PRIMARY barcode source)
  std::string addr;            // 4-character short address
  std::string checksum;        // CRC checksum
  std::string frame09_barcode; // Frame 09: 6-character barcode (fallback when Frame 27 unavailable)
  int sensor_index = -1;       // ESPHome sensor index (-1 = unassigned)
  bool is_persistent = false;  // Whether this mapping should be saved to flash
  
  // CCA-sourced metadata (optional, populated via HTTP query)
  std::string cca_label;          // Friendly name from CCA (e.g., "East Roof Panel 3")
  std::string cca_string_label;   // Parent string label (e.g., "String 1")
  std::string cca_inverter_label; // Parent inverter label (e.g., "Inverter 1")
  std::string cca_channel;        // CCA channel identifier
  int cca_object_id = -1;         // CCA's internal object ID
  bool cca_validated = false;     // True if matched with CCA configuration
};


class TigoMonitorComponent : public PollingComponent, public uart::UARTDevice {
 public:
  TigoMonitorComponent() = default;
  
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
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

  // Configuration
  void set_number_of_devices(int count) { number_of_devices_ = count; }
  void set_cca_ip(const std::string &ip) { cca_ip_ = ip; }
  void set_sync_cca_on_startup(bool sync) { sync_cca_on_startup_ = sync; }
  
  // Public getters for web server access
  const std::vector<DeviceData>& get_devices() const { return devices_; }
  const std::vector<NodeTableData>& get_node_table() const { return node_table_; }
  int get_number_of_devices() const { return number_of_devices_; }
  const std::string& get_cca_ip() const { return cca_ip_; }
  bool get_sync_cca_on_startup() const { return sync_cca_on_startup_; }
  const std::string& get_cca_device_info() const { return cca_device_info_; }
  unsigned long get_last_cca_sync_time() const { return last_cca_sync_time_; }
  float get_total_energy_kwh() const { return total_energy_kwh_; }
  
  // Generate YAML configuration for manual setup
  void generate_sensor_yaml();
  
  // Print current device mappings to logs
  void print_device_mappings();
  
  // Reset node table (clear all persistent device mappings)
  void reset_node_table();
  
  // Remove individual node by address
  bool remove_node(uint16_t addr);
  
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
  void process_27_frame(const std::string &frame);
  int calculate_header_length(const std::string &hex_frame);
  
  // CRC functions
  void generate_crc_table();
  uint16_t compute_crc16_ccitt(const uint8_t *data, size_t length);
  char compute_tigo_crc4(const std::string &hex_string);
  
  // Device management
  void update_device_data(const DeviceData &data);
  void publish_sensor_data();
  DeviceData* find_device_by_addr(const std::string &addr);
  
  // Unified node table management (combines Frame 27, Frame 09, and device mappings)
  void load_node_table();
  void save_node_table();
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
  std::vector<DeviceData> devices_;
  std::vector<NodeTableData> node_table_;  // Unified table for all device info
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
  sensor::Sensor* power_sum_sensor_ = nullptr;
  sensor::Sensor* energy_sum_sensor_ = nullptr;
  sensor::Sensor* device_count_sensor_ = nullptr;
  
  // Energy calculation variables
  float total_energy_kwh_ = 0.0f;
  unsigned long last_energy_update_ = 0;
  
  // Night mode / no data handling
  unsigned long last_data_received_ = 0;
  unsigned long last_zero_publish_ = 0;
  static const unsigned long NO_DATA_TIMEOUT = 3600000;  // 1 hour in milliseconds
  static const unsigned long ZERO_PUBLISH_INTERVAL = 600000;  // 10 minutes in milliseconds
  bool in_night_mode_ = false;
  
  // Persistence
  static const uint32_t DEVICE_MAPPING_HASH = 0x12345678;  // Hash for preferences key
  static const uint32_t ENERGY_DATA_HASH = 0x87654321;     // Hash for energy data key
  
  std::string incoming_data_;
  bool frame_started_ = false;
  uint16_t crc_table_[CRC_TABLE_SIZE];
  int number_of_devices_ = 5;
  std::set<std::string> created_devices_;
  std::string cca_ip_;  // Optional CCA IP address for HTTP queries
  bool sync_cca_on_startup_ = true;  // Whether to sync from CCA on boot (default: true)
  std::string cca_device_info_;  // Cached CCA device info JSON
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