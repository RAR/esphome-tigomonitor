// The Topology editor's layout on the ESP32: saving one, keeping it in NVS,
// and holding what arrives after it for the user. The rules (what is valid,
// panel names, the tree, apply-or-hold) are tigo_layout.h's, shared with the
// Raspberry Pi build; this file is where they meet the node table and NVS.
//
// A panel's place stays in its node's labels, as a CCA sync leaves it. The
// skeleton (inverters, MPPTs and strings, including ones with no panels yet)
// is encoded as text over up to kSkeletonSlots NVS entries.

#include "tigo_monitor.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace tigo_monitor {

static const char *const LTAG = "tigo_layout";
static constexpr int kSkeletonSlots = 8;
static constexpr size_t kSlotBytes = 256;  // one char[256] preference each, NUL included

static uint32_t slot_hash(int i) {
  char key[16];
  snprintf(key, sizeof(key), "layout_%d", i);
  return fnv1_hash(key);
}

void TigoMonitorComponent::load_layout_() {
  StateLock lock(state_mutex_);
  yaml_inverters_ = inverters_;  // add_inverter() has run: these are the YAML's
  std::string text;
  for (int i = 0; i < kSkeletonSlots; i++) {
    char buf[kSlotBytes] = {};
    if (!this->cached_pref_<char[kSlotBytes]>(slot_hash(i)).load(&buf) || buf[0] == '\0') break;
    buf[kSlotBytes - 1] = '\0';
    text += buf;
  }
  if (text.empty()) return;
  layout::Layout sk;
  bool manual = false;
  layout::Declined declined;
  if (!layout::decode_skeleton(text, sk, manual, declined)) {
    ESP_LOGW(LTAG, "Saved layout unreadable; using the YAML's");
    return;
  }
  skeleton_ = std::move(sk);
  layout_manual_ = manual;
  layout_declined_ = std::move(declined);
  bool yaml_has_inverters = !inverters_.empty();
  if (!manual && yaml_has_inverters) {
    skeleton_.inverters.clear();  // the YAML's inverters stand
    return;
  }
  layout::InverterMppts yaml = inverter_mppts_();
  inverters_from_skeleton_();
  ESP_LOGI(LTAG, "Layout from the Topology editor: %zu inverters%s", skeleton_.inverters.size(),
           manual ? " (the user's own)" : "");
  // A YAML inverters: block that disagrees waits for the user, like a sync.
  if (manual && yaml_has_inverters && yaml != inverter_mppts_()) {
    uint32_t yfp = layout::fingerprint({}, yaml);
    if (yfp != layout_declined_["yaml"]) {
      held_layout_ = HeldLayout{"yaml", {}, yaml, yfp};
      ESP_LOGW(LTAG, "The YAML's inverters differ from the saved layout; the Topology page asks which to keep");
    }
  }
}

bool TigoMonitorComponent::save_skeleton_() {
  std::string text = layout::encode_skeleton(skeleton_, layout_manual_, layout_declined_);
  if (text.size() > kSkeletonSlots * (kSlotBytes - 1)) return false;
  for (int i = 0; i < kSkeletonSlots; i++) {
    char buf[kSlotBytes] = {};
    size_t off = static_cast<size_t>(i) * (kSlotBytes - 1);
    if (off < text.size()) strncpy(buf, text.c_str() + off, kSlotBytes - 1);
    this->cached_pref_<char[kSlotBytes]>(slot_hash(i)).save(&buf);
  }
  return true;
}

void TigoMonitorComponent::inverters_from_skeleton_() {
  inverters_.clear();
  for (const auto &inv : skeleton_.inverters) {
    InverterData d;
    d.name = to_node_string(inv.name);
    d.display_name = to_node_string(inv.display_name);
    for (const auto &m : inv.mppts) d.mppt_labels.push_back(to_node_string(m.label));
    inverters_.push_back(d);
  }
}

