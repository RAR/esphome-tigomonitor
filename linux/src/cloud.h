#pragma once

// Tigo cloud import, as on the ESP32 (components/tigo_monitor/tigo_cloud.cpp):
// log in with the Tigo account once, then fetch the system layout — panel
// names, strings, MPPTs and inverters — and apply it to the node table. It is
// how a site whose CCA has its local HTTP API locked down (firmware 4.0.4+)
// gets its layout. Only the token is stored, in cloud.json (owner-only);
// never the password.

#include "monitor.h"

#include <mutex>
#include <string>
#include <vector>

namespace tigod {

struct CloudInverter {
  std::string label;
  std::vector<std::string> mppts;
};
struct CloudLayout {
  std::vector<LayoutPanel> panels;
  std::vector<CloudInverter> inverters;
};

class Cloud {
 public:
  explicit Cloud(std::string path);
  void load();

  struct Status {
    bool configured = false;
    std::string email, expires;
    int system_id = 0;
  };
  Status status() const;

  // Each returns false with a reason in err.
  bool login(const std::string &email, const std::string &password, std::string &err);
  bool layout(CloudLayout &out, std::string &err);
  // Tigo's equipment-status feeds, passed through as JSON arrays.
  bool health(std::string &out, std::string &err);
  bool equipment(const std::string &view, std::string &out, std::string &err);

 private:
  bool get_(const std::string &host, const std::string &path, std::string &body, std::string &err);
  void forget_token_locked_();
  bool save_locked_() const;
  std::string path_;
  mutable std::mutex mu_;
  std::string email_, token_, refresh_, expires_;
  int system_id_ = 0;
};

}  // namespace tigod
