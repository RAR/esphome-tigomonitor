#include "monitor.h"

#include "fsutil.h"
#include "log.h"
#include "tigo_node_identity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// Today as YYYYMMDD in local time, for counters that restart at midnight.
// Asked for each power report (20-40 a second), so worked out once a second.
static int local_day() {
  static thread_local time_t last = 0;
  static thread_local int day = 0;
  time_t t = time(nullptr);
  if (t != last) {
    last = t;
    tm lt{};
    localtime_r(&t, &lt);
    day = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
  }
  return day;
}

static int64_t unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Monitor::Monitor(std::string state_path, float power_calibration, size_t max_nodes)
    : state_path_(std::move(state_path)), power_calibration_(power_calibration), max_nodes_(max_nodes),
      writer_([this] { writer_loop_(); }) {}

Monitor::~Monitor() {
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    stop_writer_ = true;
  }
  write_cv_.notify_all();
  writer_.join();
}

void Monitor::writer_loop_() {
  std::unique_lock<std::mutex> lock(write_mu_);
  for (;;) {
    write_cv_.wait(lock, [this] { return has_pending_ || stop_writer_; });
    if (!has_pending_) return;  // stopping, and nothing left to write
    std::string text = std::move(pending_);
    has_pending_ = false;
    writing_ = true;
    lock.unlock();
    if (!write_file_atomic(state_path_, text)) write_failed_ = true;  // the next save marks it dirty again
    lock.lock();
    writing_ = false;
    write_cv_.notify_all();
  }
}

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
      // The TAP it came through: a reply's address is the TAP's RS485 id
      // with 0x8000 set.
      if (!proto::walk_power_packets(hex, [this, tap = static_cast<uint16_t>(
                                                     std::strtoul(hex.substr(0, 4).c_str(), nullptr, 16) & 0x7FFF)](
                                                const std::string &type, const std::string &packet) {
            if (type == "31") on_power_packet_(packet, tap);
            else if (type == "09") on_netdiag_(packet);
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

void Monitor::on_power_packet_(const std::string &packet, uint16_t tap) {
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
  int64_t now_s = p->updated_unix_ms / 1000;
  links_.on_report(r.addr, tap, r.rssi, static_cast<uint16_t>(std::strtoul(r.slot_counter, nullptr, 16)), now_s,
                   local_day());
  for (auto &n : nodes_) {
    if (n.addr == p->addr) {
      if (now_s - n.last_heard_unix >= SEEN_SAVE_EVERY_S) dirty_ = true;
      n.last_heard_unix = now_s;
      if (n.known) {
        n.known = false;
        dirty_ = true;
        LOGI("%s (%s) is reporting: no longer marked known", n.cca_label.empty() ? n.addr.c_str() : n.cca_label.c_str(),
             n.addr.c_str());
      }
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

// A NetDiag join: the optimizer named by its MAC, else by the packet's
// address, and the parent it joined through; see decode_netdiag_packet.
void Monitor::on_netdiag_(const std::string &packet) {
  proto::NetDiagJoin j;
  if (!proto::decode_netdiag_packet(packet, j)) return;
  std::string addr = j.addr;
  for (const auto &n : nodes_)
    if (n.long_address == j.node_mac) addr = n.addr;
  int64_t now_s = unix_ms() / 1000;
  links_.on_join(addr, now_s, j.parent, j.nwid);
  join_log_.push_back({addr, now_s, j.parent, j.nwid});
  while (!join_log_.empty() && join_log_.front().unix_s < now_s - JOIN_DAYS * 86400) join_log_.erase(join_log_.begin());
  dirty_ = true;  // kept in state.json: the next join may be tomorrow's sunrise
  LOGI("%s joined (NetDiag)", addr.c_str());
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
    note_barcode_(l, a);
    int64_t now_s = static_cast<int64_t>(time(nullptr));
    for (auto &n : nodes_)
      if (n.addr == a) {
        if (now_s - n.last_roster_unix >= SEEN_SAVE_EVERY_S) dirty_ = true;
        n.last_roster_unix = now_s;
      }
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

void Monitor::note_barcode_(const std::string &barcode, const std::string &addr) {
  if (barcode.empty() || !seen_barcodes_.insert(barcode).second) return;
  dirty_ = true;
  int64_t now = static_cast<int64_t>(time(nullptr));
  if (seen_since_ == 0) {
    seen_since_ = now;
    return;
  }
  if (now - seen_since_ < NEW_AFTER_S) return;
  first_heard_[barcode] = now;
  LOGW("new optimizer heard: %s at node %s; place it in the layout (Topology, Edit layout)%s", barcode.c_str(), addr.c_str(),
       new_note_.c_str());
}

std::map<std::string, int64_t> Monitor::first_heard() const {
  std::lock_guard<std::mutex> lock(mu_);
  return first_heard_;
}

int Monitor::missing_optimizers(int64_t now_unix) const {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t newest_roster = 0;
  for (const auto &n : nodes_) newest_roster = std::max(newest_roster, n.last_roster_unix);
  int count = 0;
  for (const auto &n : nodes_) {
    if (n.long_address.empty() || n.known) continue;
    bool silent = n.last_heard_unix > 0 && now_unix - n.last_heard_unix >= MISSING_AFTER_S;
    bool off_roster = n.last_roster_unix > 0 && newest_roster - n.last_roster_unix >= OFF_ROSTER_AFTER_S;
    if (silent || off_roster) count++;
  }
  return count;
}

bool Monitor::set_known(const std::string &barcode, bool known) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &n : nodes_) {
    if (n.long_address != barcode) continue;
    if (n.known != known) {
      n.known = known;
      dirty_ = true;
      LOGI("%s (%s) %s", n.cca_label.empty() ? n.addr.c_str() : n.cca_label.c_str(), n.addr.c_str(),
           known ? "marked known: not reporting, and not counted as missing" : "no longer marked known");
    }
    return true;
  }
  return false;
}

bool Monitor::set_tap_name(const std::string &tap, const std::string &name) {
  if (tap.size() != 4 || tap.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  size_t a = name.find_first_not_of(" \t"), b = name.find_last_not_of(" \t");
  std::string trimmed = a == std::string::npos ? "" : name.substr(a, b - a + 1);
  if (trimmed.size() > 32) return false;
  std::string key = tap;
  for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  std::lock_guard<std::mutex> lock(mu_);
  if (trimmed.empty()) tap_names_.erase(key);
  else tap_names_[key] = trimmed;
  dirty_ = true;
  save_locked_();
  LOGI("TAP %s %s", key.c_str(), trimmed.empty() ? "has no name now" : ("is named " + trimmed).c_str());
  return true;
}

std::map<std::string, std::map<std::string, std::pair<uint32_t, uint32_t>>> Monitor::loss_days() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto days = loss_book_.days();
  // Today's as they stand now, not as last saved.
  if (!today_.empty())
    for (const auto &kv : links_.links())
      if (kv.second.day == local_day() && kv.second.reports_today)
        days[today_][kv.first] = {kv.second.reports_today, kv.second.lost_today};
  return days;
}

std::vector<esphome::tigo_monitor::mesh_history::JoinEntry> Monitor::join_log() const {
  std::lock_guard<std::mutex> lock(mu_);
  return join_log_;
}

std::map<std::string, std::string> Monitor::tap_names() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tap_names_;
}

int Monitor::new_optimizers(int64_t now_unix) const {
  std::lock_guard<std::mutex> lock(mu_);
  int n = 0;
  for (const auto &node : nodes_) {
    auto it = first_heard_.find(node.long_address);
    if (it != first_heard_.end() && now_unix - it->second < NEW_FOR_S && node.cca_string_label.empty()) n++;
  }
  return n;
}

std::vector<int64_t> Monitor::expected_last_heard() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<int64_t> out;
  for (const auto &n : nodes_)
    if (!n.cca_string_label.empty() && !n.known) out.push_back(n.last_heard_unix);
  return out;
}

int64_t Monitor::last_report_unix() const {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t newest = 0;
  for (const auto &p : panels_) newest = std::max(newest, p.updated_unix_ms / 1000);
  return newest;
}

std::vector<Node> Monitor::nodes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return nodes_;
}

esphome::tigo_monitor::link::LinkStats Monitor::links() const {
  esphome::tigo_monitor::link::LinkStats copy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    copy = links_;
  }
  // An optimizer not heard since midnight reads today's zeros, not
  // yesterday's counts.
  copy.roll(local_day());
  return copy;
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
  links_.erase(addr);
  if (found) {
    dirty_ = true;
    save_locked_();
  }
  return found;
}

std::vector<esphome::tigo_monitor::layout::Assignment> Monitor::resolve_layout(
    const std::vector<LayoutPanel> &panels) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto upper = [](std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };
  std::vector<esphome::tigo_monitor::layout::Assignment> out;
  for (const auto &lp : panels) {
    std::string serial = upper(lp.serial);
    for (const auto &n : nodes_) {
      if (n.long_address.size() < 6) continue;
      if (serial.find(upper(n.long_address.substr(n.long_address.size() - 6))) == std::string::npos) continue;
      out.push_back({n.long_address, lp.label, lp.string_label, lp.mppt});
      break;
    }
  }
  return out;
}

