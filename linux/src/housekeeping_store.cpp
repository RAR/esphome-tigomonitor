#include "housekeeping_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tigod {

using nlohmann::json;
using esphome::tigo_monitor::master::Bytes;
using esphome::tigo_monitor::master::Frame;
using esphome::tigo_monitor::master::Learned41;

static std::string to_hex(const Bytes &b) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (uint8_t c : b) {
    out += digits[c >> 4];
    out += digits[c & 0xF];
  }
  return out;
}

static bool from_hex(const std::string &s, Bytes &out) {
  if (s.size() % 2) return false;
  out.clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    char *end = nullptr;
    std::string pair = s.substr(i, 2);
    long v = strtol(pair.c_str(), &end, 16);
    if (*end) return false;
    out.push_back(static_cast<uint8_t>(v));
  }
  return true;
}

HousekeepingStore::HousekeepingStore(std::string path) : path_(std::move(path)) {}

void HousekeepingStore::load() {
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.contains("entries") || !j["entries"].is_array()) {
    LOGW("housekeeping: ignoring %s, not the expected JSON", path_.c_str());
    return;
  }
  std::vector<Learned41::Entry> saved;
  for (const json &e : j["entries"]) {
    Learned41::Entry entry;
    if (!e.is_object() || !e.value("args", json()).is_string() || !from_hex(e["args"].get<std::string>(), entry.args))
      continue;
    entry.stream = static_cast<uint8_t>(e.value("stream", 0));
    entry.tuid = static_cast<uint16_t>(e.value("tuid", 0));
    saved.push_back(entry);
  }
  std::lock_guard<std::mutex> lock(mu_);
  learned_.load(saved);
  LOGI("housekeeping: %zu learned 0x41 targets", learned_.entries().size());
}

void HousekeepingStore::feed(const char *data, size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < n; i++) {
    splitter_.feed(data[i], [&](const std::string &body) {
      Frame f;
      if (esphome::tigo_monitor::master::decode_frame(body, f) && learned_.on_frame(f)) dirty_ = true;
    });
  }
}

void HousekeepingStore::save_if_changed() {
  json entries = json::array();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dirty_) return;
    dirty_ = false;
    for (const auto &e : learned_.entries())
      entries.push_back({{"stream", e.stream}, {"tuid", e.tuid}, {"args", to_hex(e.args)}});
  }
  if (!write_file_atomic(path_, json{{"entries", entries}}.dump(1) + "\n", 0600))
    LOGW("housekeeping: cannot write %s", path_.c_str());
}

std::vector<Learned41::Entry> HousekeepingStore::entries() const {
  std::lock_guard<std::mutex> lock(mu_);
  return learned_.entries();
}

}  // namespace tigod
