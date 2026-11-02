#include "layout_api.h"

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

}  // namespace tigod
