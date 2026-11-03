#include "pv_store.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sstream>

namespace tigod {

bool PvStore::load() {
  std::lock_guard<std::mutex> lock(mu_);
  // Only a file that isn't there is PV on. One that is there but can't be
  // opened (its owner or mode changed, say) is as unreadable as a damaged one.
  struct stat st;
  if (stat(path_.c_str(), &st) != 0 && errno == ENOENT) return true;
  try {
    std::ifstream f(path_);
    if (!f) throw std::runtime_error(std::string("cannot open: ") + strerror(errno));
    std::stringstream ss;
    ss << f.rdbuf();
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