int Monitor::set_assignments(const std::vector<esphome::tigo_monitor::layout::Assignment> &as,
                             bool clear_unlisted) {
  std::lock_guard<std::mutex> lock(mu_);
  std::map<std::string, const esphome::tigo_monitor::layout::Assignment *> by_bc;
  for (const auto &a : as) by_bc[a.barcode] = &a;
  int matched = 0;
  for (auto &n : nodes_) {
    if (n.long_address.empty()) continue;
    auto it = by_bc.find(n.long_address);
    if (it != by_bc.end()) {
      n.cca_label = it->second->label;
      n.cca_string_label = it->second->string_label;
      n.cca_mppt_label = it->second->mppt_label;
      matched++;
    } else if (clear_unlisted) {
      n.cca_label.clear();
      n.cca_string_label.clear();
      n.cca_mppt_label.clear();
    } else {
      continue;
    }
    for (auto &p : panels_)
      if (p.addr == n.addr) p.label = n.cca_label;
  }
  string_peaks_.clear();
  dirty_ = true;
  save_locked_();
  return matched;
}

std::vector<esphome::tigo_monitor::layout::Assignment> Monitor::current_assignments() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<esphome::tigo_monitor::layout::Assignment> out;
  for (const auto &n : nodes_)
    if (!n.long_address.empty())
      out.push_back({n.long_address, n.cca_label, n.cca_string_label, n.cca_mppt_label});
  return out;
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
  // Not until the clock is set: a board that boots at 1970 and gets NTP a
  // minute later would otherwise file today's energy under two dates.
  bool clock_set = time(nullptr) > 1577836800 /* 2020 */;
  std::string date = clock_set ? local_date() : today_;
  if (clock_set && today_.empty()) {
    today_ = date;
  } else if (clock_set && date != today_) {
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

// Waits for the write, for the save at shutdown.
bool Monitor::save() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    dirty_ = true;
    save_locked_();
  }
  std::unique_lock<std::mutex> lock(write_mu_);
  write_cv_.wait(lock, [this] { return !has_pending_ && !writing_; });
  return !write_failed_;
}

