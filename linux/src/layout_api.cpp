#include "layout_api.h"

#include "log.h"

#include <algorithm>
#include <climits>
#include <map>
#include <set>

namespace tigod {

using nlohmann::json;
namespace layout = esphome::tigo_monitor::layout;

// The tree rule is tigo_layout.h's build_tree(), shared with the firmware.
json layout_json(const Monitor &mon, const Topology &topo) {
  auto ov = topo.string_overrides();
  std::vector<layout::NodeView> nodes;
  std::set<std::string> known;
  for (const auto &n : mon.nodes()) {
    nodes.push_back({n.long_address, n.addr, n.cca_label, n.cca_string_label, n.cca_mppt_label});
    if (n.known) known.insert(n.long_address);
  }
  layout::Tree t = layout::build_tree(topo.skeleton(), nodes, [&ov](layout::StringSpec &s) {
    auto it = ov.find(s.label);
    if (it == ov.end()) return;
    s.display_label = it->second.display_label;
    s.rating_w = it->second.panel_rating_w;
  });

  // first_heard_unix: when a new optimizer (Monitor::first_heard) was first
  // heard; 0 for the panels the monitor started out with.
  auto heard = mon.first_heard();
  // known: the owner marked it as not reporting, and expected not to.
  auto panel = [&heard, &known](const layout::NodeView &n) {
    auto it = heard.find(n.barcode);
    return json{{"barcode", n.barcode}, {"addr", n.addr}, {"name", n.label},
                {"first_heard_unix", it == heard.end() ? 0 : it->second}, {"known", known.count(n.barcode) > 0}};
  };
  auto mppts_json = [&](const std::vector<layout::MpptSpec> &ms) {
    json arr = json::array();
    for (const auto &m : ms) {
      json strings = json::array();
      for (const auto &s : m.strings) {
        json panels = json::array();
        for (const auto &n : t.panels[s.label]) panels.push_back(panel(n));
        strings.push_back(
            {{"label", s.label}, {"display_label", s.display_label}, {"rating_w", s.rating_w}, {"panels", panels}});
      }
      arr.push_back({{"label", m.label}, {"display_label", m.display_label}, {"strings", strings}});
    }
    return arr;
  };
  json inverters = json::array();
  for (const auto &inv : t.layout.inverters)
    inverters.push_back({{"name", inv.name}, {"display_name", inv.display_name}, {"mppts", mppts_json(inv.mppts)}});
  json unassigned = json::array();
  for (const auto &n : t.unassigned) unassigned.push_back(panel(n));

  json pending = nullptr;
  if (auto p = topo.pending())
    pending = {{"source", p->source}, {"changes", layout::diff(mon.current_assignments(), p->assignments).size()}};

  return {{"manual", topo.manual()},
          {"inverters", inverters},
          {"unplaced_mppts", mppts_json(t.unplaced)},
          {"unassigned", unassigned},
          {"pending", pending}};
}

std::string parse_layout(const json &body, layout::Layout &out) {
  if (!body.is_object() || !body.contains("inverters") || !body["inverters"].is_array())
    return "expected an \"inverters\" array";
  auto str = [](const json &o, const char *k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  out = {};
  for (const auto &i : body["inverters"]) {
    if (!i.is_object()) return "every inverter must be an object";
    layout::InverterSpec inv;
    inv.name = str(i, "name");
    inv.display_name = str(i, "display_name");
    for (const auto &m : i.value("mppts", json::array())) {
      if (!m.is_object()) return "every MPPT must be an object";
      layout::MpptSpec ms;
      ms.label = str(m, "label");
      ms.display_label = str(m, "display_label");
      for (const auto &s : m.value("strings", json::array())) {
        if (!s.is_object()) return "every string must be an object";
        layout::StringSpec ss;
        ss.label = str(s, "label");
        ss.display_label = str(s, "display_label");
        if (s.contains("rating_w")) {
          if (!s["rating_w"].is_number_integer()) return "rating_w must be a whole number";
          ss.rating_w = s["rating_w"].get<int>();
        }
        for (const auto &p : s.value("panels", json::array())) {
          if (!p.is_string()) return "panels must be barcodes";
          ss.panels.push_back(p.get<std::string>());
        }
        ms.strings.push_back(std::move(ss));
      }
      inv.mppts.push_back(std::move(ms));
    }
    out.inverters.push_back(std::move(inv));
  }
  return "";
}

namespace {

layout::InverterMppts inverter_mppts(const std::vector<InverterConfig> &invs) {
  layout::InverterMppts out;
  for (const auto &i : invs) out.push_back({i.name, i.mppts});
  return out;
}

const char *offer_name(layout::Offer o) {
  switch (o) {
    case layout::Offer::apply: return "applied";
    case layout::Offer::hold: return "held for the user";
    case layout::Offer::unchanged: return "no different";
    case layout::Offer::declined: return "already turned down";
  }
  return "";
}

// Applies an incoming layout the way its source always has.
void apply_incoming(Monitor &mon, Topology &topo, const PendingLayout &in, bool replace_inverters,
                    bool *inverters_added) {
  if (!in.panels.empty()) mon.apply_layout(in.panels);
  else mon.set_assignments(in.assignments, false);
  // The cloud's inverters only on a Pi with none set up, so they never undo
  // an edit, unless the user chose them.
  if (!in.inverters.empty() && !in.assignments.empty() && (replace_inverters || topo.inverters().empty())) {
    bool ok = topo.set_inverters(in.inverters);
    if (inverters_added) *inverters_added = ok;
  }
}

}  // namespace

layout::Offer offer_layout(Monitor &mon, Topology &topo, PendingLayout in, bool *inverters_added) {
  if (inverters_added) *inverters_added = false;
  if (in.assignments.empty() && !in.panels.empty()) in.assignments = mon.resolve_layout(in.panels);
  bool changes = !layout::diff(mon.current_assignments(), in.assignments).empty();
  if (!in.inverters.empty() && inverter_mppts(in.inverters) != inverter_mppts(topo.inverters())) changes = true;
  in.fp = layout::fingerprint(in.assignments, inverter_mppts(in.inverters));
  layout::Offer o = layout::decide(topo.manual(), changes, in.fp, topo.declined_fp(in.source));
  if (o == layout::Offer::apply) apply_incoming(mon, topo, in, false, inverters_added);
  LOGI("layout: from %s, %zu panels matched: %s", in.source.c_str(), in.assignments.size(), offer_name(o));
  if (o == layout::Offer::hold) topo.set_pending(std::move(in));
  return o;
}

json pending_json(const Monitor &mon, const Topology &topo) {
  auto p = topo.pending();
  if (!p) return {{"pending", nullptr}};
  auto place = [](const layout::Assignment &a) {
    return json{{"name", a.label}, {"string", a.string_label}, {"mppt", a.mppt_label}};
  };
  json changes = json::array();
  for (const auto &c : layout::diff(mon.current_assignments(), p->assignments))
    changes.push_back({{"barcode", c.barcode}, {"now", place(c.now)}, {"then", place(c.then)}});
  auto invs = [](const std::vector<InverterConfig> &v) {
    json arr = json::array();
    for (const auto &i : v) arr.push_back({{"name", i.name}, {"mppts", i.mppts}});
    return arr;
  };
  json out = {{"source", p->source}, {"changes", changes}, {"inverters_now", nullptr}, {"inverters_then", nullptr}};
  if (!p->inverters.empty()) {
    out["inverters_now"] = invs(topo.inverters());
    out["inverters_then"] = invs(p->inverters);
  }
  return out;
}

bool resolve_pending(Monitor &mon, Topology &topo, bool use) {
  auto p = topo.pending();
  if (!p) return false;
  if (use) {
    apply_incoming(mon, topo, *p, true, nullptr);
    topo.set_manual(false);
    topo.clear_skeleton_strings();
    LOGI("layout: the user took the %s layout", p->source.c_str());
  } else {
    topo.set_declined_fp(p->source, p->fp);
    LOGI("layout: the user kept theirs over the %s layout", p->source.c_str());
  }
  topo.clear_pending();
  return true;
}

}  // namespace tigod
