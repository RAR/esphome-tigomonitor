#include "tap_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <fstream>
#include <sstream>

namespace tigod {

namespace mp = esphome::tigo_monitor::master;
using TapEntry = esphome::tigo_master::MasterCore::TapEntry;

static bool bytes_of(const std::string &hex, mp::Bytes &out) {
  out.clear();
  if (hex.empty()) return true;  // not heard yet
  if (hex.size() != 16 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  for (size_t i = 0; i < 16; i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return true;
}

std::vector<TapEntry> TapStore::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<TapEntry> taps;
  std::ifstream f(path_);
  if (!f) {
    if (errno != ENOENT) LOGE("cannot open %s; running the TAP at gateway_id only", path_.c_str());
    return taps;
  }
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str());
    for (const auto &t : j.at("taps")) {
      TapEntry e;
      std::string id = t.at("id").get<std::string>();
      if (id.size() != 4 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::runtime_error("bad id");
      e.id = static_cast<uint16_t>(std::stoi(id, nullptr, 16));
      if (!bytes_of(t.value("mac", ""), e.mac)) throw std::runtime_error("bad mac");
      taps.push_back(e);
    }
  } catch (const std::exception &e) {
    LOGE("%s is unreadable (%s); running the TAP at gateway_id only", path_.c_str(), e.what());
    taps.clear();
  }
  return taps;
}

bool TapStore::save(const std::vector<TapEntry> &taps) {
  std::lock_guard<std::mutex> lock(mu_);
  nlohmann::json list = nlohmann::json::array();
  for (const auto &t : taps) {
    char id[5];
    snprintf(id, sizeof(id), "%04X", t.id);
    list.push_back({{"id", id}, {"mac", t.mac.empty() ? "" : mp::mac_hex(t.mac)}});
  }
  if (!write_file_atomic(path_, nlohmann::json{{"taps", list}}.dump() + "\n", 0600)) {
    LOGE("cannot write %s", path_.c_str());
    return false;
  }
  return true;
}

}  // namespace tigod