json Monitor::loss_days_json_locked_() const {
  json out = json::object();
  for (const auto &day : loss_book_.days()) {
    json d = json::object();
    for (const auto &kv : day.second) d[kv.first] = {kv.second.first, kv.second.second};
    out[day.first] = d;
  }
  return out;
}

json Monitor::join_log_json_locked_() const {
  json out = json::array();
  for (const auto &e : join_log_) out.push_back({e.addr, e.unix_s, e.parent, e.nwid});
  return out;
}

bool Monitor::save_locked_() {
  last_save_ = Clock::now();
  if (write_failed_.exchange(false)) dirty_ = true;
  if (!dirty_) return true;
  // Today's reports and losses so far: the last noted before midnight stand
  // as the day's.
  if (!today_.empty()) {
    for (const auto &kv : links_.links())
      if (kv.second.day == local_day() && kv.second.reports_today)
        loss_book_.note(today_, kv.first, kv.second.reports_today, kv.second.lost_today);
    loss_book_.prune(LOSS_DAYS);
  }
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
                     {"cca_validated", n.cca_validated},
                     {"last_heard_unix", n.last_heard_unix},
                     {"last_roster_unix", n.last_roster_unix},
                     {"known", n.known}});
  }
  json history = json::array();
  for (const auto &d : history_) history.push_back({d.first, d.second});
  json peaks = json::object();
  for (const auto &kv : saved_peaks_) peaks[kv.first] = kv.second;  // not seen since start
  for (const auto &p : panels_)
    if (!p.barcode.empty()) peaks[p.barcode] = p.peak_power;
  json first = json::object();
  for (const auto &kv : first_heard_) first[kv.first] = kv.second;
  // Where each optimizer last joined the mesh, and when.
  json joins = json::array();
  for (const auto &kv : links_.links())
    if (kv.second.last_join_s)
      joins.push_back({{"addr", kv.first}, {"parent", kv.second.parent}, {"nwid", kv.second.nwid}, {"unix", kv.second.last_join_s}});
  // And a day of each one's signal, which the mesh map draws.
  json rssi = json::array();
  const int64_t now_s = unix_ms() / 1000;
  for (const auto &kv : links_.links()) {
    std::string b = links_.rssi_dump(kv.first, now_s);
    if (!b.empty()) rssi.push_back({{"addr", kv.first}, {"to", now_s / esphome::tigo_monitor::link::BUCKET_S}, {"b", b}});
  }
  json j = {{"version", 1},     {"nodes", nodes},           {"energy_kwh", energy_kwh_}, {"energy_in_kwh", energy_in_kwh_}, {"today_in_kwh", today_in_kwh_}, {"peaks", peaks},
            {"today", today_},  {"today_kwh", today_kwh_}, {"daily_kwh", history},
            {"seen_barcodes", seen_barcodes_}, {"seen_since", seen_since_}, {"first_heard", first}, {"joins", joins}, {"rssi_24h", rssi},
            {"tap_names", tap_names_}, {"loss_days", loss_days_json_locked_()}, {"join_log", join_log_json_locked_()}};
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    pending_ = j.dump(1) + "\n";
    has_pending_ = true;
  }
  write_cv_.notify_all();
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
  for (const auto &e : j.value("joins", json::array()))
    if (e.is_object() && !e.value("addr", "").empty())
      links_.on_join(e.value("addr", ""), e.value("unix", static_cast<int64_t>(0)), e.value("parent", ""), e.value("nwid", ""));
  for (const auto &e : j.value("rssi_24h", json::array()))
    if (e.is_object() && !e.value("addr", "").empty() && e.value("to", 0) > 0)
      links_.rssi_restore(e.value("addr", ""), e.value("to", 0), e.value("b", ""));
  loss_book_ = {};
  const json losses = j.value("loss_days", json::object());
  for (const auto &day : losses.items())
    if (day.value().is_object())
      for (const auto &kv : day.value().items())
        if (kv.value().is_array() && kv.value().size() == 2 && kv.value()[0].is_number_unsigned() && kv.value()[1].is_number_unsigned())
          loss_book_.restore(day.key(), kv.key(), kv.value()[0].get<uint32_t>(), kv.value()[1].get<uint32_t>());
  join_log_.clear();
  for (const auto &e : j.value("join_log", json::array()))
    if (e.is_array() && e.size() == 4 && e[0].is_string() && e[1].is_number_integer() && e[2].is_string() && e[3].is_string())
      join_log_.push_back({e[0].get<std::string>(), e[1].get<int64_t>(), e[2].get<std::string>(), e[3].get<std::string>()});
  tap_names_.clear();
  const json names = j.value("tap_names", json::object());
  for (const auto &kv : names.items())
    if (kv.value().is_string() && kv.key().size() == 4) tap_names_[kv.key()] = kv.value().get<std::string>();
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
    n.last_heard_unix = e.value("last_heard_unix", static_cast<int64_t>(0));
    n.last_roster_unix = e.value("last_roster_unix", static_cast<int64_t>(0));
    n.known = e.value("known", false);
    nodes_.push_back(std::move(n));
  }
  // Before seen_barcodes was kept, the node table was all it had heard, and
  // long enough ago that a barcode heard now is news.
  seen_barcodes_.clear();
  first_heard_.clear();
  if (j.contains("seen_barcodes") && j["seen_barcodes"].is_array()) {
    for (const auto &b : j["seen_barcodes"])
      if (b.is_string()) seen_barcodes_.insert(b.get<std::string>());
    seen_since_ = j.value("seen_since", static_cast<int64_t>(0));
    const json first = j.value("first_heard", json::object());  // items() of a temporary would dangle
    for (const auto &kv : first.items())
      if (kv.value().is_number_integer()) first_heard_[kv.key()] = kv.value().get<int64_t>();
  } else {
    for (const auto &n : nodes_)
      if (!n.long_address.empty()) seen_barcodes_.insert(n.long_address);
    seen_since_ = seen_barcodes_.empty() ? 0 : 1;
  }
  LOGI("state: %zu nodes, %.3f kWh", nodes_.size(), energy_kwh_);
  return true;
}

}  // namespace tigod
