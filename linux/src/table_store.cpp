#include "table_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tigod {

namespace mp = esphome::tigo_monitor::master;

static bool bytes_of(const std::string &hex, mp::Bytes &out) {
  if (hex.size() != 16 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  out.clear();
  for (size_t i = 0; i < 16; i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return true;
}

bool TableStore::load(mp::Bytes &mac, std::vector<mp::TableRecord> &stay) {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream f(path_);
  if (!f) {
    if (errno != ENOENT) LOGE("cannot open %s; an unfinished removal can't be resumed", path_.c_str());
    return false;
  }
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str());
    if (!bytes_of(j.at("mac").get<std::string>(), mac)) throw std::runtime_error("bad mac");
    stay.clear();
    for (const auto &r : j.at("stay")) {
      mp::TableRecord rec;
      if (!bytes_of(r.at(0).get<std::string>(), rec.mac)) throw std::runtime_error("bad record");
      rec.tuid = static_cast<uint16_t>(r.at(1).get<int>());
      stay.push_back(rec);
    }
    return true;
  } catch (const std::exception &e) {
    // Left in place for a person to look at: it is the only copy of what
    // the TAP's table should hold.
    LOGE("%s is unreadable (%s); not resuming the removal it records", path_.c_str(), e.what());
    return false;
  }
}

bool TableStore::save(const mp::Bytes &mac, const std::vector<mp::TableRecord> *stay) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!stay) {
    if (std::remove(path_.c_str()) != 0 && errno != ENOENT) {
      LOGE("cannot remove %s", path_.c_str());
      return false;
    }
    return true;
  }
  nlohmann::json list = nlohmann::json::array();
  for (const auto &r : *stay) list.push_back({mp::mac_hex(r.mac), r.tuid});
  std::string data = nlohmann::json{{"mac", mp::mac_hex(mac)}, {"stay", list}}.dump() + "\n";
  if (!write_file_atomic(path_, data, 0600)) {
    LOGE("cannot write %s", path_.c_str());
    return false;
  }
  return true;
}

}  // namespace tigod
