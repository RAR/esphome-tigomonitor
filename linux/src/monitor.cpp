#include "monitor.h"

#include "fsutil.h"
#include "log.h"
#include "tigo_node_identity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>

namespace tigod {

namespace proto = esphome::tigo_monitor::protocol;
namespace identity = esphome::tigo_monitor::node_identity;
using nlohmann::json;

static std::string local_date() {
  time_t t = time(nullptr);
  tm lt{};
  localtime_r(&t, &lt);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
  return buf;
}

static int64_t unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Monitor::Monitor(std::string state_path, float power_calibration, size_t max_nodes)
    : state_path_(std::move(state_path)), power_calibration_(power_calibration), max_nodes_(max_nodes) {}

void Monitor::feed(const char *data, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  stats_.bytes += len;
  for (size_t i = 0; i < len; i++) {
    splitter_.feed(data[i], [this](const std::string &frame) { on_frame_(frame); });
  }
  stats_.ends_without_start = splitter_.ends_without_start();
  stats_.overflows = splitter_.overflows();
}

void Monitor::on_frame_(const std::string &raw) {
  stats_.frames++;
  std::string frame = proto::unescape(raw);
  if (!proto::checksum_ok(frame)) {
    stats_.bad_checksums++;
    LOGD("bad checksum: expected %04X, got %04X, %zu bytes", proto::computed_crc(frame),
         proto::carried_crc(frame), frame.size());
    return;
  }
  stats_.last_frame_unix_ms = unix_ms();
  last_frame_ = Clock::now();
  frame.resize(frame.size() - 2);
  std::string hex = proto::to_hex(frame);

  switch (proto::frame_type(hex)) {
    case proto::FrameType::power:
      if (!proto::walk_power_packets(hex, [this](const std::string &type, const std::string &packet) {
            if (type == "31") on_power_packet_(packet);
          }))
        LOGD("power frame ended in a truncated packet");
      break;
    case proto::FrameType::command:
      if (proto::command_byte(hex) == "27") on_frame27_(hex);
      break;
    default:
      break;
  }
}

Panel *Monitor::find_panel_(const std::string &addr) {
  for (auto &p : panels_)
    if (p.addr == addr) return &p;
  return nullptr;
}

void Monitor::on_power_packet_(const std::string &packet) {
  proto::PowerReading r;
  auto result = proto::decode_power_packet(packet, power_calibration_, r);
  if (result != proto::PowerDecode::ok) {
    LOGW("power packet too short (%zu chars), skipping: %s", packet.size(), packet.c_str());
    return;
  }
  stats_.power_packets++;
  Panel *p = find_panel_(r.addr);
  if (p == nullptr) {
    panels_.push_back(Panel{});
    p = &panels_.back();
    p->addr = r.addr;
    LOGI("new panel at address %s", r.addr);
  }
  p->reading = r;
  p->updated = Clock::now();
  p->updated_unix_ms = unix_ms();
  p->readings++;
  for (const auto &n : nodes_) {
    if (n.addr == p->addr) {
      p->barcode = n.long_address;
      p->label = n.cca_label;
      break;
    }
  }
  // A peak saved before a restart comes back once the panel has a barcode,
  // as the firmware restores its peaks; keyed by barcode, not address (#74).
  if (!p->barcode.empty()) {
    auto saved = saved_peaks_.find(p->barcode);
    if (saved != saved_peaks_.end()) {
      p->peak_power = std::max<float>(p->peak_power, static_cast<float>(saved->second));
      saved_peaks_.erase(saved);
    }
  }
  if (r.power_out > p->peak_power) p->peak_power = r.power_out;
  LOGD("%s: %.1f W out, %.2f V in, %.1f C", r.addr, r.power_out, r.voltage_in, r.temperature);
}

void Monitor::on_frame27_(const std::string &hex) {
  stats_.frame27++;
  proto::Frame27Header header;
  bool changed = false;
  bool ok = proto::walk_frame27(hex, proto::COMMAND_PAYLOAD_OFFSET, header,
                                [&](const char *long_addr, const char *addr) {
    std::string a(addr), l(long_addr);
    auto res = identity::apply_frame27_entry(
        nodes_, parked_cca_, a, l, max_nodes_,
        [](const std::string &na, const std::string &nl) {
          Node n;
          n.addr = na;
          n.long_address = nl;
          n.checksum.assign(1, proto::crc4_char(na.c_str(), na.size()));
          return n;
        },
        [&](const Node &dup) {
          LOGW("node %s has the same barcode as %s; dropping the old address", dup.addr.c_str(), a.c_str());
          for (auto it = panels_.begin(); it != panels_.end(); ++it) {
            if (it->addr == dup.addr) {
              panels_.erase(it);
              break;
            }
          }
        });
    if (res.table_full) LOGW("node table full (%zu); %s not added", nodes_.size(), a.c_str());
    if (res.created) LOGI("node %s is barcode %s", a.c_str(), l.c_str());
    if (res.renumbered) LOGW("address %s now belongs to barcode %s (gateway renumbered)", a.c_str(), l.c_str());
    if (res.table_changed) changed = true;
    if (Panel *p = find_panel_(a)) p->barcode = l;
  });
  if (!ok) {
    LOGW("Frame 27 too short for its header");
    return;
  }
  LOGD("Frame 27: entries %d..%d", header.starting_index, header.starting_index + header.num_entries - 1);
  if (changed) {
    dirty_ = true;
    // Same 10 s rate limit as the firmware: discovery arrives as a burst.
    if (Clock::now() - last_save_ > std::chrono::seconds(10)) save_locked_();
  }
}

std::vector<Panel> Monitor::panels() const {
  std::lock_guard<std::mutex> lock(mu_);
  return panels_;
}

std::vector<Node> Monitor::nodes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return nodes_;
}

