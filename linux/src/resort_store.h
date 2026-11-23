#pragma once

// The re-sort's state (MasterCore's ResortFile, resort.h), kept in
// <state_dir>/resort.json while one runs: the optimizers it waits for and
// every TAP's network before it. Those networks hold the TAPs' keys, so the
// file is mode 0600, written with write_file_atomic, deleted when the
// re-sort ends, and never logged, exported or backed up (the owner,
// 2026-11-23). Nothing else is kept on disk with a key in it.

#include "../../components/tigo_master/master_core.h"

#include <mutex>
#include <optional>
#include <string>

namespace tigod {

class ResortStore {
 public:
  explicit ResortStore(std::string path) : path_(std::move(path)) {}
  std::optional<esphome::tigo_master::ResortFile> load();  // none, or unreadable (said, without its content)
  bool save(const esphome::tigo_master::ResortFile &f);
  bool clear();  // true once it's gone

 private:
  std::string path_;
  std::mutex mu_;
};

}  // namespace tigod
