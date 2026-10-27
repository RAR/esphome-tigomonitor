#pragma once

// The 0x41s the CCA sends (tigo_master.h's Housekeeping), learned off the bus
// while listening beside it, so the master can send the same ones with the
// CCA unplugged. Kept in <state_dir>/cca-0x41.json: they are the site's own
// and stay on this machine.

#include "tigo_master.h"
#include "tigo_protocol.h"

#include <mutex>
#include <string>
#include <vector>

namespace tigod {

class HousekeepingStore {
 public:
  explicit HousekeepingStore(std::string path);
  void load();
  // Bytes read off the bus while listening. Only the CCA's requests teach it.
  void feed(const char *data, size_t n);
  // Writes the file if anything new was learned since the last save.
  void save_if_changed();
  std::vector<esphome::tigo_monitor::master::Learned41::Entry> entries() const;

 private:
  std::string path_;
  mutable std::mutex mu_;
  esphome::tigo_monitor::master::Learned41 learned_;
  esphome::tigo_monitor::protocol::FrameSplitter<std::string> splitter_;
  bool dirty_ = false;
};

}  // namespace tigod