BusStats Monitor::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

double Monitor::energy_kwh() const {
  std::lock_guard<std::mutex> lock(mu_);
  return energy_kwh_;
}

double Monitor::today_kwh() const {
  std::lock_guard<std::mutex> lock(mu_);
  return today_kwh_;
}

double Monitor::today_in_kwh() const {
  std::lock_guard<std::mutex> lock(mu_);
  return today_in_kwh_;
}

std::vector<std::pair<std::string, double>> Monitor::daily_history() const {
  std::lock_guard<std::mutex> lock(mu_);
  return history_;
}

float Monitor::total_power_locked_(bool input) const {
  if (night_locked_()) return 0;
  auto now = Clock::now();
  float total = 0;
  for (const auto &p : panels_)
    if (now - p.updated < online_window_) total += input ? p.reading.power_in : p.reading.power_out;
  return total;
}

float Monitor::total_power_in() const {
  std::lock_guard<std::mutex> lock(mu_);
  return total_power_locked_(true);
}

double Monitor::energy_in_kwh() const {
  std::lock_guard<std::mutex> lock(mu_);
  return energy_in_kwh_;
}

Clock::duration Monitor::online_window() const {
  std::lock_guard<std::mutex> lock(mu_);
  return online_window_;
}

void Monitor::set_stale_timeout_min(int minutes) {
  std::lock_guard<std::mutex> lock(mu_);
  online_window_ = minutes > 0 ? Clock::duration(std::chrono::minutes(minutes)) : Clock::duration::max() / 2;
}

bool Monitor::night_locked_() const {
  return last_frame_ != Clock::time_point{} && Clock::now() - last_frame_ > night_timeout_;
}

bool Monitor::night_mode() const {
  std::lock_guard<std::mutex> lock(mu_);
  return night_locked_();
}

void Monitor::set_reset_at_midnight(bool on) {
  std::lock_guard<std::mutex> lock(mu_);
  reset_peaks_at_midnight_ = on;
}

