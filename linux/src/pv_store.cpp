#include "pv_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace tigod {

bool PvStore::load() {
  std::ifstream f(path_);
  std::lock_guard<std::mutex> lock(mu_);
  if (!f) return true;
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    nlohmann::json j = nlohmann::json::parse(ss.str());
    state_.off = j.at("off").get<bool>();
    state_.heard = j.value("heard", false);
    state_.since_unix = j.value("since_unix", static_cast<int64_t>(0));
    return true;
  } catch (const std::exception &e) {
    LOGE("%s is unreadable (%s); holding PV output off", path_.c_str(), e.what());
    state_ = {true, false, 0};
    unreadable_ = true;
    return false;
  }
}

PvState PvStore::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

bool PvStore::unreadable() const {
  std::lock_guard<std::mutex> lock(mu_);
  return unreadable_;
}

void PvStore::save(const PvState &s) {
  std::string data = nlohmann::json{{"off", s.off}, {"heard", s.heard}, {"since_unix", s.since_unix}}.dump() + "\n";
  std::lock_guard<std::mutex> lock(mu_);
  state_ = s;
  if (write_file_atomic(path_, data, 0600)) unreadable_ = false;
  else LOGE("cannot write %s", path_.c_str());
}

}  // namespace tigod
