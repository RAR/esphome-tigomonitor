#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include <vector>
#include <map>
#include <set>
#include <string>

namespace esphome {
namespace tigo_server {

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
  bool changed = false;
  unsigned long last_update = 0;
};

struct NodeTableData {
  std::string long_address;    // Frame 27: 16-character long address
  std::string addr;            // 4-character short address
  std::string checksum;        // CRC checksum
  std::string frame09_barcode; // Frame 09: 6-character barcode
  int sensor_index = -1;       // ESPHome sensor index (-1 = unassigned)
  bool is_persistent = false;  // Whether this mapping should be saved to flash
};

class TigoServerComponent : public PollingComponent, public uart::UARTDevice {
 public:
  TigoServerComponent() = default;
  
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
  }
  void add_voltage_out_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->voltage_out_sensors_[address] = sensor; 
  }
  void add_current_in_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->current_in_sensors_[address] = sensor; 
  }
  void add_temperature_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->temperature_sensors_[address] = sensor; 
  }
  void add_power_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->power_sensors_[address] = sensor; 
  }
  void add_rssi_sensor(const std::string &address, sensor::Sensor *sensor) { 
    this->rssi_sensors_[address] = sensor; 
  }
  void add_tigo_sensor(const std::string &address, sensor::Sensor *sensor) {
    this->power_sensors_[address] = sensor;
  }



  // Configuration
  void set_number_of_devices(int count) { number_of_devices_ = count; }
  void set_auto_create_sensors(bool auto_create) { auto_create_sensors_ = auto_create; }
  
  // Generate YAML configuration for manual setup
  void generate_sensor_yaml();
  
  // Print current device mappings to logs
  void print_device_mappings();
  
  // Reset node table (clear all persistent device mappings)
  void reset_node_table();
  
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
  
 private:
  std::vector<DeviceData> devices_;
  std::vector<NodeTableData> node_table_;  // Unified table for all device info
  std::map<std::string, sensor::Sensor*> voltage_in_sensors_;
  std::map<std::string, sensor::Sensor*> voltage_out_sensors_;
  std::map<std::string, sensor::Sensor*> current_in_sensors_;
  std::map<std::string, sensor::Sensor*> temperature_sensors_;
  std::map<std::string, sensor::Sensor*> power_sensors_;
  std::map<std::string, sensor::Sensor*> rssi_sensors_;
  
  // Persistence
  static const uint32_t DEVICE_MAPPING_HASH = 0x12345678;  // Hash for preferences key
  
  std::string incoming_data_;
  bool frame_started_ = false;
  uint16_t crc_table_[CRC_TABLE_SIZE];
  int number_of_devices_ = 5;
  bool auto_create_sensors_ = true;
  std::set<std::string> created_devices_;
  
  // No timing variables needed - ESPHome handles update intervals
  
  // Character mapping for Tigo CRC
  static constexpr const char* crc_char_map_ = "GHJKLMNPRSTVWXYZ";
  static const uint8_t tigo_crc_table_[256];
};

#ifdef USE_BUTTON
class TigoYamlGeneratorButton : public button::Button, public Component {
 public:
  void set_tigo_server(TigoServerComponent *server) { this->tigo_server_ = server; }
  void press_action() override;
 protected:
  TigoServerComponent *tigo_server_;
};

class TigoDeviceMappingsButton : public button::Button, public Component {
 public:
  void set_tigo_server(TigoServerComponent *server) { this->tigo_server_ = server; }
  void press_action() override;
 protected:
  TigoServerComponent *tigo_server_;
};

class TigoResetNodeTableButton : public button::Button, public Component {
 public:
  void set_tigo_server(TigoServerComponent *server) { this->tigo_server_ = server; }
  void press_action() override;
 protected:
  TigoServerComponent *tigo_server_;
};
#endif

}  // namespace tigo_server
}  // namespace esphome