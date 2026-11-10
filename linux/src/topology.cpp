#include "topology.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace tigod {

using nlohmann::json;

Topology::Topology(std::string path) : path_(std::move(path)) {}

void Topology::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    LOGW("topology: %s is not valid JSON, ignoring it", path_.c_str());
    return;
  }
  try {
    const json invs = j.value("inverters", json::array()), strs = j.value("strings", json::object());
    const json pending = j.value("pending_inverter_names", json::object());
    const json ms = j.value("mppt_strings", json::object());
    for (const auto &kv : ms.items()) mppt_strings_[kv.key()] = kv.value().get<std::vector<std::string>>();
    const json mn = j.value("mppt_names", json::object());
    for (const auto &kv : mn.items())
      if (kv.value().is_string()) mppt_names_[kv.key()] = kv.value();
    manual_ = j.value("manual", false);
    const json declined = j.value("declined", json::object());
    for (const auto &kv : declined.items())
      if (kv.value().is_number_unsigned()) declined_[kv.key()] = kv.value().get<uint32_t>();
    for (const auto &o : invs) {
      InverterConfig inv;
      inv.name = o.value("name", "");
      inv.display_name = o.value("display_name", "");
      inv.mppts = o.value("mppts", std::vector<std::string>{});
      if (!inv.name.empty()) inverters_.push_back(std::move(inv));
    }
    for (const auto &kv : pending.items())
      if (kv.value().is_string()) pending_names_[kv.key()] = kv.value();
    for (const auto &kv : strs.items()) {
      StringOverride so;
      so.display_label = kv.value().value("display_label", "");
      so.panel_rating_w = kv.value().value("panel_rating_w", 0);
      // The no-string group's old name: its rating carries over.
      strings_[kv.key() == Monitor::OLD_NO_STRING ? Monitor::NO_STRING : kv.key()] = so;
    }
  } catch (const json::exception &) {
    LOGW("topology: %s has a field of the wrong type; using what was read", path_.c_str());
  }
  LOGI("topology: %zu inverters", inverters_.size());
}

bool Topology::save_locked_() const {
  json inv = json::array();
  for (const auto &i : inverters_)
    inv.push_back({{"name", i.name}, {"display_name", i.display_name}, {"mppts", i.mppts}});
  json str = json::object();
  for (const auto &kv : strings_) {
    if (kv.second.display_label.empty() && kv.second.panel_rating_w == 0) continue;
    str[kv.first] = {{"display_label", kv.second.display_label}, {"panel_rating_w", kv.second.panel_rating_w}};
  }
  return write_file_atomic(path_, json({{"inverters", inv},
                                        {"strings", str},
                                        {"pending_inverter_names", pending_names_},
                                        {"mppt_strings", mppt_strings_},
                                        {"mppt_names", mppt_names_},
                                        {"manual", manual_},
                                        {"declined", declined_}})
                                      .dump(2) +
                                      "\n");
}

std::vector<InverterConfig> Topology::inverters() const {
  std::lock_guard<std::mutex> lock(mu_);
  return inverters_;
}

std::map<std::string, StringOverride> Topology::string_overrides() const {
  std::lock_guard<std::mutex> lock(mu_);
  return strings_;
}

bool Topology::set_inverters(std::vector<InverterConfig> inverters) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &n : inverters) {
    for (const auto &o : inverters_)
      if (n.name == o.name && n.display_name.empty()) n.display_name = o.display_name;
    auto p = pending_names_.find(n.name);
    if (p != pending_names_.end()) {
      if (n.display_name.empty()) n.display_name = p->second;
      pending_names_.erase(p);
    }
  }
  inverters_ = std::move(inverters);
  // A name is for an MPPT in a given inverter; one no inverter lists any
  // more must not name a different MPPT that later takes its label.
  for (auto it = mppt_names_.begin(); it != mppt_names_.end();) {
    bool listed = false;
    for (const auto &i : inverters_)
      for (const auto &m : i.mppts) listed = listed || m == it->first;
    it = listed ? std::next(it) : mppt_names_.erase(it);
  }
  return save_locked_();
}

