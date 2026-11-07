#include "bus_log.h"

#include "fsutil.h"
#include "log.h"

#include <chrono>
#include <ctime>
#include <fstream>

namespace tigod {

using nlohmann::json;
using esphome::tigo_monitor::master::BusEvent;
using esphome::tigo_monitor::master::Frame;

static int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static std::string hex4(unsigned v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%04X", v & 0xFFFF);
  return b;
}

void BusLog::load() {
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

void BusLog::on_frame_(const Frame &f, bool ours, int64_t now) {
  std::string what = watch_.on_frame(f, ours, now);
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
      if (!(f.dest & 0x8000))
        for (auto it = sent_.begin(); it != sent_.end(); ++it)
          if (*it == body) {
            sent_.erase(it);
            return;
          }
      on_frame_(f, false, now);
    });
  }
}

void BusLog::feed_tx(const char *data, size_t n) {
  int64_t now = unix_ms_now();
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

void BusLog::save_if_changed() {
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
  json events = json::array();
  const auto &ev = watch_.events();
  size_t from = ev.size() > limit ? ev.size() - limit : 0;
  for (size_t i = from; i < ev.size(); i++) {
    const BusEvent &e = ev[i];
    events.push_back({{"unix_ms", e.unix_ms},
                      {"from_tap", e.from_tap},
                      {"ours", e.ours},
                      {"addr", hex4(e.addr)},
                      {"cmd", hex4(e.cmd)},
                      {"sub", e.sub < 0 ? json(nullptr) : json(e.sub)},
                      {"node", e.node < 0 ? json(nullptr) : json(hex4(static_cast<unsigned>(e.node)))},
                      {"len", e.len},
                      {"name", e.name},
                      {"text", e.text},
                      {"reconfig", e.reconfig}});
  }
  return {{"alarm",
           {{"active", alarm_count_ > 0},
            {"count", alarm_count_},
            {"first_unix_ms", first_alarm_ms_},
            {"recent", recent}}},
          {"polls", watch_.polls()},
          {"events", events}};
}

}  // namespace tigod
