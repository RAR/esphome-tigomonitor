#pragma once

// The CCA over its local HTTP API, for CCA firmware before 4.0.4 (later
// firmware locks it; use Bluetooth): the firmware's query_cca_device_info()
// and query_cca_config(). DEVICE_INFO feeds the CCA Info page, and
// summary_config's panel/string/inverter tree names the node table.
// Used when Device Configuration has a CCA IP, as cca_source: http is.

#include "monitor.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace tigod {

class CcaHttp {
 public:
  explicit CcaHttp(Monitor &monitor);
  ~CcaHttp();
  void start();
  void stop();

  // Queues DEVICE_INFO and a config sync against ip; returns at once.
  void request_refresh(const std::string &ip);

  struct View {
    std::string device_info;  // raw JSON, or {"error": ...}
    int64_t epoch = 0;        // last attempt
    bool busy = false;
  };
  View view() const;

  // Parses summary_config into layout panels (exposed for tests).
  static bool parse_summary_config(const std::string &body, std::vector<LayoutPanel> &out);

 private:
  void run_();
  Monitor &monitor_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::string pending_ip_;
  bool stop_ = false;
  View v_;
  std::thread thread_;
};

}  // namespace tigod
