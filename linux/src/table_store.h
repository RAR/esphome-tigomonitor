#pragma once

// A removal's list of the optimizers that stay in the TAP's table
// (MasterCore::set_table_saved), kept in <state_dir>/table-remove.json from
// just before the TAP's table is erased until they are written back. A file
// found at the start is a removal that stopped in between: the next master
// finishes it. It holds MACs, so it is readable by this machine's root only.

#include "../../components/tigo_monitor/table_edit.h"

#include <mutex>
#include <string>
#include <vector>

namespace tigod {

class TableStore {
 public:
  explicit TableStore(std::string path) : path_(std::move(path)) {}
  // True if a removal was left unfinished; its MAC and the list.
  bool load(esphome::tigo_monitor::master::Bytes &mac, std::vector<esphome::tigo_monitor::master::TableRecord> &stay);
  // stay null: the removal finished, so the file goes.
  bool save(const esphome::tigo_monitor::master::Bytes &mac,
            const std::vector<esphome::tigo_monitor::master::TableRecord> *stay);

 private:
  std::string path_;
  std::mutex mu_;
};

}  // namespace tigod