bool Topology::set_inverter_display_name(const std::string &name, const std::string &display) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &i : inverters_) {
    if (i.name != name) continue;
    i.display_name = display;
    save_locked_();
    return true;
  }
  return false;
}

void Topology::remember_inverter_display_name(const std::string &name, const std::string &display) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &i : inverters_) {
    if (i.name != name) continue;
    i.display_name = display;
    save_locked_();
    return;
  }
  pending_names_[name] = display;
  save_locked_();
}

bool Topology::set_string_display_label(const std::string &label, const std::string &display) {
  std::lock_guard<std::mutex> lock(mu_);
  strings_[label].display_label = display;
  return save_locked_();
}

bool Topology::set_string_rating(const std::string &label, int rating_w) {
  std::lock_guard<std::mutex> lock(mu_);
  strings_[label].panel_rating_w = rating_w;
  return save_locked_();
}

}  // namespace tigod

namespace tigod {

namespace layout = esphome::tigo_monitor::layout;

bool Topology::manual() const {
  std::lock_guard<std::mutex> lock(mu_);
  return manual_;
}

bool Topology::set_manual(bool manual) {
  std::lock_guard<std::mutex> lock(mu_);
  manual_ = manual;
  return save_locked_();
}

uint32_t Topology::declined_fp(const std::string &source) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = declined_.find(source);
  return it == declined_.end() ? 0 : it->second;
}

bool Topology::set_declined_fp(const std::string &source, uint32_t fp) {
  std::lock_guard<std::mutex> lock(mu_);
  declined_[source] = fp;
  return save_locked_();
}

layout::Layout Topology::skeleton() const {
  std::lock_guard<std::mutex> lock(mu_);
  layout::Layout l;
  for (const auto &i : inverters_) {
    layout::InverterSpec inv;
    inv.name = i.name;
    inv.display_name = i.display_name;
    for (const auto &m : i.mppts) {
      layout::MpptSpec ms;
      ms.label = m;
      auto name = mppt_names_.find(m);
      if (name != mppt_names_.end()) ms.display_label = name->second;
      auto it = mppt_strings_.find(m);
      if (it != mppt_strings_.end()) {
        for (const auto &label : it->second) {
          layout::StringSpec ss;
          ss.label = label;
          auto so = strings_.find(label);
          if (so != strings_.end()) {
            ss.display_label = so->second.display_label;
            ss.rating_w = so->second.panel_rating_w;
          }
          ms.strings.push_back(std::move(ss));
        }
      }
      inv.mppts.push_back(std::move(ms));
    }
    l.inverters.push_back(std::move(inv));
  }
  return l;
}

bool Topology::set_skeleton(const layout::Layout &l) {
  std::lock_guard<std::mutex> lock(mu_);
  inverters_.clear();
  mppt_strings_.clear();
  mppt_names_.clear();
  for (const auto &inv : l.inverters) {
    InverterConfig ic;
    ic.name = inv.name;
    ic.display_name = inv.display_name;
    pending_names_.erase(inv.name);
    for (const auto &m : inv.mppts) {
      ic.mppts.push_back(m.label);
      if (!m.display_label.empty()) mppt_names_[m.label] = m.display_label;
      auto &labels = mppt_strings_[m.label];
      for (const auto &s : m.strings) {
        labels.push_back(s.label);
        strings_[s.label].display_label = s.display_label;
        strings_[s.label].panel_rating_w = s.rating_w;
      }
    }
    inverters_.push_back(std::move(ic));
  }
  return save_locked_();
}

std::map<std::string, std::string> Topology::mppt_names() const {
  std::lock_guard<std::mutex> lock(mu_);
  return mppt_names_;
}

bool Topology::clear_skeleton_strings() {
  std::lock_guard<std::mutex> lock(mu_);
  mppt_strings_.clear();
  return save_locked_();
}

void Topology::set_pending(PendingLayout p) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_ = std::move(p);
}

std::optional<PendingLayout> Topology::pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_;
}

void Topology::clear_pending() {
  std::lock_guard<std::mutex> lock(mu_);
  pending_.reset();
}

}  // namespace tigod
