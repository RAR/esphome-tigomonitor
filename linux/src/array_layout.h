// The array layout (Topology > Array), for the Linux daemon: the shared
// array_grid.h (components/tigo_monitor), which both builds keep the same
// way, with nlohmann::json in and out as the daemon's other code has it.
// The rules, the parsing and the barcode following all live there.
#pragma once

#include "array_grid.h"

#include <nlohmann/json.hpp>

#include <string>

using esphome::tigo_monitor::array_grid::ArrayLayout;
using esphome::tigo_monitor::array_grid::ArrayPanel;
using esphome::tigo_monitor::array_grid::ArrayTap;
using esphome::tigo_monitor::array_grid::ARRAY_CELLS;
using esphome::tigo_monitor::array_grid::ARRAY_MAX;
using esphome::tigo_monitor::array_grid::array_fill_barcodes;
using esphome::tigo_monitor::array_grid::array_follow_barcodes;
using esphome::tigo_monitor::array_grid::array_node_maps;

// {"panels": [...], "taps": [...]} into out; "" when it's an array, else
// why not (out is then unchanged).
inline std::string array_from_json(const nlohmann::json &j, ArrayLayout &out) {
  return esphome::tigo_monitor::array_grid::array_from_text(j.dump(), out);
}

inline nlohmann::json array_to_json(const ArrayLayout &r) {
  std::string text;
  esphome::tigo_monitor::array_grid::array_write_json(text, r);
  return nlohmann::json::parse(text);
}
