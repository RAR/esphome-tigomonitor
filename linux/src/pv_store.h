#pragma once

// The PV switch's state (master_core.h's PvState), kept in <state_dir>/pv.json
// rather than settings.json: a settings save rebuilds the bus master, and a
// state heard off the bus is state, not a setting. A missing file is PV on.
// One that can't be read holds PV off, and says so on /api/health: better
// an array left off than one turned back on by a damaged file.

#include "bus_master.h"

#include <mutex>
#include <string>

namespace tigod {

using esphome::tigo_master::PvState;

class PvStore {
 public:
  explicit PvStore(std::string path) : path_(std::move(path)) {}
  // False if the file was there but couldn't be read.
  bool load();
  PvState state() const;
  void save(const PvState &s);
  bool unreadable() const;

 private:
  std::string path_;
  mutable std::mutex mu_;
  PvState state_;
  bool unreadable_ = false;
};

}  // namespace tigod
