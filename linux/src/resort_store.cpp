#include "resort_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tigod {

namespace mp = esphome::tigo_monitor::master;
using esphome::tigo_master::ResortFile;

static bool from_hex(const std::string &hex, mp::Bytes &out) {
  out.clear();
  if (hex.size() % 2 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  for (size_t i = 0; i < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return true;
}

std::optional<ResortFile> ResortStore::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream f(path_);
  if (!f) {
    if (errno != ENOENT) LOGE("cannot open %s; no re-sort resumed", path_.c_str());
    return std::nullopt;
  }
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str());
    ResortFile r;
    r.phase = j.at("phase").get<std::string>();
    if (r.phase != "writing" && r.phase != "waiting") throw std::runtime_error("bad phase");
    r.started_unix = j.value("started_unix", static_cast<int64_t>(0));
    for (const auto &m : j.at("waited")) {
      mp::Bytes mac;
      if (!from_hex(m.get<std::string>(), mac) || mac.size() != 8) throw std::runtime_error("bad MAC");
      r.waited.push_back(mac);
    }
    for (const auto &n : j.at("old_networks")) {
      unsigned tap = 0;
      if (sscanf(n.at("tap").get<std::string>().c_str(), "%x", &tap) != 1 || !tap || tap > 0xFFFF)
        throw std::runtime_error("bad TAP");
      mp::Bytes b;
      mp::RadioBlock block;
      if (!from_hex(n.at("block").get<std::string>(), b) || !mp::RadioBlock::decode(b, block))
        throw std::runtime_error("bad block");
      r.old_networks.push_back({static_cast<uint16_t>(tap), block});
    }
    return r;
  } catch (const std::exception &e) {
    // Never its content: it holds the TAPs' keys.
    LOGE("%s is unreadable (%s); no re-sort resumed", path_.c_str(), e.what());
    return std::nullopt;
  }
}

bool ResortStore::save(const ResortFile &r) {
  std::lock_guard<std::mutex> lock(mu_);
  nlohmann::json waited = nlohmann::json::array(), old = nlohmann::json::array();
  for (const auto &m : r.waited) waited.push_back(mp::mac_hex(m));
  for (const auto &n : r.old_networks) {
    char tap[5];
    snprintf(tap, sizeof(tap), "%04X", n.first);
    old.push_back({{"tap", tap}, {"block", mp::mac_hex(n.second.encode())}});
  }
  nlohmann::json j = {{"phase", r.phase}, {"started_unix", r.started_unix}, {"waited", waited}, {"old_networks", old}};
  if (!write_file_atomic(path_, j.dump() + "\n", 0600)) {
    LOGE("cannot write %s", path_.c_str());
    return false;
  }
  return true;
}

bool ResortStore::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  if (std::remove(path_.c_str()) == 0 || errno == ENOENT) return true;
  LOGE("cannot delete %s", path_.c_str());
  return false;
}

}  // namespace tigod
