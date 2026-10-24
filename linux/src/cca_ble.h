#pragma once

// The CCA over Bluetooth, through BlueZ on D-Bus: the Pi's side of the ESP32's
// tigo_server/tigo_cca_ble.cpp, for the CCA Info page. CCA firmware 4.0.4+
// locks its local HTTP API, and BLE is what the Tigo app uses instead.
//
// Protocol (verified against the Tigo app 5.4.6; see the firmware file):
//   request  = [reqNum] "cmd=X[ params]" '\0', one write-with-response
//   response = notifications; byte 0 reqNum, body from byte 3, ends with '\0'
//   session  = sid = SHA-512 hex of (uts + 159260), uts from DEVICE_INFO
//
// One job at a time on a worker thread, each a one-shot connection: connect,
// DEVICE_INFO (for the sid), the command, disconnect — so the CCA is free for
// the Tigo app in between. HTTP handlers only queue jobs and read the cache.

#include <systemd/sd-bus.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


namespace tigod {

class CcaBle {
 public:
  explicit CcaBle(std::string state_path);
  ~CcaBle();
  void start();
  void stop();

  // Queue work. Each returns at once; results land in the cache.
  void request_refresh();  // DEVICE_INFO + NETWORK_INFO
  void request_command(const std::string &cmd, const std::string &params = "");
  void request_scan();
  // Looks for the adapter again: bluetoothd may have come up after us.
  void request_probe();

  struct ScanHit {
    std::string mac, name;
    int rssi = 0;
    int64_t seen = 0;  // unix seconds
  };
  struct View {
    bool adapter = false;  // false: this machine has no usable Bluetooth
    std::string active_mac, saved_mac, error;
    bool busy = false;
    std::string device_info, network_info;  // raw JSON from the CCA
    int64_t info_epoch = 0, info_failed_epoch = 0;
    std::string discovery;
    int64_t discovery_epoch = 0;
    std::vector<ScanHit> hits;
  };
  View view() const;
  // Cached reply to a network command and when it came.
  bool result(const std::string &cmd, std::string &json, int64_t &epoch) const;
  void clear_hits();
  bool set_mac(const std::string &mac);  // false if malformed
  void reset_mac();

  // Percent-encode a parameter value as the app does (RFC 3986 unreserved kept).
  static std::string uri_encode(const std::string &v);

 private:
  struct Job {
    enum Kind { REFRESH, COMMAND, SCAN, PROBE } kind;
    std::string cmd, params;
  };
  void run_();
  void do_job_(const Job &job);
  bool ensure_adapter_(std::string &err);
  bool scan_(int seconds, std::string &err);
  bool hear_(const std::string &dev, int max_s);
  bool session_(const std::vector<std::pair<std::string, std::string>> &cmds, std::string &err);
  bool exchange_(const std::string &write_path, const std::string &text, std::string &reply, std::string &err);
  void store_(const std::string &cmd, const std::string &reply);
  void save_locked_() const;
  void load_();

  std::string path_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::atomic<bool> stop_{false};
  std::thread thread_;

  // Cache and settings, under mu_.
  View v_;
  std::map<std::string, std::pair<std::string, int64_t>> results_;

  // Worker-thread only.
  bool open_bus_();
  sd_bus *bus_ = nullptr;
  std::string adapter_path_;
  uint8_t req_num_ = 0;
  std::vector<uint8_t> rx_;
  bool rx_done_ = false;

  friend int cca_on_properties_changed(sd_bus_message *, void *, sd_bus_error *);
};

}  // namespace tigod
