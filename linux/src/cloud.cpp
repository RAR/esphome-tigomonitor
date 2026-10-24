#include "cloud.h"

#include "fsutil.h"
#include "log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace tigod {

using nlohmann::json;

// TIGOD_TIGO_API and TIGOD_TIGO_EI point these at a test server.
static std::string host(const char *env, const char *dflt) {
  const char *v = std::getenv(env);
  return v && *v ? v : dflt;
}
static const std::string API_HOST = host("TIGOD_TIGO_API", "https://mapi.tigoenergy.com");
static const std::string EI_HOST = host("TIGOD_TIGO_EI", "https://ei.tigoenergy.com");

static std::string str_field(const json &o, const char *k) {
  auto it = o.find(k);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

static std::unique_ptr<httplib::Client> client(const std::string &host) {
  auto c = std::make_unique<httplib::Client>(host);
  c->set_connection_timeout(10);
  c->set_read_timeout(20);
  c->set_default_headers({{"Accept", "application/json"}});
  return c;
}

Cloud::Cloud(std::string path) : path_(std::move(path)) {}

void Cloud::load() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return;
  email_ = j.value("email", "");
  token_ = j.value("token", "");
  refresh_ = j.value("refresh_token", "");
  expires_ = j.value("expires", "");
  system_id_ = j.value("system_id", 0);
  if (!token_.empty()) LOGI("cloud: token for system %d, expires %s", system_id_, expires_.c_str());
}

bool Cloud::save_locked_() const {
  json j = {{"email", email_}, {"token", token_}, {"refresh_token", refresh_}, {"expires", expires_},
            {"system_id", system_id_}};
  return write_file_atomic(path_, j.dump(2) + "\n", 0600);  // the token is a credential
}

void Cloud::forget_token_locked_() {
  token_.clear();
  save_locked_();
}

Cloud::Status Cloud::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return {!token_.empty(), email_, expires_, system_id_};
}

bool Cloud::login(const std::string &email, const std::string &password, std::string &err) {
  auto c = client(API_HOST);
  auto res = c->Post("/api/v3/user/login?type=8", json({{"username", email}, {"password", password}}).dump(),
                     "application/json");
  if (!res) return err = "cannot reach Tigo's servers: " + httplib::to_string(res.error()), false;
  if (res->status == 401 || res->status == 403) return err = "Tigo rejected the email or password", false;
  if (res->status != 200) return err = "Tigo login returned HTTP " + std::to_string(res->status), false;
  json j = json::parse(res->body, nullptr, false);
  if (j.is_discarded() || !j.contains("user") || !j["user"].is_object() || str_field(j["user"], "auth").empty())
    return err = "Tigo's login response had no token", false;
  const json &user = j["user"];

  std::string token = str_field(user, "auth");
  int system_id = 0;
  c->set_bearer_token_auth(token);
  if (auto sres = c->Get("/api/v3/systems/query?limit=100&page=1&sort=-id"); sres && sres->status == 200) {
    json s = json::parse(sres->body, nullptr, false);
    json systems = s.is_object() ? s.value("systems", s.value("data", json::array())) : json::array();
    if (systems.is_array() && !systems.empty() && systems[0].is_object()) {
      const json &s0 = systems[0];  // the newest system on the account
      if (s0.contains("system_id") && s0["system_id"].is_number()) system_id = s0["system_id"];
      else if (s0.contains("id") && s0["id"].is_number()) system_id = s0["id"];
    }
  }
  if (system_id == 0) return err = "logged in, but the account has no Tigo system", false;

  std::lock_guard<std::mutex> lock(mu_);
  email_ = email;
  token_ = token;
  refresh_ = str_field(user, "refresh_token");
  expires_ = str_field(user, "expires");
  system_id_ = system_id;
  save_locked_();
  LOGI("cloud: logged in, system %d, token expires %s", system_id_, expires_.c_str());
  return true;
}

bool Cloud::get_(const std::string &host, const std::string &path, std::string &body, std::string &err) {
  std::string token;
  {
    std::lock_guard<std::mutex> lock(mu_);
    token = token_;
  }
  if (token.empty()) return err = "not connected to Tigo cloud", false;
  auto c = client(host);
  c->set_bearer_token_auth(token);
  auto res = c->Get(path);
  if (!res) return err = "cannot reach Tigo's servers: " + httplib::to_string(res.error()), false;
  if (res->status == 401) {
    std::lock_guard<std::mutex> lock(mu_);
    forget_token_locked_();
    LOGW("cloud: token expired; log in again");
    return err = "the Tigo login has expired; connect again", false;
  }
  if (res->status != 200) return err = "Tigo returned HTTP " + std::to_string(res->status), false;
  body = std::move(res->body);
  return true;
}

bool Cloud::layout(CloudLayout &out, std::string &err) {
  int sid;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sid = system_id_;
  }
  std::string body;
  if (!get_(API_HOST, "/api/v3/systems/layout?id=" + std::to_string(sid), body, err)) return false;
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.contains("system") || !j["system"].is_object() ||
      !j["system"].value("inverters", json()).is_array())
    return err = "the layout had no inverters", false;
  auto str = [](const json &o, const char *k) {
    auto it = o.find(k);
    if (it == o.end()) return std::string();
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return std::string();
  };
  int global_mppt = 0;
  for (const auto &inv : j["system"]["inverters"]) {
    if (!inv.is_object()) continue;
    CloudInverter ci;
    ci.label = str(inv, "label");
    for (const auto &m : inv.value("mppts", json::array())) {
      // Renumbered across the system, as the firmware does: the cloud gives
      // every inverter its own "MPPT 1", which would merge their strings.
      std::string mppt = "MPPT " + std::to_string(++global_mppt);
      ci.mppts.push_back(mppt);
      if (!m.is_object()) continue;
      for (const auto &s : m.value("strings", json::array())) {
        if (!s.is_object()) continue;
        for (const auto &p : s.value("panels", json::array())) {
          if (!p.is_object() || str(p, "serial").empty()) continue;
          out.panels.push_back({str(p, "serial"), str(p, "label"), str(s, "label"), mppt, str(p, "object_id"), ""});
        }
      }
    }
    out.inverters.push_back(std::move(ci));
  }
  return true;
}

bool Cloud::health(std::string &out, std::string &err) {
  int sid;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sid = system_id_;
  }
  return get_(EI_HOST, "/api/v4/equipment-status/summary?systemId=" + std::to_string(sid), out, err);
}

bool Cloud::equipment(const std::string &view, std::string &out, std::string &err) {
  int sid;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sid = system_id_;
  }
  std::string path = view == "history" ? "/api/v4/equipment-status/history?limit=50&offset=0&systemId="
                                       : "/api/v4/equipment-status/latest?systemId=";
  return get_(EI_HOST, path + std::to_string(sid), out, err);
}

}  // namespace tigod