void Monitor::set_night_mode_timeout_min(int minutes) {
  std::lock_guard<std::mutex> lock(mu_);
  night_timeout_ = std::chrono::minutes(minutes);
}

float Monitor::total_power() const {
  std::lock_guard<std::mutex> lock(mu_);
  return total_power_locked_();
}

std::vector<StringTotals> Monitor::strings() const {
  std::lock_guard<std::mutex> lock(mu_);
  return strings_locked_();
}

std::vector<StringTotals> Monitor::strings_locked_() const {
  auto now = Clock::now();
  std::map<std::string, const Node *> node_by_addr;
  for (const auto &n : nodes_) node_by_addr[n.addr] = &n;
  std::map<std::string, StringTotals> by_label;
  for (const auto &p : panels_) {
    auto it = node_by_addr.find(p.addr);
    const Node *n = it == node_by_addr.end() ? nullptr : it->second;
    std::string label = n && !n->cca_string_label.empty() ? n->cca_string_label : ALL_PANELS;
    StringTotals &s = by_label[label];
    s.label = label;
    if (s.mppt.empty() && n) s.mppt = n->cca_mppt_label;
    s.total_devices++;
    if (now - p.updated >= online_window_) continue;
    const auto &r = p.reading;
    if (s.active_devices == 0) s.min_efficiency = s.max_efficiency = r.efficiency;
    s.active_devices++;
    s.total_power += r.power_out;
    s.total_current += r.current_in;
    s.avg_voltage_in += r.voltage_in;
    s.avg_voltage_out += r.voltage_out;
    s.avg_temperature += r.temperature;
    s.avg_efficiency += r.efficiency;
    s.min_efficiency = std::min<double>(s.min_efficiency, r.efficiency);
    s.max_efficiency = std::max<double>(s.max_efficiency, r.efficiency);
  }
  std::vector<StringTotals> out;
  for (auto &kv : by_label) {
    StringTotals &s = kv.second;
    if (s.active_devices > 0) {
      s.avg_voltage_in /= s.active_devices;
      s.avg_voltage_out /= s.active_devices;
      s.avg_temperature /= s.active_devices;
      s.avg_efficiency /= s.active_devices;
    }
    double &peak = string_peaks_[s.label];
    peak = std::max(peak, s.total_power);
    s.peak_power = peak;
    out.push_back(s);
  }
  return out;
}

void Monitor::set_power_calibration(float c) {
  std::lock_guard<std::mutex> lock(mu_);
  power_calibration_ = c;
}

void Monitor::reset_peaks() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &p : panels_) p.peak_power = p.reading.power_out;
  saved_peaks_.clear();
  string_peaks_.clear();
  dirty_ = true;
}

void Monitor::reset_nodes() {
  std::lock_guard<std::mutex> lock(mu_);
  nodes_.clear();
  parked_cca_.clear();
  panels_.clear();
  string_peaks_.clear();
  dirty_ = true;
  save_locked_();
  LOGW("node table cleared");
}

int Monitor::apply_layout(const std::vector<LayoutPanel> &panels) {
  std::lock_guard<std::mutex> lock(mu_);
  auto upper = [](std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };
  int matched = 0;
  for (const auto &lp : panels) {
    std::string serial = upper(lp.serial);
    for (auto &n : nodes_) {
      if (n.long_address.size() < 6) continue;
      if (serial.find(upper(n.long_address.substr(n.long_address.size() - 6))) == std::string::npos) continue;
      n.cca_label = lp.label;
      n.cca_string_label = lp.string_label;
      n.cca_mppt_label = lp.mppt;
      n.cca_object_id = lp.object_id;
      if (!lp.channel.empty()) n.cca_channel = lp.channel;
      n.cca_validated = true;
      for (auto &p : panels_)
        if (p.addr == n.addr) p.label = n.cca_label;
      matched++;
      break;
    }
  }
  LOGI("layout: %zu panels, %d matched to nodes", panels.size(), matched);
  if (matched > 0) {
    string_peaks_.clear();
    dirty_ = true;
    save_locked_();
  }
  return matched;
}

