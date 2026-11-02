#include "layout_api.h"

#include "log.h"

#include <algorithm>
#include <climits>
#include <map>

namespace tigod {

using nlohmann::json;
namespace layout = esphome::tigo_monitor::layout;

namespace {

struct TreePanel {
  std::string barcode, addr, name;
};

json panels_json(std::vector<TreePanel> panels) {
  std::sort(panels.begin(), panels.end(), [](const TreePanel &a, const TreePanel &b) {
    int pa = layout::position_of(a.name), pb = layout::position_of(b.name);
    return pa != pb ? pa < pb : a.barcode < b.barcode;
  });
  json out = json::array();
  for (const auto &p : panels) out.push_back({{"barcode", p.barcode}, {"addr", p.addr}, {"name", p.name}});
  return out;
}

}  // namespace

// The skeleton first, then whatever the nodes say that it doesn't: a string
// it lacks goes under its nodes' MPPT, and an MPPT no inverter lists goes in
// unplaced_mppts (label "" for strings with no MPPT at all).
json layout_json(const Monitor &mon, const Topology &topo) {
  layout::Layout tree = topo.skeleton();
  std::vector<layout::MpptSpec> unplaced;
  auto ov = topo.string_overrides();
  std::map<std::string, std::vector<TreePanel>> panels;  // string label -> panels
  json unassigned = json::array();

  auto find_mppt = [&](const std::string &label) -> layout::MpptSpec * {
    for (auto &inv : tree.inverters)
      for (auto &m : inv.mppts)
        if (m.label == label) return &m;
    for (auto &m : unplaced)
      if (m.label == label) return &m;
    return nullptr;
  };
  auto has_string = [&](const std::string &label) {
    auto in = [&](const std::vector<layout::MpptSpec> &ms) {
      for (const auto &m : ms)
        for (const auto &s : m.strings)
          if (s.label == label) return true;
      return false;
    };
    for (const auto &inv : tree.inverters)
      if (in(inv.mppts)) return true;
    return in(unplaced);
  };

  for (const auto &n : mon.nodes()) {
    if (n.long_address.empty()) continue;
    if (n.cca_string_label.empty()) {
      unassigned.push_back({{"barcode", n.long_address}, {"addr", n.addr}, {"name", n.cca_label}});
      continue;
    }
    panels[n.cca_string_label].push_back({n.long_address, n.addr, n.cca_label});
    if (has_string(n.cca_string_label)) continue;
    layout::MpptSpec *m = find_mppt(n.cca_mppt_label);
    if (!m) {
      unplaced.push_back({n.cca_mppt_label, {}});
      m = &unplaced.back();
    }
    layout::StringSpec s;
    s.label = n.cca_string_label;
    auto it = ov.find(s.label);
    if (it != ov.end()) {
      s.display_label = it->second.display_label;
      s.rating_w = it->second.panel_rating_w;
    }
    m->strings.push_back(s);
  }

  auto mppts_json = [&](const std::vector<layout::MpptSpec> &ms) {
    json arr = json::array();
    for (const auto &m : ms) {
      json strings = json::array();
      for (const auto &s : m.strings)
        strings.push_back({{"label", s.label},
                           {"display_label", s.display_label},
                           {"rating_w", s.rating_w},
                           {"panels", panels_json(panels[s.label])}});
      arr.push_back({{"label", m.label}, {"strings", strings}});
    }
    return arr;
  };
  json inverters = json::array();
  for (const auto &inv : tree.inverters)
    inverters.push_back({{"name", inv.name}, {"display_name", inv.display_name}, {"mppts", mppts_json(inv.mppts)}});

  json pending = nullptr;
  if (auto p = topo.pending())
    pending = {{"source", p->source}, {"changes", layout::diff(mon.current_assignments(), p->assignments).size()}};

  return {{"manual", topo.manual()},
          {"inverters", inverters},
          {"unplaced_mppts", mppts_json(unplaced)},
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
  layout::Offer o = layout::decide(topo.manual(), changes, in.fp, topo.declined_fp());
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
    topo.set_declined_fp(p->fp);
    LOGI("layout: the user kept theirs over the %s layout", p->source.c_str());
  }
  topo.clear_pending();
  return true;
}

}  // namespace tigod