layout::InverterMppts TigoMonitorComponent::inverter_mppts_() const {
  layout::InverterMppts out;
  for (const auto &inv : inverters_) {
    std::vector<std::string> mppts;
    for (const auto &m : inv.mppt_labels) mppts.push_back(to_std_string(m));
    out.push_back({to_std_string(inv.name), mppts});
  }
  return out;
}

// The saved skeleton when the user made one; else the inverters as they
// stand, with no strings of their own (those come from the nodes).
layout::Layout TigoMonitorComponent::current_skeleton_() const {
  if (layout_manual_) return skeleton_;
  layout::Layout l;
  for (const auto &inv : inverters_) {
    layout::InverterSpec s;
    s.name = to_std_string(inv.name);
    s.display_name = to_std_string(inv.display_name);
    for (const auto &m : inv.mppt_labels) s.mppts.push_back({to_std_string(m), {}});
    l.inverters.push_back(std::move(s));
  }
  return l;
}

void TigoMonitorComponent::save_string_prefs_(const std::string &label, const std::string &display,
                                              uint16_t rating_w) {
  char key[80];
  snprintf(key, sizeof(key), "str_dn:%s", label.c_str());
  char dn[64] = {};
  strncpy(dn, display.c_str(), sizeof(dn) - 1);
  this->cached_pref_<char[64]>(fnv1_hash(key)).save(&dn);
  snprintf(key, sizeof(key), "str_rt:%s", label.c_str());
  this->cached_pref_<uint16_t>(fnv1_hash(key)).save(&rating_w);
}

void TigoMonitorComponent::write_assignments_(const std::vector<layout::Assignment> &as, bool clear_unlisted) {
  std::map<std::string, const layout::Assignment *> by_bc;
  for (const auto &a : as) by_bc[a.barcode] = &a;
  for (auto &n : node_table_) {
    if (n.long_address.empty()) continue;
    auto it = by_bc.find(to_std_string(n.long_address));
    if (it != by_bc.end()) {
      n.cca_label = to_node_string(it->second->label);
      n.cca_string_label = to_node_string(it->second->string_label);
      n.cca_mppt_label = to_node_string(it->second->mppt_label);
    } else if (clear_unlisted) {
      n.cca_label.clear();
      n.cca_string_label.clear();
      n.cca_mppt_label.clear();
    }
  }
}

bool TigoMonitorComponent::layout_manual() const {
  StateLock lock(state_mutex_);
  return layout_manual_;
}

std::vector<layout::Assignment> TigoMonitorComponent::current_assignments() const {
  StateLock lock(state_mutex_);
  std::vector<layout::Assignment> out;
  for (const auto &n : node_table_)
    if (!n.long_address.empty())
      out.push_back({to_std_string(n.long_address), to_std_string(n.cca_label), to_std_string(n.cca_string_label),
                     to_std_string(n.cca_mppt_label)});
  return out;
}

layout::Tree TigoMonitorComponent::layout_tree(bool &manual, std::string &pending_source, size_t &pending_changes) {
  StateLock lock(state_mutex_);
  std::vector<layout::NodeView> nodes;
  for (const auto &n : node_table_)
    nodes.push_back({to_std_string(n.long_address), to_std_string(n.addr), to_std_string(n.cca_label),
                     to_std_string(n.cca_string_label), to_std_string(n.cca_mppt_label)});
  layout::Tree t = layout::build_tree(current_skeleton_(), nodes, [this](layout::StringSpec &s) {
    auto it = strings_.find(to_node_string(s.label));
    if (it == strings_.end()) return;
    s.display_label = to_std_string(it->second.display_label);
    s.rating_w = it->second.panel_rating_w;
  });
  manual = layout_manual_;
  pending_source = held_layout_ ? held_layout_->source : "";
  pending_changes = 0;
  if (held_layout_) {
    pending_changes = layout::diff(current_assignments(), held_layout_->assignments).size();
    if (!held_layout_->inverters.empty() && held_layout_->inverters != inverter_mppts_()) pending_changes++;
  }
  return t;
}