bool Monitor::delete_node(const std::string &addr) {
  std::lock_guard<std::mutex> lock(mu_);
  bool found = false;
  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if (it->addr != addr) continue;
    nodes_.erase(it);
    found = true;
    break;
  }
  for (auto it = panels_.begin(); it != panels_.end(); ++it) {
    if (it->addr != addr) continue;
    panels_.erase(it);
    found = true;
    break;
  }
  if (found) {
    dirty_ = true;
    save_locked_();
  }
  return found;
}

Monitor::ImportResult Monitor::import_nodes(const std::vector<Node> &in) {
  std::lock_guard<std::mutex> lock(mu_);
  ImportResult r;

  auto counts = identity::import_backup(
      nodes_, parked_cca_, in, [](Node &n) { n.checksum.assign(1, proto::crc4_char(n.addr.c_str(), n.addr.size())); },
      [](identity::ImportEvent e, const Node &n, const std::string &addr) {
        using E = identity::ImportEvent;
        if (e == E::moved)
          LOGW("import: %s (%s) is at %s now, not %s; placing it there", n.long_address.c_str(), n.cca_label.c_str(),
               addr.c_str(), n.addr.c_str());
        else if (e == E::held)
          LOGW("import: %s (%s) is no longer at %s; holding its labels until it reappears", n.long_address.c_str(),
               n.cca_label.c_str(), n.addr.c_str());
        else if (e == E::no_address)
          LOGW("import: skipping an entry with no address");
        else if (e == E::duplicate_address)
          LOGW("import: skipping a second entry for address %s", n.addr.c_str());
        else
          LOGW("import: %s has the same barcode as %s; merged", n.addr.c_str(), addr.c_str());
      });
  r.remapped = counts.remapped;
  r.parked = counts.parked;
  r.imported = nodes_.size();
  string_peaks_.clear();  // the strings themselves may have just changed

  // Panels already on screen pick up their barcode and label now, not at
  // their next reading.
  for (auto &p : panels_) {
    p.barcode.clear();
    p.label.clear();
    for (const auto &n : nodes_) {
      if (n.addr == p.addr) {
        p.barcode = n.long_address;
        p.label = n.cca_label;
        break;
      }
    }
  }
  if (r.remapped || r.parked)
    LOGW("import: backup predates a renumbering; %d moved to current addresses, %d held", r.remapped, r.parked);
  LOGI("import: %zu nodes", r.imported);
  dirty_ = true;
  save_locked_();
  return r;
}

void Monitor::tick() {
  std::lock_guard<std::mutex> lock(mu_);
  auto now = Clock::now();
  // Day rollover at local midnight: close out yesterday, reset the peaks.
  std::string date = local_date();
  if (today_.empty()) {
    today_ = date;
  } else if (date != today_) {
    history_.emplace_back(today_, today_kwh_);
    if (history_.size() > HISTORY_DAYS) history_.erase(history_.begin());
    today_ = date;
    today_kwh_ = 0;
    today_in_kwh_ = 0;
    // Peaks are all-time unless reset_at_midnight, as on the ESP32.
    if (reset_peaks_at_midnight_) {
      for (auto &p : panels_) p.peak_power = 0;
      saved_peaks_.clear();
      string_peaks_.clear();
    }
    dirty_ = true;
  }
  if (last_tick_ != Clock::time_point{}) {
    double hours = std::chrono::duration<double>(now - last_tick_).count() / 3600.0;
    // A long gap (suspend, a stalled loop) is not evidence of production.
    if (hours < 0.1) {
      double kwh = total_power_locked_() / 1000.0 * hours;
      double kwh_in = total_power_locked_(true) / 1000.0 * hours;
      energy_in_kwh_ += kwh_in;
      today_in_kwh_ += kwh_in;
      energy_kwh_ += kwh;
      today_kwh_ += kwh;
    }
  }
  last_tick_ = now;
  strings_locked_();  // keeps the string peaks current between page loads
  if (now - last_save_ > std::chrono::minutes(5)) {
    dirty_ = true;
    save_locked_();
  } else if (dirty_ && now - last_save_ > std::chrono::seconds(10)) {
    save_locked_();
  }
}

