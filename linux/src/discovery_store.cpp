#include "discovery_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <fstream>
#include <sstream>

namespace tigod {

void DiscoveryStore::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream f(path_);
  if (!f) {
    if (errno != ENOENT) LOGE("cannot open %s; renewing the discovery window", path_.c_str());
    return;
  }
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    renew_ = nlohmann::json::parse(ss.str()).at("renew").get<bool>();
  } catch (const std::exception &e) {
    LOGE("%s is unreadable (%s); renewing the discovery window", path_.c_str(), e.what());
    renew_ = true;
  }
}

bool DiscoveryStore::renew() const {
  std::lock_guard<std::mutex> lock(mu_);
  return renew_;
}

void DiscoveryStore::save(bool renew) {
  std::string data = nlohmann::json{{"renew", renew}}.dump() + "\n";
  std::lock_guard<std::mutex> lock(mu_);
  renew_ = renew;
  if (!write_file_atomic(path_, data, 0600)) LOGE("cannot write %s", path_.c_str());
}

}  // namespace tigod
