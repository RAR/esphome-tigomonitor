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
using esphome::tigo_monitor::master::CalSeen;

static std::string hex4(unsigned v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%04X", v & 0xFFFF);
  return b;
}

BusLog::BusLog(std::string alarm_path, std::string versions_path)
    : path_(std::move(alarm_path)),
      versions_path_(std::move(versions_path)),
      book_([](char level, const char *msg) {
        if (level == 'W') LOGW("%s", msg);
        else LOGI("%s", msg);
      }) {}

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
      if (!e.version.empty()) book_.versions.restore(e);
    }
  for (const auto &o : j.value("radios", json::array())) {
    if (!o.is_object()) continue;
    TapRadio r;
    r.tap = static_cast<uint16_t>(std::strtoul(o.value("tap", std::string("0")).c_str(), nullptr, 16));
    r.channel = o.value("channel", 0);
    r.slt = o.value("slt", 0);
    r.slm = o.value("slm", 0);
    r.profile = o.value("profile", 0);
    r.pan = static_cast<uint16_t>(std::strtoul(o.value("pan", std::string("0")).c_str(), nullptr, 16));
    if (r.tap) book_.radios[r.tap] = {r, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("ids", json::array())) {
    if (!o.is_object()) continue;
    auto tap = static_cast<uint16_t>(std::strtoul(o.value("tap", std::string("0")).c_str(), nullptr, 16));
    std::string mac = o.value("mac", std::string());
    if (tap && mac.size() == 16) book_.tap_ids[tap] = {mac, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("settings", json::array())) {
    if (!o.is_object()) continue;
    SmartSeen m;
    m.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
    m.settings = o.value("settings", std::string());
    m.stored = o.value("stored", false);
    if (!m.settings.empty()) book_.smarts[m.addr] = {m, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("ratings", json::array())) {
    if (!o.is_object()) continue;
    CalSeen c;
    c.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
    c.volts = o.value("volts", 0);
    c.watts = o.value("watts", 0);
    c.cal = o.value("cal", std::string());
    if (!c.cal.empty()) book_.cals[c.addr] = {c, o.value("heard_unix_ms", int64_t{0})};
  }
  for (const auto &o : j.value("owners", json::array())) {
    if (!o.is_object()) continue;
    std::string b = o.value("barcode", std::string());
    if (!b.empty())
      book_.owners.restore(static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16)), b);
  }
  for (const auto &o : j.value("changes", json::array())) {  // newest first
    if (!o.is_object()) continue;
    book_.changes.push_front({o.value("unix_ms", int64_t{0}),
                         static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16)),
                         o.value("what", std::string()), o.value("before", std::string()), o.value("after", std::string())});
  }
  while (book_.changes.size() > esphome::tigo_monitor::master::BusBook::KEEP_CHANGES) book_.changes.pop_front();
  for (const auto &o : j.value("mppt", json::array())) {
    if (!o.is_object()) continue;
    MpptSeen m;
    m.addr = static_cast<uint16_t>(std::strtoul(o.value("addr", std::string("0")).c_str(), nullptr, 16));
    m.params = o.value("params", std::string());
    m.answer = o.value("answer", std::string());
    if (!m.params.empty()) book_.mppts[m.addr] = {m, o.value("heard_unix_ms", int64_t{0})};
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

int BusLog::changes_since(int64_t since_ms) const {
  std::lock_guard<std::mutex> lock(mu_);
  int n = 0;
  for (const auto &c : book_.changes) n += c.unix_ms >= since_ms;
  return n;
}

void BusLog::on_frame_(const Frame &f, bool ours, int64_t now) {
  std::string what = watch_.on_frame(f, ours, now);
  book_.absorb(watch_, now, runs_bus_);
  if (what.empty()) return;
  // Listening (or standing by), another master runs the bus, and what it
  // sends the TAP is its own business: marked in the log, not raised.
  if (!runs_bus_) return;
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
  return dirty_ || book_.dirty;
}

// The API's: no PAN. The saved file's keeps it (versions_json_(true)).
json BusLog::versions_json() const { return versions_json_(false); }

std::vector<esphome::tigo_monitor::master::TapRadio> BusLog::radios() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<esphome::tigo_monitor::master::TapRadio> out;
  for (const auto &kv : book_.radios) out.push_back(kv.second.first);
  return out;
}

std::map<uint16_t, std::string> BusLog::tap_macs() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::map<uint16_t, std::string> out;
  for (const auto &kv : book_.tap_ids) out[kv.first] = kv.second.first;
  return out;
}

uint16_t BusLog::tap_of_mac(const std::string &mac) const {
  std::lock_guard<std::mutex> lock(mu_);
  // The address it gave this MAC from most recently: a TAP re-addressed by a
  // herd leaves its old one behind in book_.tap_ids.
  uint16_t best = 0;
  int64_t when = -1;
  for (const auto &kv : book_.tap_ids)
    if (kv.second.first == mac && kv.second.second > when) {
      best = kv.first;
      when = kv.second.second;
    }
  return best;
}

// One writer for both builds (bus_book.h): the ESP32 serves the same text.
json BusLog::versions_json_(bool with_pans) const {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(mu_);
    book_.write_json(text, with_pans);
  }
  return json::parse(text);
}

void BusLog::save_if_changed() {
  if (!versions_path_.empty()) {
    bool write;
    {
      std::lock_guard<std::mutex> lock(mu_);
      write = book_.dirty;
      book_.dirty = false;
    }
    if (write && !write_file_atomic(versions_path_, versions_json_(true).dump(1) + "\n", 0644))
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
