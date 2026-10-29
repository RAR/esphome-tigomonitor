#pragma once

// The ESP32 as the TAP's bus master, in the CCA's place, or standing by
// beside another master to take the bus over when that one stops
// (docs/superpowers/specs/2026-10-29-esp32-bus-master-design.md).
//
// The logic is the Pi's (master_core.h). It runs in its own FreeRTOS task on
// core 1, away from Wi-Fi, and is the only thing that touches the RS485 UART.
// Everything it reads is handed on through this component's own UART side,
// which tigo_monitor reads as if it were the bus: decoding and publishing
// stay in the ESPHome loop, out of the master's way.
//
// This component is the only write path to the bus in the project. Configs
// without it compile no UART write at all (see the board files' read-only
// notes).

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "esphome/components/tigo_monitor/tigo_monitor.h"

#include "master_core.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace tigo_master {

// The bus UART, from the master task only. A frame is written whole and
// drained before the reply is read; the IDF's RS485 half-duplex mode (the
// uart's flow_control_pin) drops DE as the last bit leaves.
class UartTransport : public Transport {
 public:
  explicit UartTransport(uart::UARTComponent *uart) : uart_(uart) {}
  bool configured() const override { return uart_ != nullptr; }
  bool open(std::string &) override {
    open_ = true;
    return true;
  }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }
  bool write(const uint8_t *data, size_t n, std::string &) override {
    uart_->write_array(data, n);
    return true;
  }
  void drain() override { uart_->flush(); }
  int read(char *buf, size_t n, int timeout_ms) override;
  int baud() const override { return static_cast<int>(uart_->get_baud_rate()); }
  std::string name() const override { return "the RS485 UART"; }

 private:
  uart::UARTComponent *uart_;
  bool open_ = false;
};

// One writer (the master task), one reader (the ESPHome loop), no locks and
// no allocation. Bytes that don't fit are dropped and counted.
class ByteRing {
 public:
  static const size_t SIZE = 8192;
  void push(const char *data, size_t n);
  size_t available() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed); }
  bool peek(uint8_t *b) const;
  size_t pop(uint8_t *out, size_t n);
  uint32_t dropped() const { return dropped_; }

 private:
  uint8_t buf_[SIZE];
  std::atomic<size_t> head_{0}, tail_{0};
  std::atomic<uint32_t> dropped_{0};
};

class TigoMasterComponent : public Component, public uart::UARTComponent {
 public:
  void set_bus(uart::UARTComponent *bus) { bus_ = bus; }
  void set_monitor(tigo_monitor::TigoMonitorComponent *m) { monitor_ = m; }
  void set_standby(uint32_t takeover_s) {
    standby_ = true;
    takeover_s_ = takeover_s;
  }
  void set_gateway_id(uint16_t id) { gateway_id_ = id; }
  void set_startup(bool on) { startup_ = on; }
  void set_housekeeping(bool on) { housekeeping_ = on; }
  void set_queries(bool on, bool writes) {
    queries_ = on;
    writes_ = on && writes;
  }
#ifdef USE_TEXT_SENSOR
  void set_status_sensor(text_sensor::TextSensor *s) { status_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_problem_sensor(binary_sensor::BinarySensor *s) { problem_sensor_ = s; }
#endif
#ifdef USE_SENSOR
  void set_lease_sensor(sensor::Sensor *s) { lease_sensor_ = s; }
  void set_takeovers_sensor(sensor::Sensor *s) { takeovers_sensor_ = s; }
  void set_pause_sensor(sensor::Sensor *s) { pause_sensor_ = s; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before tigo_monitor, which reads this component's UART side in its loop.
  float get_setup_priority() const override { return setup_priority::BUS; }

  MasterStatus status() const { return core_ ? core_->status() : MasterStatus{}; }

  // The UART side tigo_monitor reads: what the master read off the bus.
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override { return ring_.peek(data); }
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override { return ring_.available(); }
  uart::UARTFlushResult flush() override { return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS; }
  void load_settings(bool) override {}

  // The ESPHome loop must have run this recently for the master to send
  // anything: a device whose loop hangs goes quiet, and the lease lapses.
  static const uint32_t LOOP_DEAD_MS = 5000;
  static const uint32_t TICK_EVERY_MS = 10;  // as the Pi's main loop
  static const uint32_t PUBLISH_EVERY_MS = 5000;
  static const uint32_t TARGETS_EVERY_MS = 60000;

 protected:
  void check_logger_conflict() override {}

 private:
  static void task_(void *arg);
  void run_();
  void on_bytes_(const char *data, size_t n);
  void on_listen_(const char *data, size_t n);
  void refresh_targets_();
  void publish_();

  uart::UARTComponent *bus_ = nullptr;
  tigo_monitor::TigoMonitorComponent *monitor_ = nullptr;
  bool standby_ = false;
  uint32_t takeover_s_ = 8;
  uint16_t gateway_id_ = 0x1201;
  bool startup_ = false, housekeeping_ = false, queries_ = false, writes_ = false;

  std::unique_ptr<UartTransport> port_;
  std::unique_ptr<MasterCore> core_;
  TaskHandle_t task_handle_ = nullptr;
  std::atomic<uint32_t> loop_alive_ms_{0};
  ByteRing ring_;

  // The 0x41s another master sends, learned while standing by. In RAM only:
  // a reboot falls back to the built-in content until it hears them again.
  esphome::tigo_monitor::master::Learned41 learned_;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> learn_splitter_;

  // The optimizers heard in the last day, for the 0x41s. The loop fills it,
  // the master task reads it.
  std::mutex targets_mu_;
  std::vector<uint16_t> targets_;
  uint32_t targets_at_ms_ = 0;
  uint32_t published_at_ms_ = 0;
  bool write_refused_logged_ = false;
  bool loop_dead_logged_ = false;

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_sensor_ = nullptr;
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *problem_sensor_ = nullptr;
#endif
#ifdef USE_SENSOR
  sensor::Sensor *lease_sensor_ = nullptr;
  sensor::Sensor *takeovers_sensor_ = nullptr;
  sensor::Sensor *pause_sensor_ = nullptr;
#endif
};

}  // namespace tigo_master
}  // namespace esphome
