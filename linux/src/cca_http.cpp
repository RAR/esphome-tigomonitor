#include "cca_http.h"

#include "log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <map>

namespace tigod {

using nlohmann::json;

// The CCA's local API answers to the account it ships with (Tigo:$olar),
// which the firmware sends too.
static const char *const CCA_USER = "Tigo";
static const char *const CCA_PASS = "$olar";
static constexpr size_t MAX_RESPONSE = 64 * 1024;  // as the firmware caps it

static bool get(const std::string &ip, const std::string &path, std::string &body, std::string &err) {
  httplib::Client c("http://" + ip);
  c.set_connection_timeout(5);
  c.set_read_timeout(10);
  c.set_basic_auth(CCA_USER, CCA_PASS);
  auto res = c.Get(path);
  if (!res) return err = "Failed to connect to CCA", false;
  if (res->status != 200) return err = "HTTP " + std::to_string(res->status), false;
  if (res->body.size() > MAX_RESPONSE) return err = "Response too large", false;
  if (res->body.empty()) return err = "Empty response", false;
  body = std::move(res->body);
  return true;
}

CcaHttp::CcaHttp(Monitor &monitor) : monitor_(monitor) {}

CcaHttp::~CcaHttp() { stop(); }

void CcaHttp::start() {
  thread_ = std::thread([this] { run_(); });
}

void CcaHttp::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void CcaHttp::request_refresh(const std::string &ip) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_ip_ = ip;
  v_.busy = true;
  cv_.notify_all();
}

CcaHttp::View CcaHttp::view() const {
  std::lock_guard<std::mutex> lock(mu_);
  return v_;
}

// summary_config is a flat array of objects linked by parent ids (strings):
// type 2 a panel, 3 a string, 4 an inverter — which the CCA calls what is
// an MPPT, so "Inverter N" becomes "MPPT N", as the firmware renames it.
bool CcaHttp::parse_summary_config(const std::string &body, std::vector<LayoutPanel> &out) {
  json root = json::parse(body, nullptr, false);
  if (!root.is_array()) return false;
  auto str = [](const json &o, const char *k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  std::map<std::string, const json *> by_id;
  for (const auto &o : root)
    if (o.is_object() && !str(o, "id").empty()) by_id[str(o, "id")] = &o;
  for (const auto &o : root) {
    if (!o.is_object() || o.value("type", json(0)) != 2 || str(o, "serial").empty()) continue;
    LayoutPanel p;
    p.serial = str(o, "serial");
    p.label = str(o, "label");
    p.channel = str(o, "channel");
    p.object_id = str(o, "id");
    auto parent = by_id.find(str(o, "parent"));
    if (parent != by_id.end() && parent->second->value("type", json(0)) == 3) {
      p.string_label = str(*parent->second, "label");
      auto gp = by_id.find(str(*parent->second, "parent"));
      if (gp != by_id.end()) p.mppt = str(*gp->second, "label");
      if (p.mppt.rfind("Inverter ", 0) == 0) p.mppt.replace(0, 9, "MPPT ");
    }
    out.push_back(std::move(p));
  }
  return true;
}

void CcaHttp::run_() {
  for (;;) {
    std::string ip;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !pending_ip_.empty(); });
      if (stop_) return;
      ip.swap(pending_ip_);
    }
    std::string info, err;
    if (!get(ip, "/cgi-bin/mobile_api?cmd=DEVICE_INFO", info, err)) {
      LOGW("cca: DEVICE_INFO from %s: %s", ip.c_str(), err.c_str());
      info = json({{"error", err}}).dump();
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      v_.device_info = info;
      v_.epoch = static_cast<int64_t>(time(nullptr));
    }
    std::string body;
    std::vector<LayoutPanel> panels;
    if (!get(ip, "/cgi-bin/summary_config", body, err)) LOGW("cca: summary_config from %s: %s", ip.c_str(), err.c_str());
    else if (!CcaHttp::parse_summary_config(body, panels)) LOGW("cca: summary_config from %s is not a JSON array", ip.c_str());
    else LOGI("cca: sync from %s: %zu panels, %d matched", ip.c_str(), panels.size(), monitor_.apply_layout(panels));
    std::lock_guard<std::mutex> lock(mu_);
    v_.busy = !pending_ip_.empty();
  }
}

}  // namespace tigod
