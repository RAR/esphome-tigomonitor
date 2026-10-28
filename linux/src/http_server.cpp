#include "http_server.h"

#include "log.h"
#include "serial_port.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ifaddrs.h>
#include <map>
#include <set>
#include <arpa/inet.h>
#include <net/if.h>
#include <sstream>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <thread>

namespace tigod {

using nlohmann::json;

static double r1(double v) { return std::round(v * 10) / 10; }
static double r2(double v) { return std::round(v * 100) / 100; }
static double r3(double v) { return std::round(v * 1000) / 1000; }

static void send_json(httplib::Response &res, const json &j, int status = 200) {
  res.status = status;
  res.set_header("Cache-Control", "no-store");
  // Replies carry text from the CCA and Tigo's cloud, which need not be UTF-8.
  res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

// Browsers send a saved login with requests other sites' pages make too, and
// a no-cors POST needs no preflight, so anything on a page the user visits
// could otherwise drive the API. Sec-Fetch-Site says where a request came
// from; browsers too old to send it still send Origin on a cross-site POST.
// Following a link here from another site is a navigation, and allowed.
static bool from_another_site(const httplib::Request &req) {
  std::string site = req.get_header_value("Sec-Fetch-Site");
  if (!site.empty()) {
    if (site == "same-origin" || site == "none") return false;
    return !(req.method == "GET" && req.get_header_value("Sec-Fetch-Mode") == "navigate");
  }
  std::string origin = req.get_header_value("Origin");
  if (origin.empty()) return false;  // not a browser, or a same-origin GET
  size_t p = origin.find("://");
  return p == std::string::npos || origin.substr(p + 3) != req.get_header_value("Host");
}

static bool same_secret(const std::string &a, const std::string &b) {
  unsigned char diff = a.size() != b.size();
  for (size_t i = 0; i < a.size() && i < b.size(); i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static std::string string_of(const Monitor &mon, const Panel &p) {
  for (const auto &n : mon.nodes())
    if (n.addr == p.addr && !n.cca_string_label.empty()) return n.cca_string_label;
  return Monitor::ALL_PANELS;
}

// /api/strings carries the MPPT label as "inverter" (the CCA's word for it);
// /api/inverters nests the same strings with it as "mppt". Same fields as the
// firmware's build_strings_json() and build_inverters_json().
static json string_json(const StringTotals &s, const std::map<std::string, StringOverride> &ov, bool nested) {
  auto it = ov.find(s.label);
  StringOverride o = it == ov.end() ? StringOverride{} : it->second;
  json j = {{"label", s.label},
            {"display_label", o.display_label},
            {nested ? "mppt" : "inverter", s.mppt},
            {"panel_rating_w", o.panel_rating_w},
            {"total_power", r1(s.total_power)},
            {"peak_power", r1(s.peak_power)},
            {"active_devices", s.active_devices},
            {"total_devices", s.total_devices}};
  if (!nested) {
    j["total_current"] = r3(s.total_current);
    j["avg_voltage_in"] = r2(s.avg_voltage_in);
    j["avg_voltage_out"] = r2(s.avg_voltage_out);
    j["avg_temperature"] = r1(s.avg_temperature);
    j["avg_efficiency"] = r2(s.avg_efficiency);
    j["min_efficiency"] = r2(s.min_efficiency);
    j["max_efficiency"] = r2(s.max_efficiency);
  }
  return j;
}

// Configured inverters, each with the strings whose MPPT it lists, then an
// "Unassigned" block for strings no inverter claims — which, with no
// inverters configured, is all of them (#60).
static json inverters_json(const Monitor &mon, const Topology &topo) {
  auto strings = mon.strings();
  auto ov = topo.string_overrides();
  std::vector<bool> claimed(strings.size(), false);
  json out = json::array();
  auto block = [&](json base, const std::vector<size_t> &idx) {
    double power = 0, peak = 0;
    int active = 0, total = 0;
    json arr = json::array();
    for (size_t i : idx) {
      const auto &s = strings[i];
      power += s.total_power;
      peak += s.peak_power;
      active += s.active_devices;
      total += s.total_devices;
      arr.push_back(string_json(s, ov, true));
    }
    base["total_power"] = r1(power);
    base["peak_power"] = r1(peak);
    base["active_devices"] = active;
    base["total_devices"] = total;
    base["strings"] = arr;
    return base;
  };
  for (const auto &inv : topo.inverters()) {
    std::vector<size_t> idx;
    for (const auto &m : inv.mppts)
      for (size_t i = 0; i < strings.size(); i++)
        if (!claimed[i] && strings[i].mppt == m) {
          claimed[i] = true;
          idx.push_back(i);
        }
    out.push_back(block({{"name", inv.name}, {"display_name", inv.display_name}, {"mppts", inv.mppts}}, idx));
  }
  std::vector<size_t> rest;
  json rest_mppts = json::array();
  for (size_t i = 0; i < strings.size(); i++) {
    if (claimed[i]) continue;
    rest.push_back(i);
    if (!strings[i].mppt.empty() && std::find(rest_mppts.begin(), rest_mppts.end(), strings[i].mppt) == rest_mppts.end())
      rest_mppts.push_back(strings[i].mppt);
  }
  if (!rest.empty())
    out.push_back(block({{"name", "Unassigned"}, {"display_name", ""}, {"unassigned", true}, {"mppts", rest_mppts}}, rest));
  return out;
}

static std::string panel_name(const Panel &p) {
  if (!p.label.empty()) return p.label;
  if (p.barcode.size() >= 6) return p.barcode.substr(p.barcode.size() - 6);
  return p.addr;
}

// /proc/meminfo, in bytes.
static void meminfo(uint64_t &total, uint64_t &available) {
  total = available = 0;
  std::ifstream in("/proc/meminfo");
  std::string key;
  uint64_t kb;
  std::string unit;
  while (in >> key >> kb) {
    std::getline(in, unit);
    if (key == "MemTotal:") total = kb * 1024;
    else if (key == "MemAvailable:") available = kb * 1024;
  }
}

static json cpu_temp() {
  std::ifstream in("/sys/class/thermal/thermal_zone0/temp");
  long milli;
  if (in >> milli) return r1(milli / 1000.0);
  return nullptr;
}

static std::string ip_address() {
  ifaddrs *ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return "";
  std::string ip;
  for (ifaddrs *i = ifs; i != nullptr; i = i->ifa_next) {
    if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
    if (i->ifa_flags & IFF_LOOPBACK) continue;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, buf, sizeof(buf));
    ip = buf;
    break;
  }
  freeifaddrs(ifs);
  return ip;
}

// Signal level of the first wireless interface, from /proc/net/wireless.
static json wifi_rssi() {
  std::ifstream in("/proc/net/wireless");
  std::string line;
  for (int i = 0; std::getline(in, line); i++) {
    if (i < 2) continue;  // two header lines
    std::istringstream ss(line);
    std::string iface, status, link, level;
    if (ss >> iface >> status >> link >> level) return std::atoi(level.c_str());
  }
  return nullptr;
}

static std::string mac_address() {
  for (const char *iface : {"wlan0", "eth0", "end0"}) {
    std::ifstream in(std::string("/sys/class/net/") + iface + "/address");
    std::string mac;
    if (in >> mac && mac != "00:00:00:00:00:00") return mac;
  }
  return "";
}

static long system_uptime_s() {
  struct sysinfo si {};
  return sysinfo(&si) == 0 ? si.uptime : 0;
}

HttpServer::HttpServer(HttpContext ctx) : ctx_(std::move(ctx)), svr_(std::make_unique<httplib::Server>()) {}

HttpServer::~HttpServer() { stop(); }

int HttpServer::start(int port, int fallback) {
  routes_();
  if (!svr_->bind_to_port("0.0.0.0", port)) {
    int bind_errno = errno;
    // A failed bind retires an httplib::Server for good; try on a new one.
    if (fallback != 0) {
      svr_ = std::make_unique<httplib::Server>();
      routes_();
    }
    if (fallback == 0 || !svr_->bind_to_port("0.0.0.0", fallback)) {
      LOGE("web: cannot listen on port %d (%s)", port, strerror(bind_errno));
      return 0;
    }
    LOGW("web: port %d is taken, so the web UI is on port %d; choose another under Tools", port, fallback);
    port = fallback;
  } else {
    LOGI("web: listening on port %d", port);
  }
  port_ = port;
  thread_ = std::make_unique<std::thread>([this] { svr_->listen_after_bind(); });
  return port;
}

void HttpServer::stop() {
  if (!thread_) return;
  svr_->stop();
  thread_->join();
  thread_.reset();
}

void HttpServer::routes_() {
  auto &svr = *svr_;
  const Monitor &mon = ctx_.monitor;

  // httplib reads a body of any size into memory; the largest real one, a
  // node table backup, is well under 1 MB.
  svr.set_payload_max_length(4 << 20);
  svr.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
    if (from_another_site(req)) {
      send_json(res, {{"error", "Request from another site refused."}}, 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    Settings s = ctx_.current_settings();
    if (!s.web_password.empty() &&
        !same_secret(req.get_header_value("Authorization"),
                     "Basic " + httplib::detail::base64_encode(s.web_username + ":" + s.web_password))) {
      res.status = 401;
      res.set_header("WWW-Authenticate", "Basic realm=\"Tigo Monitor\", charset=\"UTF-8\"");
      res.set_content("Log in with the web UI username and password set under Tools.", "text/plain");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  auto app = [this](const httplib::Request &, httplib::Response &res) {
    std::ifstream in(ctx_.web_dir + "/app.html", std::ios::binary);
    if (!in) {
      res.status = 500;
      res.set_content("app.html is missing from " + ctx_.web_dir, "text/plain");
      return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string page = ss.str();
    // The ESP32 puts its api_token here. The Pi has none, and left in, the
    // placeholder goes out as "Authorization: Bearer __TIGO_API_TOKEN__" on
    // every API call, which displaces the browser's login and fails it.
    static const std::string TOKEN = "__TIGO_API_TOKEN__";
    size_t at = page.find(TOKEN);
    if (at != std::string::npos) page.replace(at, TOKEN.size(), "");
    res.set_content(page, "text/html; charset=utf-8");
  };
  svr.Get("/", app);
  svr.Get("/app", app);

  svr.Get("/favicon.ico", [this](const httplib::Request &, httplib::Response &res) {
    std::ifstream in(ctx_.web_dir + "/favicon.svg", std::ios::binary);
    if (!in) {
      res.status = 404;
      return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    res.set_header("Cache-Control", "public, max-age=86400");
    res.set_content(ss.str(), "image/svg+xml");
  });
  // The firmware's old page addresses, sent to the matching view. Relative,
  // as the firmware's are, so a reverse proxy's path prefix survives.
  for (const auto &page : std::vector<std::pair<const char *, const char *>>{
           {"/nodes", "nodes"}, {"/status", "diagnostics"}, {"/history", "history"}, {"/cca", "cca"}, {"/yaml", "tools"}}) {
    std::string loc = std::string("app#") + page.second;
    svr.Get(page.first, [loc](const httplib::Request &, httplib::Response &res) {
      res.status = 302;
      res.set_header("Location", loc);
      res.set_content("<a href=\"" + loc + "\">" + loc + "</a>", "text/html");
    });
  }
  // The page checks for a newer release from the browser; this only says where.
  svr.Get("/api/github/release", [](const httplib::Request &, httplib::Response &res) {
    res.set_header("Cache-Control", "public, max-age=3600");
    res.set_content(R"({"fetch_url":"https://api.github.com/repos/RAR/esphome-tigomonitor/releases/latest"})",
                    "application/json");
  });

  svr.Get("/api/devices", [&mon](const httplib::Request &, httplib::Response &res) {
    auto now = Clock::now();
    json devices = json::array();
    for (const auto &p : mon.panels()) {
      const auto &r = p.reading;
      auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.updated).count();
      devices.push_back({{"addr", p.addr},
                         {"barcode", p.barcode},
                         {"name", panel_name(p)},
                         {"string_label", string_of(mon, p)},
                         {"voltage_in", r2(r.voltage_in)},
                         {"voltage_out", r2(r.voltage_out)},
                         {"current", r2(r.current_in)},
                         {"current_out", r2(r.current_out)},
                         {"power_in", r1(r.power_in)},
                         {"power", r1(r.power_out)},
                         {"power_out", r1(r.power_out)},
                         {"peak_power", r1(p.peak_power)},
                         {"temperature", r1(r.temperature)},
                         {"rssi", r.rssi},
                         {"duty_cycle", r1(r.duty_cycle * 100.0 / 255.0)},
                         {"efficiency", r2(r.efficiency)},
                         {"data_age_ms", age},
                         {"stale", now - p.updated >= mon.online_window()}});
    }
    send_json(res, {{"devices", devices}, {"count", devices.size()}});
  });

  svr.Get("/api/overview", [&mon](const httplib::Request &, httplib::Response &res) {
    auto now = Clock::now();
    double power_out = 0, current = 0, eff = 0, temp = 0;
    int active = 0;
    auto panels = mon.panels();
    for (const auto &p : panels) {
      if (now - p.updated >= mon.online_window()) continue;
      active++;
      power_out += p.reading.power_out;
      current += p.reading.current_in;
      eff += p.reading.efficiency;
      temp += p.reading.temperature;
    }
    double today = mon.today_kwh();
    double month = today;
    char ym[8];
    time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    strftime(ym, sizeof(ym), "%Y-%m", &lt);
    for (const auto &d : mon.daily_history())
      if (d.first.compare(0, 7, ym) == 0) month += d.second;
    size_t known = std::max(panels.size(), mon.nodes().size());
    send_json(res, {{"total_power", r1(power_out)},
                    {"total_current", r2(current)},
                    {"avg_efficiency", active ? r2(eff / active) : 0.0},
                    {"avg_temperature", active ? r2(temp / active) : 0.0},
                    {"active_devices", active},
                    {"max_devices", known},
                    {"total_energy", r2(mon.energy_kwh())},
                    {"total_energy_in", r2(mon.energy_in_kwh())},
                    {"total_energy_out", r2(mon.energy_kwh())},
                    {"today_energy", r2(today)},
                    {"month_energy", r1(month)}});
  });

  svr.Get("/api/strings", [this, &mon](const httplib::Request &, httplib::Response &res) {
    auto ov = ctx_.topology.string_overrides();
    json arr = json::array();
    for (const auto &st : mon.strings()) arr.push_back(string_json(st, ov, false));
    send_json(res, {{"strings", arr}});
  });
  svr.Get("/api/inverters", [this, &mon](const httplib::Request &, httplib::Response &res) {
    send_json(res, {{"inverters", inverters_json(mon, ctx_.topology)}});
  });

  svr.Post("/api/nodes/delete", [this](const httplib::Request &req, httplib::Response &res) {
    std::string addr = req.has_param("addr") ? req.get_param_value("addr") : "";
    if (addr.empty()) return send_json(res, {{"status", "error"}, {"message", "Missing addr parameter"}}, 400);
    if (!ctx_.mutable_monitor.delete_node(addr))
      return send_json(res, {{"status", "error"}, {"message", "Node not found"}}, 404);
    send_json(res, {{"status", "ok"}, {"message", "Node deleted successfully"}});
  });

  // The CCA over Bluetooth, in the firmware's shapes (tigo_web_server.cpp
  // api_cca_*). Requests only queue work for the CCA thread; the page polls
  // the cached results, as it does on the ESP32.
  auto cca_unavailable = [this](httplib::Response &res) {
    auto v = ctx_.cca.view();
    if (v.adapter) return false;
    ctx_.cca.request_probe();  // so the page works on its next load once BlueZ is up
    send_json(res, {{"status", "error"}, {"message", v.error.empty() ? "Bluetooth is not available" : v.error}}, 400);
    return true;
  };
  svr.Get("/api/cca", [this](const httplib::Request &, httplib::Response &res) {
    // A CCA IP means its local HTTP API, as cca_source: http does.
    std::string ip = ctx_.current_settings().cca_ip;
    if (!ip.empty()) {
      auto h = ctx_.cca_http.view();
      if (h.epoch == 0 && !h.busy) ctx_.cca_http.request_refresh(ip);  // first look, as the firmware queries
      int64_t age = h.epoch ? std::max<int64_t>(0, time(nullptr) - h.epoch) : 0;
      send_json(res, {{"cca_ip", ip},
                      {"last_sync_seconds_ago", age},
                      {"device_info", h.device_info.empty() ? "{}" : h.device_info},
                      {"network_info", "{}"}});
      return;
    }
    auto v = ctx_.cca.view();
    std::string info = v.device_info;
    // A refresh that failed after the last good one shows its reason, which
    // the page treats as the end of a refresh; otherwise the cached copy.
    if (!v.adapter || (v.info_failed_epoch > v.info_epoch && !v.busy))
      info = json({{"error", v.error.empty() ? "Bluetooth is not available" : v.error}}).dump();
    int64_t age = v.info_epoch ? std::max<int64_t>(0, time(nullptr) - v.info_epoch) : 0;
    send_json(res, {{"cca_ip", v.active_mac.empty() ? "BLE (nearest CCA)" : "BLE " + v.active_mac},
                    {"last_sync_seconds_ago", age},
                    {"device_info", info.empty() ? "{}" : info},
                    {"network_info", v.network_info.empty() ? "{}" : v.network_info}});
  });
  auto refresh = [this, cca_unavailable](const httplib::Request &, httplib::Response &res) {
    std::string ip = ctx_.current_settings().cca_ip;
    if (!ip.empty()) {
      ctx_.cca_http.request_refresh(ip);  // device info, then the node table
      return send_json(res, {{"status", "ok"}, {"message", "CCA refresh initiated"}});
    }
    if (cca_unavailable(res)) return;
    ctx_.cca.request_refresh();
    send_json(res, {{"status", "ok"}, {"message", "BLE refresh initiated"}});
  };
  svr.Get("/api/cca/refresh", refresh);
  auto scan_json = [this]() {
    auto v = ctx_.cca.view();
    json devices = json::array();
    int64_t now = time(nullptr);
    for (const auto &h : v.hits)
      devices.push_back({{"mac", h.mac}, {"rssi", h.rssi}, {"name", h.name}, {"age_s", now - h.seen}});
    // No YAML on the Pi: without a saved CCA, the nearest one is used.
    return json({{"active_mac", v.active_mac}, {"yaml_mac", ""}, {"saved", !v.saved_mac.empty()},
                 {"devices", devices}});
  };
  svr.Get("/api/cca/ble-scan", [this, cca_unavailable, scan_json](const httplib::Request &req, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    if (req.has_param("rescan") && req.get_param_value("rescan") == "1") {
      ctx_.cca.clear_hits();
      ctx_.cca.request_scan();
    }
    send_json(res, scan_json());
  });
  svr.Post("/api/cca/ble-mac", [this, cca_unavailable, scan_json](const httplib::Request &req, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    json j = json::parse(req.body, nullptr, false);
    bool ok = false;
    if (j.is_object() && j.value("reset", json(false)).is_boolean() && j.value("reset", false)) {
      ctx_.cca.reset_mac();
      ok = true;
    } else if (j.is_object() && j.contains("mac") && j["mac"].is_string()) {
      ok = ctx_.cca.set_mac(j["mac"].get<std::string>());
    }
    if (!ok)
      return send_json(res, {{"status", "error"}, {"message", "invalid MAC (expected AA:BB:CC:DD:EE:FF)"}}, 400);
    send_json(res, scan_json());
  });
  svr.Get("/api/cca/discovery", [this](const httplib::Request &, httplib::Response &res) {
    auto v = ctx_.cca.view();
    if (v.discovery.empty()) return send_json(res, {{"age_s", nullptr}, {"status", nullptr}});
    json status = json::parse(v.discovery, nullptr, false);
    send_json(res, {{"age_s", std::max<int64_t>(0, time(nullptr) - v.discovery_epoch)},
                    {"status", status.is_discarded() ? json(v.discovery) : status}});
  });
  svr.Post("/api/cca/discovery/start", [this, cca_unavailable](const httplib::Request &, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command("START_DISCOVERY");
    send_json(res, {{"status", "ok"}, {"message", "Discovery start requested"}});
  });
  svr.Post("/api/cca/discovery/poll", [this, cca_unavailable](const httplib::Request &, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command("DISCOVERY_STATUS");
    send_json(res, {{"status", "ok"}, {"message", "Discovery status poll requested"}});
  });
  // Network commands, allowlisted as on the ESP32 so a query value never
  // reaches the CCA or the response unchecked.
  static const std::set<std::string> NET_READS = {"NETWORK_WIFI_SCAN"};
  static const std::set<std::string> NET_KNOWN = {"NETWORK_WIFI_SCAN", "NETWORK_WIFI_CONNECT", "NETWORK_WIFI_CLEAR",
                                                  "NETWORK_INFO", "DEVICE_DATA_EXPORT"};
  svr.Get("/api/cca/network", [this](const httplib::Request &req, httplib::Response &res) {
    std::string cmd = req.has_param("cmd") ? req.get_param_value("cmd") : "";
    if (!NET_KNOWN.count(cmd)) return send_json(res, {{"error", "missing or unknown cmd"}}, 400);
    std::string raw;
    int64_t epoch = 0;
    if (cmd == "NETWORK_INFO") {
      auto v = ctx_.cca.view();
      raw = v.network_info;
      epoch = v.info_epoch;
    } else {
      ctx_.cca.result(cmd, raw, epoch);
    }
    if (raw.empty()) return send_json(res, {{"cmd", cmd}, {"age_s", nullptr}, {"result", nullptr}});
    json result = json::parse(raw, nullptr, false);
    send_json(res, {{"cmd", cmd},
                    {"age_s", std::max<int64_t>(0, time(nullptr) - epoch)},
                    {"result", result.is_discarded() ? json(raw) : result}});
  });
  svr.Post("/api/cca/network/poll", [this, cca_unavailable](const httplib::Request &req, httplib::Response &res) {
    std::string cmd = req.has_param("cmd") ? req.get_param_value("cmd") : "";
    if (!NET_READS.count(cmd))
      return send_json(res, {{"status", "error"}, {"message", "cmd must be a known network read"}}, 400);
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command(cmd);
    send_json(res, {{"status", "ok"}, {"message", "queued"}});
  });
  svr.Post("/api/cca/network/wifi-connect", [this, cca_unavailable](const httplib::Request &req,
                                                                     httplib::Response &res) {
    json j = json::parse(req.body, nullptr, false);
    std::string nid = j.is_object() && j.contains("nid") && j["nid"].is_string() ? j["nid"].get<std::string>() : "";
    std::string pwd = j.is_object() && j.contains("pwd") && j["pwd"].is_string() ? j["pwd"].get<std::string>() : "";
    if (nid.empty()) return send_json(res, {{"status", "error"}, {"message", "missing 'nid' (SSID)"}}, 400);
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command("NETWORK_WIFI_CONNECT",
                             "nid=" + CcaBle::uri_encode(nid) + " pwd=" + CcaBle::uri_encode(pwd));
    send_json(res, {{"status", "ok"}, {"message", "WiFi join requested"}});
  });
  svr.Post("/api/cca/network/wifi-clear", [this, cca_unavailable](const httplib::Request &, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command("NETWORK_WIFI_CLEAR");
    send_json(res, {{"status", "ok"}, {"message", "WiFi clear requested"}});
  });
  svr.Post("/api/cca/data-export", [this, cca_unavailable](const httplib::Request &, httplib::Response &res) {
    if (cca_unavailable(res)) return;
    ctx_.cca.request_command("DEVICE_DATA_EXPORT");
    send_json(res, {{"status", "ok"}, {"message", "Cloud export requested"}});
  });

  // Tigo cloud, in the firmware's shapes (tigo_web_server.cpp api_cloud_*).
  svr.Get("/api/cloud/status", [this](const httplib::Request &, httplib::Response &res) {
    auto s = ctx_.cloud.status();
    send_json(res, {{"configured", s.configured}, {"email", s.email}, {"expires", s.expires},
                    {"system_id", s.system_id}});
  });
  svr.Post("/api/cloud/login", [this](const httplib::Request &req, httplib::Response &res) {
    json j = json::parse(req.body, nullptr, false);
    auto field = [&j](const char *k) {
      auto it = j.is_object() ? j.find(k) : j.end();
      return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::string email = field("email"), password = field("password");
    if (email.empty() || password.empty())
      return send_json(res, {{"status", "error"}, {"message", "email and password required"}}, 400);
    std::string err;
    bool ok = ctx_.cloud.login(email, password, err);
    password.assign(password.size(), '\0');
    if (!ok) return send_json(res, {{"status", "error"}, {"message", "Login failed: " + err}}, 502);
    auto s = ctx_.cloud.status();
    send_json(res, {{"status", "ok"}, {"email", s.email}, {"expires", s.expires}, {"system_id", s.system_id}});
  });
  svr.Post("/api/cloud/import", [this](const httplib::Request &, httplib::Response &res) {
    CloudLayout layout;
    std::string err;
    if (!ctx_.cloud.layout(layout, err))
      return send_json(res, {{"status", "error"}, {"message", "Import failed: " + err}}, 502);
    int matched = ctx_.mutable_monitor.apply_layout(layout.panels);
    // The ESP32 takes its inverters from YAML. Here the layout can supply
    // them, but only on a Pi with none set up, so it never undoes an edit.
    bool added_inverters = false;
    if (matched > 0 && ctx_.topology.inverters().empty()) {
      std::vector<InverterConfig> invs;
      int n = 0;
      for (const auto &ci : layout.inverters) {
        n++;
        if (ci.mppts.empty()) continue;
        invs.push_back({ci.label.empty() ? "Inverter " + std::to_string(n) : ci.label, "", ci.mppts});
      }
      added_inverters = !invs.empty() && ctx_.topology.set_inverters(invs);
    }
    send_json(res, {{"status", "ok"},
                    {"message", "Layout imported from Tigo cloud: " + std::to_string(matched) + " of " +
                                    std::to_string(layout.panels.size()) + " panels matched"},
                    {"panels", layout.panels.size()},
                    {"matched", matched},
                    {"inverters_added", added_inverters}});
  });
  svr.Get("/api/cloud/health", [this](const httplib::Request &, httplib::Response &res) {
    std::string body, err;
    if (!ctx_.cloud.health(body, err))
      return send_json(res, {{"status", "error"}, {"message", "Health unavailable: " + err}}, 502);
    json summary = json::parse(body, nullptr, false);
    if (!summary.is_array())
      return send_json(res, {{"status", "error"}, {"message", "Health unavailable: unexpected response"}}, 502);
    send_json(res, {{"status", "ok"}, {"summary", summary}});
  });
  svr.Get("/api/cloud/equipment", [this](const httplib::Request &req, httplib::Response &res) {
    std::string view = req.has_param("view") && req.get_param_value("view") == "history" ? "history" : "latest";
    std::string body, err;
    if (!ctx_.cloud.equipment(view, body, err))
      return send_json(res, {{"status", "error"}, {"message", "Equipment status unavailable: " + err}}, 502);
    json data = json::parse(body, nullptr, false);
    if (!data.is_array())
      return send_json(res, {{"status", "error"}, {"message", "Equipment status unavailable: unexpected response"}},
                       502);
    send_json(res, {{"status", "ok"}, {"view", view}, {"data", data}});
  });

  // History, in the firmware's shapes: ?range=day|week|month|year, and for a
  // panel ?slot=N from /api/panels.
  auto history_window = [this](const httplib::Request &req, httplib::Response &res, int64_t &start, int64_t &end,
                                int64_t &bucket, std::string &range) {
    range = req.has_param("range") ? req.get_param_value("range") : "day";
    int64_t window;
    if (range == "day") window = 86400;
    else if (range == "week") window = 7 * 86400;
    else if (range == "month") window = 30 * 86400;
    else if (range == "year") window = 365 * 86400;
    else {
      send_json(res, {{"error", "range must be day|week|month|year"}}, 400);
      return false;
    }
    if (!ctx_.history.ok()) {
      send_json(res, {{"error", "history not initialized"}, {"detail", ctx_.history.error()}}, 503);
      return false;
    }
    end = static_cast<int64_t>(time(nullptr));
    if (end < 1577836800) {
      send_json(res, {{"error", "system clock not set"},
                      {"detail", "the Pi's clock has not been set, so there is no time range to query — "
                                 "it sets itself from the network once it is online"}},
                503);
      return false;
    }
    start = end - window;
    // Long ranges are averaged into at most MAX_POINTS buckets: the first
    // round size (the chart names it) that is a whole number of snapshots.
    int64_t step = ctx_.current_settings().history_interval_min * 60;
    int64_t need = (window + History::MAX_POINTS - 1) / History::MAX_POINTS;
    bucket = 0;
    for (int64_t m : {1, 5, 10, 15, 30, 60, 120, 180, 240, 360, 720, 1440}) {
      if (m * 60 >= need && m * 60 >= step && (m * 60) % step == 0) {
        bucket = m * 60;
        break;
      }
    }
    if (bucket == 0) bucket = std::max(step, (need + step - 1) / step * step);
    return true;
  };
  svr.Get("/api/history/power", [this, history_window](const httplib::Request &req, httplib::Response &res) {
    int64_t start, end, bucket;
    std::string range;
    if (!history_window(req, res, start, end, bucket, range)) return;
    auto t0 = Clock::now();
    json rows = json::array();
    for (const auto &r : ctx_.history.system(start, end, bucket))
      rows.push_back({{"t", r.t}, {"p", std::lround(r.p)}, {"e", r3(r.e)}});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    send_json(res, {{"range", range}, {"start", start}, {"end", end}, {"interval_min", bucket / 60},
                    {"records", rows}, {"count", rows.size()}, {"query_ms", ms}});
  });
  svr.Get("/api/history/panel", [this, history_window](const httplib::Request &req, httplib::Response &res) {
    int slot = req.has_param("slot") ? std::atoi(req.get_param_value("slot").c_str()) : -1;
    if (slot < 0) return send_json(res, {{"error", "slot is required; see /api/panels"}}, 400);
    int64_t start, end, bucket;
    std::string range;
    if (!history_window(req, res, start, end, bucket, range)) return;
    auto t0 = Clock::now();
    json rows = json::array();
    for (const auto &r : ctx_.history.panel(slot, start, end, bucket))
      rows.push_back({{"t", r.t}, {"p", std::lround(r.p)}});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    send_json(res, {{"slot", slot}, {"range", range}, {"start", start}, {"end", end}, {"interval_min", bucket / 60},
                    {"records", rows}, {"count", rows.size()}, {"query_ms", ms}});
  });
  // The panel charts find a panel's slot by the last 6 characters of its
  // barcode, as on the ESP32.
  svr.Get("/api/panels", [this, &mon](const httplib::Request &, httplib::Response &res) {
    if (!ctx_.history.ok())
      return send_json(res, {{"error", "history not initialized"}, {"detail", ctx_.history.error()}}, 503);
    std::map<std::string, Node> by_barcode;
    for (const auto &n : mon.nodes()) by_barcode[n.long_address] = n;
    json slots = json::array();
    for (const auto &kv : ctx_.history.slots()) {
      const std::string &bc = kv.first;
      json o = {{"slot", kv.second}, {"barcode", bc.size() > 6 ? bc.substr(bc.size() - 6) : bc}};
      auto it = by_barcode.find(bc);
      if (it != by_barcode.end()) {
        o["label"] = it->second.cca_label;
        o["mppt"] = it->second.cca_mppt_label;
        o["string"] = it->second.cca_string_label;
      }
      slots.push_back(o);
    }
    send_json(res, {{"slots", slots}, {"count", slots.size()}, {"max_slots", ctx_.mutable_monitor.max_nodes()}});
  });

  // Topology page edits, as on the ESP32. Bodies: {"name","display_name"},
  // {"label","display_label"}, {"label","rating_w"}; an empty name clears it.
  auto body_of = [](const httplib::Request &req, httplib::Response &res, json &j) {
    j = json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      send_json(res, {{"success", false}, {"error", "body must be a JSON object"}}, 400);
      return false;
    }
    return true;
  };
  auto known_string = [&mon](const std::string &label) {
    for (const auto &st : mon.strings())
      if (st.label == label) return true;
    return false;
  };
  svr.Post("/api/inverters/rename", [this, body_of](const httplib::Request &req, httplib::Response &res) {
    json j;
    if (!body_of(req, res, j)) return;
    std::string name = j.value("name", ""), display = j.value("display_name", "");
    if (name.empty()) return send_json(res, {{"success", false}, {"error", "missing 'name'"}}, 400);
    if (!ctx_.topology.set_inverter_display_name(name, display))
      return send_json(res, {{"success", false}, {"error", "unknown inverter"}}, 404);
    send_json(res, {{"success", true}});
  });
  svr.Post("/api/strings/rename", [this, body_of, known_string](const httplib::Request &req, httplib::Response &res) {
    json j;
    if (!body_of(req, res, j)) return;
    std::string label = j.value("label", ""), display = j.value("display_label", "");
    if (label.empty()) return send_json(res, {{"success", false}, {"error", "missing 'label'"}}, 400);
    if (!known_string(label)) return send_json(res, {{"success", false}, {"error", "unknown string"}}, 404);
    ctx_.topology.set_string_display_label(label, display);
    send_json(res, {{"success", true}});
  });
  svr.Post("/api/strings/rating", [this, body_of, known_string](const httplib::Request &req, httplib::Response &res) {
    json j;
    if (!body_of(req, res, j)) return;
    std::string label = j.value("label", "");
    if (label.empty()) return send_json(res, {{"success", false}, {"error", "missing 'label'"}}, 400);
    if (!j.contains("rating_w") || !j["rating_w"].is_number_integer() || j["rating_w"].get<int>() < 0 ||
        j["rating_w"].get<int>() > 65535)
      return send_json(res, {{"success", false}, {"error", "rating_w must be 0-65535"}}, 400);
    if (!known_string(label)) return send_json(res, {{"success", false}, {"error", "unknown string"}}, 404);
    ctx_.topology.set_string_rating(label, j["rating_w"].get<int>());
    send_json(res, {{"success", true}});
  });

  // Pi-only: the inverters an ESP32 user lists under `inverters:` in YAML,
  // plus the MPPT labels the node table uses, for the Tools form to offer.
  svr.Get("/api/topology", [this, &mon](const httplib::Request &, httplib::Response &res) {
    json inv = json::array();
    for (const auto &i : ctx_.topology.inverters()) inv.push_back({{"name", i.name}, {"mppts", i.mppts}});
    std::set<std::string> seen;
    for (const auto &n : mon.nodes())
      if (!n.cca_mppt_label.empty()) seen.insert(n.cca_mppt_label);
    send_json(res, {{"inverters", inv}, {"mppts_seen", seen}});
  });
  svr.Post("/api/topology", [this, body_of](const httplib::Request &req, httplib::Response &res) {
    json j;
    if (!body_of(req, res, j)) return;
    if (!j.contains("inverters") || !j["inverters"].is_array())
      return send_json(res, {{"success", false}, {"error", "expected an \"inverters\" array"}}, 400);
    std::vector<InverterConfig> list;
    std::set<std::string> names, mppts;
    for (const auto &o : j["inverters"]) {
      InverterConfig inv;
      if (!o.is_object() || !o.contains("name") || !o["name"].is_string())
        return send_json(res, {{"success", false}, {"error", "every inverter needs a name"}}, 400);
      inv.name = o["name"].get<std::string>();
      if (inv.name.empty() || !names.insert(inv.name).second)
        return send_json(res, {{"success", false}, {"error", "inverter names must be unique and not empty"}}, 400);
      for (const auto &m : o.value("mppts", json::array())) {
        if (!m.is_string() || m.get<std::string>().empty()) continue;
        if (!mppts.insert(m.get<std::string>()).second)
          return send_json(res, {{"success", false},
                                 {"error", "MPPT \"" + m.get<std::string>() + "\" is listed under two inverters"}},
                           400);
        inv.mppts.push_back(m.get<std::string>());
      }
      list.push_back(std::move(inv));
    }
    if (!ctx_.topology.set_inverters(std::move(list)))
      return send_json(res, {{"success", false}, {"error", "Could not save to disk."}}, 500);
    send_json(res, {{"success", true}});
  });

  svr.Get("/api/nodes", [this, &mon](const httplib::Request &, httplib::Response &res) {
    json nodes = json::array();
    for (const auto &n : mon.nodes()) {
      nodes.push_back({{"addr", n.addr},
                       {"long_address", n.long_address},
                       {"sensor_index", n.sensor_index},
                       {"checksum", n.checksum},
                       {"cca_validated", n.cca_validated},
                       {"cca_label", n.cca_label},
                       {"cca_string", n.cca_string_label},
                       {"cca_mppt", n.cca_mppt_label},
                       {"cca_inverter", n.cca_mppt_label},  // the field's name before #60; older imports read it
                       {"cca_channel", n.cca_channel}});
    }
    // Topology names and ratings ride along, so an export restores them too.
    json inverters = json::array(), strings = json::array();
    for (const auto &i : ctx_.topology.inverters())
      inverters.push_back({{"name", i.name}, {"display_name", i.display_name}});
    for (const auto &kv : ctx_.topology.string_overrides()) {
      json o = {{"label", kv.first}};
      if (!kv.second.display_label.empty()) o["display_label"] = kv.second.display_label;
      if (kv.second.panel_rating_w > 0) o["panel_rating_w"] = kv.second.panel_rating_w;
      if (o.size() > 1) strings.push_back(o);
    }
    send_json(res, {{"nodes", nodes}, {"inverters", inverters}, {"strings", strings}});
  });

  svr.Get("/api/energy/history", [&mon](const httplib::Request &, httplib::Response &res) {
    json days = json::array();
    for (const auto &d : mon.daily_history()) days.push_back({{"date", d.first}, {"energy", r2(d.second)}});
    char today[16];
    time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    strftime(today, sizeof(today), "%Y-%m-%d", &lt);
    days.push_back({{"date", today}, {"energy", r2(mon.today_kwh())}});
    // The firmware's shape: lifetime total plus the days, today last.
    send_json(res, {{"current_energy", r3(mon.energy_kwh())}, {"history", days}});
  });

  auto status = [this, &mon](const httplib::Request &req, httplib::Response &res) {
    uint64_t total = 0, avail = 0;
    meminfo(total, avail);
    static std::atomic<uint64_t> min_seen{UINT64_MAX};
    uint64_t prev = min_seen.load();
    while (avail && avail < prev && !min_seen.compare_exchange_weak(prev, avail)) {
    }
    uint64_t min_avail = std::min(prev, avail);
    long up = system_uptime_s();
    if (req.path == "/api/health") {
      send_json(res, {{"status", "ok"}, {"uptime", up}, {"heap_free", avail}, {"heap_min_free", min_avail}});
      return;
    }
    BusStats s = mon.stats();
    struct statvfs fs {};
    uint64_t disk_free = 0, disk_total = 0;
    // The state directory, not settings.json: that file only exists once
    // settings have been saved, and statvfs fails on a missing path.
    std::string state_dir = ctx_.settings_path.substr(0, ctx_.settings_path.rfind('/') + 1);
    if (statvfs(state_dir.c_str(), &fs) == 0) {
      disk_free = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
      disk_total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
    }
    send_json(res, {{"platform", "linux"},
                    {"disk_free", disk_free},
                    {"disk_total", disk_total},
                    {"wifi_rssi", wifi_rssi()},
                    {"mac_address", mac_address()},
                    {"mqtt_configured", !ctx_.current_settings().mqtt.host.empty()},
                    {"free_heap", avail},
                    {"total_heap", total},
                    {"min_free_heap", min_avail},
                    {"uptime_sec", up},
                    {"uptime_days", up / 86400},
                    {"uptime_hours", (up / 3600) % 24},
                    {"uptime_mins", (up / 60) % 60},
                    {"esphome_version", "tigomonitord " + ctx_.version},
                    {"compilation_time", __DATE__ " " __TIME__},
                    {"internal_temp", cpu_temp()},
                    {"invalid_checksum", s.bad_checksums},
                    {"missed_frames", s.ends_without_start + s.overflows},
                    {"total_frames", s.frames},
                    {"command_frames", s.frame27},
                    {"frame_27_count", s.frame27},
                    {"network_connected", true},
                    {"ip_address", ip_address()},
                    {"reset_reason", "unknown"},
                    {"mqtt_connected", ctx_.mqtt_connected()},
                    {"mqtt_error", ctx_.mqtt_error()}});
  };
  svr.Get("/api/status", status);
  svr.Get("/api/health", status);

  // Tools > Device Configuration: the firmware's runtime knobs, in its
  // shape (tigo_config.cpp). "default" is the built-in default, where the
  // ESP32 has the YAML value; reverting returns to it.
  auto config_json = [](const Settings &s) {
    const Settings d;
    return json({{"power_calibration",
                  {{"value", r3(s.power_calibration)},
                   {"default", r3(d.power_calibration)},
                   {"overridden", std::fabs(s.power_calibration - d.power_calibration) > 1e-6}}},
                 {"night_mode_timeout",
                  {{"value", s.night_mode_timeout_min},
                   {"default", d.night_mode_timeout_min},
                   {"overridden", s.night_mode_timeout_min != d.night_mode_timeout_min}}},
                 {"reset_at_midnight",
                  {{"value", s.reset_at_midnight},
                   {"default", d.reset_at_midnight},
                   {"overridden", s.reset_at_midnight != d.reset_at_midnight}}},
                 {"sync_cca_on_startup",
                  {{"value", s.sync_cca_on_startup},
                   {"default", d.sync_cca_on_startup},
                   {"overridden", s.sync_cca_on_startup != d.sync_cca_on_startup}}},
                 {"cca_ip", {{"value", s.cca_ip}, {"default", d.cca_ip}, {"overridden", s.cca_ip != d.cca_ip}}}});
  };
  svr.Get("/api/config", [this, config_json](const httplib::Request &, httplib::Response &res) {
    std::lock_guard<std::mutex> lock(settings_mu_);
    send_json(res, config_json(load_settings(ctx_.settings_path)));
  });
  svr.Post("/api/config", [this, config_json](const httplib::Request &req, httplib::Response &res) {
    json j = json::parse(req.body, nullptr, false);
    auto field = [&j](const char *k) {
      auto it = j.is_object() ? j.find(k) : j.end();
      return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::string reset = field("reset"), key = field("key"), value = field("value");
    std::lock_guard<std::mutex> lock(settings_mu_);
    Settings s = load_settings(ctx_.settings_path);
    const Settings d;
    auto truthy = [](const std::string &v) { return v == "1" || v == "true" || v == "on" || v == "yes"; };
    bool ok = true;
    if (!reset.empty()) {
      if (reset == "power_calibration") s.power_calibration = d.power_calibration;
      else if (reset == "night_mode_timeout") s.night_mode_timeout_min = d.night_mode_timeout_min;
      else if (reset == "reset_at_midnight") s.reset_at_midnight = d.reset_at_midnight;
      else if (reset == "sync_cca_on_startup") s.sync_cca_on_startup = d.sync_cca_on_startup;
      else if (reset == "cca_ip") s.cca_ip = d.cca_ip;
      else ok = false;
    } else if (key == "power_calibration") {
      // Same range as the firmware's.
      float f = std::strtof(value.c_str(), nullptr);
      ok = f >= 0.5f && f <= 2.0f;
      if (ok) s.power_calibration = f;
    } else if (key == "night_mode_timeout") {
      long m = std::strtol(value.c_str(), nullptr, 10);
      ok = m >= 1 && m <= 1440;
      if (ok) s.night_mode_timeout_min = static_cast<int>(m);
    } else if (key == "reset_at_midnight") {
      s.reset_at_midnight = truthy(value);
    } else if (key == "sync_cca_on_startup") {
      s.sync_cca_on_startup = truthy(value);
    } else if (key == "cca_ip") {
      ok = value.size() < 64;
      if (ok) s.cca_ip = value;
    } else {
      ok = false;
    }
    if (!ok) return send_json(res, {{"status", "error"}, {"message", "invalid key or value out of range"}}, 400);
    if (!save_settings(ctx_.settings_path, s))
      return send_json(res, {{"status", "error"}, {"message", "Could not save settings to disk."}}, 500);
    ctx_.settings_changed = true;
    send_json(res, config_json(s));
  });

  svr.Post("/api/reset_peak_power", [this](const httplib::Request &, httplib::Response &res) {
    ctx_.mutable_monitor.reset_peaks();
    send_json(res, {{"ok", true}});
  });
  svr.Post("/api/reset_node_table", [this](const httplib::Request &, httplib::Response &res) {
    ctx_.mutable_monitor.reset_nodes();
    send_json(res, {{"ok", true}});
  });
  // Restores a node table exported from Nodes (here or on an ESP32). The
  // same file format and rules as the firmware's /api/nodes/import.
  svr.Post("/api/nodes/import", [this](const httplib::Request &req, httplib::Response &res) {
    auto fail = [&res](const std::string &msg) {
      send_json(res, {{"status", "error"}, {"message", msg}}, 400);
    };
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("nodes") || !body["nodes"].is_array())
      return fail("Expected a JSON object with a \"nodes\" array");
    auto str = [](const json &o, const char *k) {
      auto it = o.find(k);
      return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::vector<Node> nodes;
    std::set<std::string> barcodes;
    for (const auto &o : body["nodes"]) {
      if (!o.is_object()) continue;
      Node n;
      n.addr = str(o, "addr");
      n.long_address = str(o, "long_address");
      if (n.addr.empty() || n.long_address.empty()) continue;  // never announced
      n.cca_label = str(o, "cca_label");
      n.cca_string_label = str(o, "cca_string");
      n.cca_mppt_label = str(o, "cca_mppt");
      if (n.cca_mppt_label.empty()) n.cca_mppt_label = str(o, "cca_inverter");  // before the rename
      n.cca_channel = str(o, "cca_channel");
      n.cca_object_id = str(o, "cca_object_id");
      if (o.contains("sensor_index") && o["sensor_index"].is_number_integer()) n.sensor_index = o["sensor_index"];
      if (o.contains("cca_validated") && o["cca_validated"].is_boolean()) n.cca_validated = o["cca_validated"];
      barcodes.insert(n.long_address);
      nodes.push_back(std::move(n));
    }
    if (nodes.empty()) return fail("No valid nodes found in import data");
    // Entries sharing a barcode fold into one, so count barcodes (#74).
    size_t cap = ctx_.mutable_monitor.max_nodes();
    if (barcodes.size() > cap)
      return fail("This file has " + std::to_string(barcodes.size()) + " panels but the node table holds " +
                  std::to_string(cap) + ".");
    auto r = ctx_.mutable_monitor.import_nodes(nodes);
    // The file's Topology names and ratings, as the firmware applies them.
    int overrides = 0;
    const json file_inverters = body.value("inverters", json::array());
    const json file_strings = body.value("strings", json::array());
    for (const auto &o : file_inverters) {
      if (!o.is_object() || o.value("name", "") == "" || !o.contains("display_name") || !o["display_name"].is_string())
        continue;
      ctx_.topology.remember_inverter_display_name(o["name"], o["display_name"]);
      overrides++;
    }
    for (const auto &o : file_strings) {
      if (!o.is_object() || o.value("label", "") == "") continue;
      std::string label = o["label"];
      if (o.contains("display_label") && o["display_label"].is_string()) {
        ctx_.topology.set_string_display_label(label, o["display_label"]);
        overrides++;
      }
      if (o.contains("panel_rating_w") && o["panel_rating_w"].is_number_integer() && o["panel_rating_w"].get<int>() > 0) {
        ctx_.topology.set_string_rating(label, o["panel_rating_w"].get<int>());
        overrides++;
      }
    }
    send_json(res, {{"status", "ok"},
                    {"message", "Successfully imported " + std::to_string(r.imported) + " nodes"},
                    {"imported", r.imported},
                    {"remapped", r.remapped},
                    {"held", r.parked},
                    {"overrides", overrides}});
  });
  // systemd restarts the service (Restart=always in the unit).
  svr.Post("/api/restart", [this](const httplib::Request &, httplib::Response &res) {
    ctx_.restart_requested = true;
    send_json(res, {{"ok", true}});
  });

  // Pi-only: the bus master, when the daemon runs the bus in the CCA's place.
  svr.Get("/api/master", [this](const httplib::Request &, httplib::Response &res) {
    MasterStatus m = ctx_.master_status ? ctx_.master_status() : MasterStatus{};
    send_json(res, {{"role", ctx_.current_settings().bus_role},
                    {"active", m.active},
                    {"fault", fault_name(m.fault)},
                    {"phase", phase_name(m.phase)},
                    {"detail", m.detail},
                    {"extra_taps", m.extra_taps},
                    {"housekeeping",
                     {{"on", m.housekeeping},
                      {"learned_41", m.learned_41},
                      {"sent_41", m.sent_41},
                      {"sent_session", m.sent_session},
                      {"sent_broadcast", m.sent_broadcast},
                      {"sent_walk_pages", m.sent_walk_pages}}},
                    {"queries",
                     {{"on", m.queries},
                      {"writes", m.writes},
                      {"sent_reads", m.sent_reads},
                      {"sent_writes", m.sent_writes},
                      {"answers", m.answers}}},
                    {"tap_version", m.tap_version},
                    {"lease_read", m.lease_read},
                    {"lease",
                     {{"running", m.lease.running},
                      {"remaining_s", m.lease.remaining_s},
                      {"node_count", m.lease.node_count}}},
                    {"renewals", m.renewals},
                    {"polls", m.polls},
                    {"timeouts", m.timeouts},
                    {"resyncs", m.resyncs},
                    {"last_reply_unix", m.last_reply_unix}});
  });

  // Pi-only: the serial ports to choose the RS485 adapter from.
  svr.Get("/api/serial-ports", [](const httplib::Request &, httplib::Response &res) {
    json ports = json::array();
    for (const auto &p : list_serial_ports())
      ports.push_back({{"path", p.path}, {"device", p.device}, {"description", p.description}});
    send_json(res, {{"ports", ports}});
  });

  // Pi-only: the settings an ESP32 user would put in YAML.
  svr.Get("/api/settings", [this](const httplib::Request &, httplib::Response &res) {
    Settings s = ctx_.current_settings();
    send_json(res, {{"serial_port", s.serial_port},
                    {"bus_role", s.bus_role},
                    {"master_bench", s.master_bench},
                    {"master_startup", s.master_startup},
                    {"master_housekeeping", s.master_housekeeping},
                    {"master_queries", s.master_queries},
                    {"master_optimizer_writes", s.master_optimizer_writes},
                    {"gateway_id", s.gateway_id},
                    {"web_port", s.web_port},
                    {"web_port_active", port_},
                    {"web_username", s.web_username},
                    {"web_password_set", !s.web_password.empty()},
                    {"power_calibration", s.power_calibration},
                    {"publish_interval_s", s.publish_interval_s},
                    {"device_id", s.device_id},
                    {"mqtt",
                     {{"host", s.mqtt.host},
                      {"port", s.mqtt.port},
                      {"username", s.mqtt.username},
                      {"password_set", !s.mqtt.password.empty()},
                      {"discovery_prefix", s.mqtt.discovery_prefix},
                      {"base_topic", s.mqtt.base_topic}}},
                    {"mqtt_connected", ctx_.mqtt_connected()},
                    {"mqtt_error", ctx_.mqtt_error()}});
  });

  svr.Post("/api/settings", [this](const httplib::Request &req, httplib::Response &res) {
    json j = json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      send_json(res, {{"error", "Expected a JSON object."}}, 400);
      return;
    }
    std::lock_guard<std::mutex> lock(settings_mu_);
    // The file, not the running copy: a save the main loop has not applied
    // yet would otherwise be overwritten.
    Settings s = load_settings(ctx_.settings_path);
    std::string err;
    try {
      if (j.contains("serial_port")) s.serial_port = j["serial_port"].get<std::string>();
      if (j.contains("bus_role")) s.bus_role = j["bus_role"].get<std::string>();
      if (j.contains("master_bench")) s.master_bench = j["master_bench"].get<bool>();
      if (j.contains("master_startup")) s.master_startup = j["master_startup"].get<bool>();
      if (j.contains("master_housekeeping")) s.master_housekeeping = j["master_housekeeping"].get<bool>();
      if (j.contains("master_queries")) s.master_queries = j["master_queries"].get<bool>();
      if (j.contains("master_optimizer_writes")) s.master_optimizer_writes = j["master_optimizer_writes"].get<bool>();
      if (j.contains("gateway_id")) s.gateway_id = j["gateway_id"].get<int>();
      if (j.contains("web_port")) s.web_port = j["web_port"].get<int>();
      if (j.contains("web_username")) s.web_username = j["web_username"].get<std::string>();
      // Absent keeps it, and "" turns the login off.
      if (j.contains("web_password")) s.web_password = j["web_password"].get<std::string>();
      if (j.contains("power_calibration")) s.power_calibration = j["power_calibration"].get<float>();
      if (j.contains("publish_interval_s")) s.publish_interval_s = j["publish_interval_s"].get<int>();
      if (j.contains("mqtt") && j["mqtt"].is_object()) {
        const json &m = j["mqtt"];
        const MqttSettings before = s.mqtt;
        if (m.contains("host")) s.mqtt.host = m["host"].get<std::string>();
        if (m.contains("port")) s.mqtt.port = m["port"].get<int>();
        if (m.contains("username")) s.mqtt.username = m["username"].get<std::string>();
        // Absent means "keep"; the UI never sees the stored password. It is
        // kept only for the same broker and user, though: otherwise anyone
        // who could save settings could send it to a broker of their own.
        if (m.contains("password")) s.mqtt.password = m["password"].get<std::string>();
        else if (s.mqtt.host != before.host || s.mqtt.port != before.port || s.mqtt.username != before.username)
          s.mqtt.password.clear();
        if (m.contains("discovery_prefix")) s.mqtt.discovery_prefix = m["discovery_prefix"].get<std::string>();
        if (m.contains("base_topic")) s.mqtt.base_topic = m["base_topic"].get<std::string>();
      }
    } catch (const json::exception &) {
      err = "A setting has the wrong type.";
    }
    std::string bridge_host, bridge_port;
    if (err.empty() && s.serial_port.rfind(TCP_PREFIX, 0) == 0 &&
        !parse_tcp_source(s.serial_port, bridge_host, bridge_port))
      err = "A network bridge is host:port, with a port of 1-65535.";
    if (err.empty() && s.bus_role != "listen" && s.bus_role != "master") err = "Bus role is listen or master.";
    if (err.empty() && s.bus_role == "master" && s.serial_port.rfind(TCP_PREFIX, 0) == 0)
      err = "The bus master needs a serial port on the bus, not a network bridge.";
    if (err.empty() && (s.gateway_id < 1 || s.gateway_id > 0x7FFF)) err = "Gateway id must be 1-7FFF.";
    if (err.empty() && (s.mqtt.port < 1 || s.mqtt.port > 65535)) err = "MQTT port must be 1-65535.";
    if (err.empty() && (s.web_port < 0 || s.web_port > 65535)) err = "Web port must be 1-65535, or empty for the default.";
    if (err.empty() && (s.web_username.empty() || s.web_username.find(':') != std::string::npos))
      err = "The web UI username cannot be empty or contain a colon.";
    // Wildcards would make every publish fail, and an empty or slash-edged
    // level gives topics nothing in Home Assistant subscribes to.
    for (const std::string *t : {&s.mqtt.base_topic, &s.mqtt.discovery_prefix}) {
      if (err.empty() && (t->empty() || t->find_first_of("+#") != std::string::npos || t->front() == '/' ||
                          t->back() == '/' || t->find("//") != std::string::npos))
        err = "Topics cannot be empty, contain + or #, or start or end with /.";
    }
    if (err.empty() && (s.power_calibration <= 0 || s.power_calibration > 5)) err = "Power calibration must be between 0 and 5.";
    if (err.empty() && s.publish_interval_s < 5) err = "Publish interval must be at least 5 seconds.";
    if (!err.empty()) {
      send_json(res, {{"error", err}}, 400);
      return;
    }
    if (!save_settings(ctx_.settings_path, s)) {
      send_json(res, {{"error", "Could not save settings to disk."}}, 500);
      return;
    }
    ctx_.settings_changed = true;
    send_json(res, {{"saved", true}});
  });
}

}  // namespace tigod
