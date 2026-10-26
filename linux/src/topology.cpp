#include "topology.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <fstream>

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
      strings_[kv.key()] = so;
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
  return write_file_atomic(
      path_, json({{"inverters", inv}, {"strings", str}, {"pending_inverter_names", pending_names_}}).dump(2) + "\n");
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