std::string TigoMonitorComponent::set_layout(const layout::Layout &l) {
  StateLock lock(state_mutex_);
  std::string err = layout::validate(l, [this](const std::string &bc) {
    for (const auto &n : node_table_)
      if (to_std_string(n.long_address) == bc) return true;
    return false;
  });
  if (!err.empty()) return err;
  layout::Layout sk = l;
  for (auto &inv : sk.inverters)
    for (auto &m : inv.mppts)
      for (auto &s : m.strings) s.panels.clear();
  if (layout::encode_skeleton(sk, true, layout_declined_).size() > kSkeletonSlots * (kSlotBytes - 1))
    return "This layout has too many inverters, MPPTs or strings to store on this device.";

  write_assignments_(layout::assignments(l), true);
  skeleton_ = std::move(sk);
  layout_manual_ = true;
  held_layout_.reset();
  inverters_from_skeleton_();
  for (const auto &inv : skeleton_.inverters)
    for (const auto &m : inv.mppts)
      for (const auto &s : m.strings) save_string_prefs_(s.label, s.display_label, static_cast<uint16_t>(s.rating_w));
  save_skeleton_();
  save_node_table();
  rebuild_string_groups();
  ESP_LOGI(LTAG, "Saved a layout from the Topology editor: %zu inverters", skeleton_.inverters.size());
  return "";
}

layout::Offer TigoMonitorComponent::offer_layout(const char *source, std::vector<layout::Assignment> incoming,
                                                 layout::InverterMppts inverters) {
  StateLock lock(state_mutex_);
  bool changes = !layout::diff(current_assignments(), incoming).empty() ||
                 (!inverters.empty() && inverters != inverter_mppts_());
  uint32_t fp = layout::fingerprint(incoming, inverters);
  layout::Offer o = layout::decide(layout_manual_, changes, fp, layout_declined_[source]);
  if (o == layout::Offer::hold) {
    held_layout_ = HeldLayout{source, std::move(incoming), std::move(inverters), fp};
    ESP_LOGW(LTAG, "A layout from %s differs from the user's; the Topology page asks which to keep", source);
  } else if (o != layout::Offer::apply) {
    ESP_LOGI(LTAG, "A layout from %s: %s", source,
             o == layout::Offer::unchanged ? "no different" : "already turned down");
  }
  return o;
}

bool TigoMonitorComponent::layout_pending(std::string &source, std::vector<layout::Change> &changes,
                                          layout::InverterMppts &now, layout::InverterMppts &then) {
  StateLock lock(state_mutex_);
  if (!held_layout_) return false;
  source = held_layout_->source;
  changes = layout::diff(current_assignments(), held_layout_->assignments);
  then = held_layout_->inverters;
  now.clear();
  if (!then.empty()) now = inverter_mppts_();
  return true;
}

bool TigoMonitorComponent::resolve_layout_pending(bool use) {
  StateLock lock(state_mutex_);
  if (!held_layout_) return false;
  HeldLayout h = std::move(*held_layout_);
  held_layout_.reset();
  if (!use) {
    layout_declined_[h.source] = h.fp;
    save_skeleton_();
    ESP_LOGI(LTAG, "Kept the user's layout over the one from %s", h.source.c_str());
    return true;
  }
  write_assignments_(h.assignments, false);
  layout_manual_ = false;
  for (auto &inv : skeleton_.inverters)
    for (auto &m : inv.mppts) m.strings.clear();
  // Back to the YAML's inverters if it has any; else the saved ones stay.
  if (!yaml_inverters_.empty()) {
    inverters_ = yaml_inverters_;
    skeleton_.inverters.clear();
  }
  save_skeleton_();
  save_node_table();
  rebuild_string_groups();
  ESP_LOGI(LTAG, "Took the layout from %s", h.source.c_str());
  return true;
}

}  // namespace tigo_monitor
}  // namespace esphome
