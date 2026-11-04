#pragma once

// The discovery switch (MasterCore::request_discovery), kept in
// <state_dir>/discovery.json, beside pv.json and for the same reason: a
// settings save rebuilds the bus master. A missing or unreadable file is
// renewing, which is what the CCA does.

#include <mutex>
#include <string>

namespace tigod {

class DiscoveryStore {
 public:
  explicit DiscoveryStore(std::string path) : path_(std::move(path)) {}
  void load();
  bool renew() const;
  void save(bool renew);

 private:
  std::string path_;
  mutable std::mutex mu_;
  bool renew_ = true;
};

}  // namespace tigod
