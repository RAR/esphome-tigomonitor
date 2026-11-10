#include "bus_log.h"

#include "fsutil.h"
#include "log.h"
#include "mesh_history.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace tigod {

using nlohmann::json;
using esphome::tigo_monitor::master::BusStats;
using esphome::tigo_monitor::master::Exchange;
using esphome::tigo_monitor::master::Frame;
using esphome::tigo_monitor::master::VersionBook;

static int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

using esphome::tigo_monitor::master::TapRadio;
using esphome::tigo_monitor::master::SmartSeen;
using esphome::tigo_monitor::master::MpptSeen;

static std::string hex4(unsigned v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%04X", v & 0xFFFF);
  return b;
}

static json entry_json(const VersionBook::Entry &e) {
  return {{"addr", hex4(e.addr)},     {"model", e.model},       {"version", e.version},
          {"previous", e.previous},   {"since_unix_ms", e.since_ms}, {"last_unix_ms", e.last_ms}};
}

void BusLog::load_versions_() {
  if (versions_path_.empty()) return;
  std::ifstream in(versions_path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (!j.is_object()) {
    LOGW("bus log: %s is unreadable; starting with no versions", versions_path_.c_str());
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (const char *kind : {"taps", "optimizers"})
    for (const auto &o : j.value(kind, json::array())) {
      if (!o.is_object()) continue;
      VersionBook::Entry e;
      e.tap = std::string(kind) == "taps";
      e.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
      e.model = o.value("model", std::string());
      e.version = o.value("version", std::string());
      e.previous = o.value("previous", std::string());
      e.since_ms = o.value("since_unix_ms", int64_t{0});
      e.last_ms = o.value("last_unix_ms", int64_t{0});
      if (!e.version.empty()) versions_.restore(e);
    }
  for (const auto &o : j.value("radios", json::array())) {
    if (!o.is_object()) continue;
    TapRadio r;
    r.tap = static_cast<uint16_t>(std::strtoul(o.value("tap", std::string("0")).c_str(), nullptr, 16));
    r.channel = o.value("channel", 0);
    r.slt = o.value("slt", 0);
    r.slm = o.value("slm", 0);
    if (r.tap) radios_[r.tap] = {r, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("ids", json::array())) {
    if (!o.is_object()) continue;
    auto tap = static_cast<uint16_t>(std::strtoul(o.value("tap", std::string("0")).c_str(), nullptr, 16));
    std::string mac = o.value("mac", std::string());
    if (tap && mac.size() == 16) tap_ids_[tap] = {mac, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("settings", json::array())) {
    if (!o.is_object()) continue;
    SmartSeen m;
    m.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
    m.settings = o.value("settings", std::string());
    m.stored = o.value("stored", false);
    if (!m.settings.empty()) smarts_[m.addr] = {m, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("changes", json::array())) {  // newest first
    if (!o.is_object()) continue;
    changes_.push_front({o.value("unix_ms", int64_t{0}),
                         static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16)),
                         o.value("what", std::string()), o.value("before", std::string()), o.value("after", std::string())});
  }
  while (changes_.size() > KEEP_CHANGES) changes_.pop_front();
  for (const auto &o : j.value("mppt", json::array())) {
    if (!o.is_object()) continue;
    MpptSeen m;
    m.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
    m.params = o.value("params", std::string());
    m.answer = o.value("answer", std::string());
    if (!m.params.empty()) mppts_[m.addr] = {m, o.value("heard_unix_ms", int64_t{0})};
  }
}

void BusLog::load() {
  load_versions_();
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (!j.is_object()) {
    LOGW("bus log: %s is unreadable; starting with no alarm", path_.c_str());
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  alarm_count_ = j.value("count", uint64_t{0});
  first_alarm_ms_ = j.value("first_unix_ms", int64_t{0});
  for (const auto &e : j.value("recent", json::array()))
    if (e.is_object()) alarms_.push_back({e.value("unix_ms", int64_t{0}), e.value("what", std::string())});
  while (alarms_.size() > KEEP_ALARMS) alarms_.pop_front();
  if (alarm_count_)
    LOGW("bus: the TAP-reconfiguration alarm is still on from before the restart (%llu frames); clear it on the Bus log page",
         static_cast<unsigned long long>(alarm_count_));
}

// An optimizer whose settings, MPPT parameters or firmware changed: a CCA
// never drifts them, so each is news. The last KEEP_CHANGES are kept.
void BusLog::change_(int64_t now, uint16_t addr, const char *what, const std::string &before, const std::string &after) {
  LOGW("bus: optimizer %s's %s changed: %s, was %s", hex4(addr).c_str(), what, after.c_str(), before.c_str());
  changes_.push_back({now, addr, what, before, after});
  while (changes_.size() > KEEP_CHANGES) changes_.pop_front();
  versions_dirty_ = true;
}

int BusLog::changes_since(int64_t since_ms) const {
  std::lock_guard<std::mutex> lock(mu_);
  int n = 0;
  for (const auto &c : changes_) n += c.unix_ms >= since_ms;
  return n;
}

void BusLog::on_frame_(const Frame &f, bool ours, int64_t now) {
  std::string what = watch_.on_frame(f, ours, now);
  for (const auto &r : watch_.take_radios()) {
    auto it = radios_.find(r.tap);
    bool changed = it == radios_.end() || it->second.first.channel != r.channel || it->second.first.slt != r.slt ||
                   it->second.first.slm != r.slm;
    if (changed)
      LOGI("bus: TAP %s radio: channel %d, SLT %d, SLm %d", hex4(r.tap).c_str(), r.channel, r.slt, r.slm);
    radios_[r.tap] = {r, now};
    versions_dirty_ = true;
  }
  for (const auto &t : watch_.take_tap_ids()) {
    auto it = tap_ids_.find(t.tap);
    if (it == tap_ids_.end() || it->second.first != t.mac) LOGI("bus: TAP %s is %s", hex4(t.tap).c_str(), t.mac.c_str());
    tap_ids_[t.tap] = {t.mac, now};
    versions_dirty_ = true;
  }
  for (const auto &m : watch_.take_mppts()) {
    auto it = mppts_.find(m.addr);
    if (it == mppts_.end() || it->second.first.params != m.params)
      LOGI("bus: optimizer %s MPPT parameters: %s", hex4(m.addr).c_str(), m.params.c_str());
    if (it != mppts_.end() && it->second.first.params != m.params) change_(now, m.addr, "MPPT", it->second.first.params, m.params);
    mppts_[m.addr] = {m, now};
    versions_dirty_ = true;
  }
  for (const auto &m : watch_.take_smarts()) {
    auto it = smarts_.find(m.addr);
    if (it == smarts_.end() || it->second.first.settings != m.settings)
      LOGI("bus: optimizer %s settings: %s (%s)", hex4(m.addr).c_str(), m.settings.c_str(),
           m.stored ? "stored" : "not stored");
    if (it != smarts_.end() && esphome::tigo_monitor::mesh_history::settings_changed(it->second.first.settings, m.settings))
      change_(now, m.addr, "settings", it->second.first.settings, m.settings);
    smarts_[m.addr] = {m, now};
    versions_dirty_ = true;
  }
  for (const auto &v : watch_.take_versions()) {
    VersionBook::Result r = versions_.record(v, now);
    versions_dirty_ = true;
    if (r == VersionBook::changed) {
      LOGW("bus: %s %s's firmware changed: %s, was %s", v.tap ? "TAP" : "optimizer", hex4(v.addr).c_str(),
           v.version.c_str(), versions_.find(v.tap, v.addr)->previous.c_str());
      if (!v.tap) change_(now, v.addr, "firmware", versions_.find(v.tap, v.addr)->previous, v.version);
    }
    else if (r == VersionBook::first)
      LOGI("bus: %s %s runs %s %s", v.tap ? "TAP" : "optimizer", hex4(v.addr).c_str(), v.model.c_str(),
           v.version.c_str());
  }
  if (what.empty()) return;
  if (!alarm_count_) first_alarm_ms_ = now;
  alarm_count_++;
  alarms_.push_back({now, what});
  if (alarms_.size() > KEEP_ALARMS) alarms_.pop_front();
  dirty_ = true;
  // A scan asks for its results every second for a minute: say each kind
  // once a minute, not each frame.
  if (what != last_logged_ || now - last_logged_ms_ >= 60000) {
    LOGE("bus: another device sent the TAP a reconfiguration: %s (to %s). Is a CCA on the bus?", what.c_str(),
         hex4(f.dest).c_str());
    last_logged_ = what;
    last_logged_ms_ = now;
  }
}

void BusLog::feed_rx(const char *data, size_t n) {
  int64_t now = unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < n; i++) {
    rx_.feed(data[i], [&](const std::string &body) {
      Frame f;
      if (!esphome::tigo_monitor::master::decode_frame(body, f)) return;
      // An adapter that hears itself gives our own requests back.
      if (!esphome::tigo_monitor::master::is_reply(f))
        for (auto it = sent_.begin(); it != sent_.end(); ++it)
          if (*it == body) {
            sent_.erase(it);
            return;
          }
      on_frame_(f, false, now);
    });
  }
}

void BusLog::feed_tx(const char *data, size_t n, uint32_t late_ms) {
  int64_t now = unix_ms_now() - late_ms;
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < n; i++) {
    tx_.feed(data[i], [&](const std::string &body) {
      Frame f;
      if (!esphome::tigo_monitor::master::decode_frame(body, f)) return;
      sent_.push_back(body);
      if (sent_.size() > 4) sent_.pop_front();
      on_frame_(f, true, now);
    });
  }
}

bool BusLog::needs_save() const {
  std::lock_guard<std::mutex> lock(mu_);
  return dirty_ || versions_dirty_;
}

json BusLog::versions_json() const {
  std::lock_guard<std::mutex> lock(mu_);
  json taps = json::array(), optimizers = json::array();
  for (const auto &kv : versions_.entries()) (kv.second.tap ? taps : optimizers).push_back(entry_json(kv.second));
  json radios = json::array();
  for (const auto &kv : radios_)
    radios.push_back({{"tap", hex4(kv.first)},
                      {"channel", kv.second.first.channel},
                      {"slt", kv.second.first.slt},
                      {"slm", kv.second.first.slm},
                      {"heard_unix_ms", kv.second.second}});
  json ids = json::array();
  for (const auto &kv : tap_ids_)
    ids.push_back({{"tap", hex4(kv.first)}, {"mac", kv.second.first}, {"heard_unix_ms", kv.second.second}});
  json settings = json::array();
  for (const auto &kv : smarts_)
    settings.push_back({{"addr", hex4(kv.first)},
                        {"settings", kv.second.first.settings},
                        {"stored", kv.second.first.stored},
                        {"heard_unix_ms", kv.second.second}});
  json mppt = json::array();
  for (const auto &kv : mppts_)
    mppt.push_back({{"addr", hex4(kv.first)},
                    {"params", kv.second.first.params},
                    {"answer", kv.second.first.answer},
                    {"heard_unix_ms", kv.second.second}});
  json changes = json::array();
  for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
    changes.push_back({{"unix_ms", it->unix_ms}, {"addr", hex4(it->addr)}, {"what", it->what},
                       {"before", it->before}, {"after", it->after}});
  return {{"taps", taps}, {"optimizers", optimizers}, {"radios", radios}, {"ids", ids}, {"settings", settings}, {"mppt", mppt},
          {"changes", changes}};
}

void BusLog::save_if_changed() {
  if (!versions_path_.empty()) {
    bool write;
    {
      std::lock_guard<std::mutex> lock(mu_);
      write = versions_dirty_;
      versions_dirty_ = false;
    }
    if (write && !write_file_atomic(versions_path_, versions_json().dump(1) + "\n", 0644))
      LOGW("bus log: cannot write %s", versions_path_.c_str());
  }
  json j;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dirty_) return;
    dirty_ = false;
    json recent = json::array();
    for (const Alarm &a : alarms_) recent.push_back({{"unix_ms", a.unix_ms}, {"what", a.what}});
    j = {{"count", alarm_count_}, {"first_unix_ms", first_alarm_ms_}, {"recent", recent}};
  }
  if (!write_file_atomic(path_, j.dump(1) + "\n", 0600)) LOGW("bus log: cannot write %s", path_.c_str());
}

