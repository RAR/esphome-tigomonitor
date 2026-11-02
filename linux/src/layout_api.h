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

// Every layout that arrives (a CCA sync, a cloud import, a node-table
// import) comes through here: applied as it always was until the user saves
// their own, held for them after (tigo_layout.h decide()). An incoming
// layout with panels but no assignments has them worked out here.
// inverters_added: set when its inverters were applied.
esphome::tigo_monitor::layout::Offer offer_layout(Monitor &mon, Topology &topo, PendingLayout incoming,
                                                  bool *inverters_added = nullptr);

// GET /api/layout/pending: what the held layout would change, or
// {"pending": null}.
nlohmann::json pending_json(const Monitor &mon, const Topology &topo);

// The user's answer. keep: forget it, and don't ask about the same one
// again; use: apply it, and the layout is no longer theirs. False if
// nothing is held.
bool resolve_pending(Monitor &mon, Topology &topo, bool use);

}  // namespace tigod
