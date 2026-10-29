#include "tigo_master_component.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdlib>

namespace esphome {
namespace tigo_master {

static const char *const TAG = "tigo_master";

int UartTransport::read(char *buf, size_t n, int timeout_ms) {
  uint32_t start = millis();
  for (;;) {
    size_t avail = uart_->available();
    if (avail > 0) {
      size_t k = std::min(avail, n);
      return uart_->read_array(reinterpret_cast<uint8_t *>(buf), k) ? static_cast<int>(k) : -1;
    }
    if (static_cast<int32_t>(millis() - start) >= timeout_ms) return 0;
    vTaskDelay(1);
  }
}

void ByteRing::push(const char *data, size_t n) {
  size_t head = head_.load(std::memory_order_relaxed);
  size_t room = SIZE - (head - tail_.load(std::memory_order_acquire));
  if (n > room) {
    dropped_ += n - room;
    n = room;
  }
  for (size_t i = 0; i < n; i++) buf_[(head + i) % SIZE] = static_cast<uint8_t>(data[i]);
  head_.store(head + n, std::memory_order_release);
}

bool ByteRing::peek(uint8_t *b) const {
  if (available() == 0) return false;
  *b = buf_[tail_.load(std::memory_order_relaxed) % SIZE];
  return true;
}

size_t ByteRing::pop(uint8_t *out, size_t n) {
  size_t tail = tail_.load(std::memory_order_relaxed);
  n = std::min(n, available());
  for (size_t i = 0; i < n; i++) out[i] = buf_[(tail + i) % SIZE];
  tail_.store(tail + n, std::memory_order_release);
  return n;
}

void TigoMasterComponent::setup() {
  loop_alive_ms_ = millis();
  port_ = std::make_unique<UartTransport>(bus_);
  auto log = [](int prio, const std::string &msg) {
    if (prio <= 3) ESP_LOGE(TAG, "%s", msg.c_str());
    else if (prio == 4) ESP_LOGW(TAG, "%s", msg.c_str());
    else ESP_LOGI(TAG, "%s", msg.c_str());
  };
  core_ = std::make_unique<MasterCore>(
      port_.get(), log, [this](const char *d, size_t n) { on_bytes_(d, n); }, nullptr, gateway_id_,
      /*bench=*/false, startup_, housekeeping_, std::vector<tigo_monitor::master::Learned41::Entry>{},
      [this] {
        std::lock_guard<std::mutex> lock(targets_mu_);
        return targets_;
      },
      queries_, writes_);
  if (standby_)
    core_->set_standby(takeover_s_, [this](const char *d, size_t n) { on_listen_(d, n); },
                       [this] { return learned_.entries(); });
  core_->open();
  // Core 1: Wi-Fi and the network stack live on core 0. Above the ESPHome
  // loop task (priority 1), so publishing never delays a reply.
  if (xTaskCreatePinnedToCore(task_, "tigo_master", 8192, this, 5, &task_handle_, 1) != pdPASS) {
    ESP_LOGE(TAG, "could not start the bus master task");
    mark_failed();
  }
}

void TigoMasterComponent::task_(void *arg) { static_cast<TigoMasterComponent *>(arg)->run_(); }

// One request and its reply per tick, as the Pi's main loop. The core sends
// nothing unless ticked, and it is ticked only while the ESPHome loop runs.
void TigoMasterComponent::run_() {
  for (;;) {
    uint32_t now = millis();
    if (now - loop_alive_ms_.load() > LOOP_DEAD_MS) {
      if (!loop_dead_logged_) {
        ESP_LOGE(TAG, "the ESPHome loop has not run for %u ms; sending nothing until it does",
                 static_cast<unsigned>(LOOP_DEAD_MS));
        loop_dead_logged_ = true;
      }
      vTaskDelay(pdMS_TO_TICKS(TICK_EVERY_MS));
      continue;
    }
    if (loop_dead_logged_) {
      ESP_LOGW(TAG, "the ESPHome loop is running again");
      loop_dead_logged_ = false;
    }
    core_->tick(now);
    vTaskDelay(pdMS_TO_TICKS(TICK_EVERY_MS));
  }
}

void TigoMasterComponent::on_bytes_(const char *data, size_t n) { ring_.push(data, n); }

// Standing by: what another master says goes to tigo_monitor as usual, and
// its 0x41s are learned for when this one takes over.
void TigoMasterComponent::on_listen_(const char *data, size_t n) {
  ring_.push(data, n);
  for (size_t i = 0; i < n; i++) {
    learn_splitter_.feed(data[i], [this](const std::string &body) {
      tigo_monitor::master::Frame f;
      if (tigo_monitor::master::decode_frame(body, f)) learned_.on_frame(f);
    });
  }
}

void TigoMasterComponent::loop() {
  uint32_t now = millis();
  loop_alive_ms_ = now;
  if (monitor_ != nullptr && (targets_at_ms_ == 0 || now - targets_at_ms_ >= TARGETS_EVERY_MS)) {
    targets_at_ms_ = now;
    refresh_targets_();
  }
  if (now - published_at_ms_ >= PUBLISH_EVERY_MS) {
    published_at_ms_ = now;
    publish_();
  }
}

// The optimizers heard in the last day, so one removed from the roof drops
// out by itself, as on the Pi.
void TigoMasterComponent::refresh_targets_() {
  std::vector<uint16_t> t;
  uint32_t now = millis();
  for (const auto &d : monitor_->get_devices()) {
    if (d.last_update == 0 || now - d.last_update > 86400000UL) continue;
    t.push_back(static_cast<uint16_t>(strtoul(d.addr.c_str(), nullptr, 16)));
  }
  std::lock_guard<std::mutex> lock(targets_mu_);
  targets_ = std::move(t);
}

void TigoMasterComponent::publish_() {
  MasterStatus m = status();
  bool standing_by = m.standby == Standby::standing_by || m.standby == Standby::taking_over;
  const char *s = m.fault != Fault::none ? fault_name(m.fault)
                  : !m.active            ? "stopped"
                  : standing_by          ? standby_name(m.standby)
                                         : "active";
  bool problem = m.fault != Fault::none || !m.active;
#ifdef USE_TEXT_SENSOR
  if (status_sensor_ != nullptr && status_sensor_->get_state() != s) status_sensor_->publish_state(s);
#endif
#ifdef USE_BINARY_SENSOR
  if (problem_sensor_ != nullptr) problem_sensor_->publish_state(problem);
#endif
#ifdef USE_SENSOR
  if (lease_sensor_ != nullptr && m.lease_read && !standing_by) lease_sensor_->publish_state(m.lease.remaining_s);
  if (takeovers_sensor_ != nullptr) takeovers_sensor_->publish_state(m.takeovers);
  if (pause_sensor_ != nullptr) pause_sensor_->publish_state(m.gap_recent_ms);
#endif
}

// tigo_monitor only ever reads. Nothing else may reach the bus this way:
// requests go out from the master task alone.
void TigoMasterComponent::write_array(const uint8_t *, size_t len) {
  if (!write_refused_logged_) {
    ESP_LOGE(TAG, "refused a write of %u bytes through tigo_master's UART: only the bus master sends", (unsigned) len);
    write_refused_logged_ = true;
  }
}

bool TigoMasterComponent::read_array(uint8_t *data, size_t len) {
  if (ring_.available() < len) return false;
  ring_.pop(data, len);
  return true;
}

void TigoMasterComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Tigo bus master:");
  ESP_LOGCONFIG(TAG, "  Role: %s", standby_ ? "standby" : "master");
  if (standby_) {
    ESP_LOGCONFIG(TAG, "  Takes over after: %u s", static_cast<unsigned>(takeover_s_));
  }
  ESP_LOGCONFIG(TAG, "  Gateway id: %04X", gateway_id_);
  ESP_LOGCONFIG(TAG, "  Bring-up: %s, housekeeping: %s, queries: %s%s", YESNO(startup_), YESNO(housekeeping_),
                YESNO(queries_), writes_ ? " (with the CCA's two writes)" : "");
}

}  // namespace tigo_master
}  // namespace esphome
