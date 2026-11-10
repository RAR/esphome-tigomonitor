#pragma once

// The TAPs this monitor runs, by MAC and RS485 id (MasterCore::set_taps),
// kept in <state_dir>/taps.json. A herd (bus master, adopting on) writes
// it, so a known TAP gets its id back, as a CCA's saved map does. None
// saved: the one TAP at gateway_id. It holds MACs, so it is readable by
// this machine's root only.

#include "../../components/tigo_master/master_core.h"

#include <mutex>
#include <string>
#include <vector>

namespace tigod {

class TapStore {
 public:
  explicit TapStore(std::string path) : path_(std::move(path)) {}
  // The saved TAPs in polling order; empty if none are saved (or unreadable).
  std::vector<esphome::tigo_master::MasterCore::TapEntry> load();
  bool save(const std::vector<esphome::tigo_master::MasterCore::TapEntry> &taps);

 private:
  std::string path_;
  std::mutex mu_;
};

}  // namespace tigod