void BusLog::clear_alarm() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    alarm_count_ = 0;
    first_alarm_ms_ = 0;
    alarms_.clear();
    last_logged_.clear();
    dirty_ = true;
  }
  LOGI("bus: the TAP-reconfiguration alarm was cleared");
  save_if_changed();
}

bool BusLog::alarm_active() const {
  std::lock_guard<std::mutex> lock(mu_);
  return alarm_count_ > 0;
}

std::string BusLog::alarm_summary() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!alarm_count_ || alarms_.empty()) return "";
  time_t t = static_cast<time_t>(alarms_.back().unix_ms / 1000);
  char when[32];
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&t));
  return alarms_.back().what + ", " + std::to_string(alarm_count_) + (alarm_count_ == 1 ? " frame" : " frames") +
         ", last " + when;
}

json BusLog::to_json(size_t limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  json recent = json::array();
  for (const Alarm &a : alarms_) recent.push_back({{"unix_ms", a.unix_ms}, {"what", a.what}});
  int64_t now = unix_ms_now();
  json exchanges = json::array();
  std::deque<Exchange> ex = watch_.exchanges_at(now);
  size_t from = ex.size() > limit ? ex.size() - limit : 0;
  for (size_t i = from; i < ex.size(); i++) {
    const Exchange &e = ex[i];
    exchanges.push_back({{"id", e.id},
                         {"unix_ms", e.unix_ms},
                         {"ours", e.ours},
                         {"other_master", e.other_master},
                         {"addr", hex4(e.addr)},
                         {"cmd", hex4(e.cmd)},
                         {"name", e.name},
                         {"sub_name", e.sub_name},
                         {"node", e.node < 0 ? json(nullptr) : json(hex4(static_cast<unsigned>(e.node)))},
                         {"category", e.category},
                         {"result", e.result},
                         {"late", e.late},
                         {"rt_ms", e.rt_ms < 0 ? json(nullptr) : json(e.rt_ms)},
                         {"summary", e.summary},
                         {"answer", e.answer},
                         {"answer_after_ms", e.answer_after_ms < 0 ? json(nullptr) : json(e.answer_after_ms)},
                         {"request_hex", e.request_hex},
                         {"reply_hex", e.reply_hex},
                         {"reconfig", e.reconfig}});
  }
  BusStats st = watch_.stats(now);
  json stats = {{"window_s", esphome::tigo_monitor::master::BusWatch::WINDOW_MS / 1000},
                {"polls", st.polls},
                {"polls_lost", st.polls_lost},
                {"polls_empty", st.polls_empty},
                {"polls_refused", st.polls_refused},
                {"count", st.count},
                {"delivered", st.delivered},
                {"not_delivered", st.not_delivered},
                {"other_master", st.other_master},
                {"reconfig", st.reconfig},
                {"rt_median_ms", st.rt_median_ms < 0 ? json(nullptr) : json(st.rt_median_ms)},
                {"rt_max_ms", st.rt_max_ms < 0 ? json(nullptr) : json(st.rt_max_ms)},
                {"discovery", st.discovery}};
  return {{"alarm",
           {{"active", alarm_count_ > 0},
            {"count", alarm_count_},
            {"first_unix_ms", first_alarm_ms_},
            {"recent", recent}}},
          {"stats", stats},
          {"exchanges", exchanges}};
}

}  // namespace tigod