bool Monitor::save() {
  std::lock_guard<std::mutex> lock(mu_);
  dirty_ = true;
  return save_locked_();
}

bool Monitor::save_locked_() {
  last_save_ = Clock::now();
  if (!dirty_) return true;
  json nodes = json::array();
  for (const auto &n : nodes_) {
    nodes.push_back({{"addr", n.addr},
                     {"long_address", n.long_address},
                     {"checksum", n.checksum},
                     {"sensor_index", n.sensor_index},
                     {"cca_label", n.cca_label},
                     {"cca_string_label", n.cca_string_label},
                     {"cca_mppt_label", n.cca_mppt_label},
                     {"cca_channel", n.cca_channel},
                     {"cca_object_id", n.cca_object_id},
                     {"cca_validated", n.cca_validated}});
  }
  json history = json::array();
  for (const auto &d : history_) history.push_back({d.first, d.second});
  json peaks = json::object();
  for (const auto &kv : saved_peaks_) peaks[kv.first] = kv.second;  // not seen since start
  for (const auto &p : panels_)
    if (!p.barcode.empty()) peaks[p.barcode] = p.peak_power;
  json j = {{"version", 1},     {"nodes", nodes},           {"energy_kwh", energy_kwh_}, {"energy_in_kwh", energy_in_kwh_}, {"today_in_kwh", today_in_kwh_}, {"peaks", peaks},
            {"today", today_},  {"today_kwh", today_kwh_}, {"daily_kwh", history}};
  if (!write_file_atomic(state_path_, j.dump(1) + "\n")) return false;
  dirty_ = false;
  return true;
}

bool Monitor::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(state_path_);
  if (!in) return false;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    LOGW("state: %s is not valid JSON, starting empty", state_path_.c_str());
    return false;
  }
  energy_kwh_ = j.value("energy_kwh", 0.0);
  energy_in_kwh_ = j.value("energy_in_kwh", 0.0);
  today_in_kwh_ = j.value("today_in_kwh", 0.0);
  saved_peaks_.clear();
  const json peaks = j.value("peaks", json::object());
  for (const auto &kv : peaks.items())
    if (kv.value().is_number()) saved_peaks_[kv.key()] = kv.value().get<double>();
  today_ = j.value("today", "");
  today_kwh_ = j.value("today_kwh", 0.0);
  history_.clear();
  for (const auto &d : j.value("daily_kwh", json::array())) {
    if (d.is_array() && d.size() == 2 && d[0].is_string() && d[1].is_number())
      history_.emplace_back(d[0].get<std::string>(), d[1].get<double>());
  }
  nodes_.clear();
  for (const auto &e : j.value("nodes", json::array())) {
    if (!e.is_object()) continue;
    Node n;
    n.addr = e.value("addr", "");
    n.long_address = e.value("long_address", "");
    if (n.addr.size() != 4) continue;
    n.checksum = e.value("checksum", "");
    n.sensor_index = e.value("sensor_index", -1);
    n.cca_label = e.value("cca_label", "");
    n.cca_string_label = e.value("cca_string_label", "");
    n.cca_mppt_label = e.value("cca_mppt_label", "");
    n.cca_channel = e.value("cca_channel", "");
    n.cca_object_id = e.value("cca_object_id", "");
    n.cca_validated = e.value("cca_validated", false);
    nodes_.push_back(std::move(n));
  }
  LOGI("state: %zu nodes, %.3f kWh", nodes_.size(), energy_kwh_);
  return true;
}

}  // namespace tigod
