#pragma once

// GET and POST /api/layout: the Topology editor's whole tree, the same JSON
// the firmware serves, built from the topology file and the node table.

#include "monitor.h"
#include "topology.h"

#include <nlohmann/json.hpp>

#include <string>

namespace tigod {

nlohmann::json layout_json(const Monitor &mon, const Topology &topo);

// A POST /api/layout body as a layout; "" or what is wrong with its shape.
// tigo_layout.h validate() judges the content.
std::string parse_layout(const nlohmann::json &body, esphome::tigo_monitor::layout::Layout &out);

}  // namespace tigod
