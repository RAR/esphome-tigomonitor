#include "http_server.h"

#include "log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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
  res.set_content(j.dump(), "application/json");
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

bool HttpServer::start(int port) {
  routes_();
  if (!svr_->bind_to_port("0.0.0.0", port)) {
    LOGE("web: cannot listen on port %d", port);
    return false;
  }
  LOGI("web: listening on port %d", port);
  thread_ = std::make_unique<std::thread>([this] { svr_->listen_after_bind(); });
  return true;
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

  auto app = [this](const httplib::Request &, httplib::Response &res) {
    std::ifstream in(ctx_.web_dir + "/app.html", std::ios::binary);
    if (!in) {
      res.status = 500;
      res.set_content("app.html is missing from " + ctx_.web_dir, "text/plain");
      return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    res.set_content(ss.str(), "text/html; charset=utf-8");
  };
  svr.Get("/", app);
  svr.Get("/app", app);

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
                         {"stale", now - p.updated >= Monitor::ONLINE_WINDOW}});
    }
    send_json(res, {{"devices", devices}, {"count", devices.size()}});
  });

  svr.Get("/api/overview", [&mon](const httplib::Request &, httplib::Response &res) {
    auto now = Clock::now();
    double power_out = 0, current = 0, eff = 0, temp = 0;
    int active = 0;
    auto panels = mon.panels();
    for (const auto &p : panels) {
      if (now - p.updated >= Monitor::ONLINE_WINDOW) continue;
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
                    {"total_energy_in", r2(mon.energy_kwh())},
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
    send_json(res, {{"days", days}});
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

  svr.Get("/api/config", [this](const httplib::Request &, httplib::Response &res) {
    Settings s = ctx_.current_settings();
    send_json(res, {{"power_calibration",
                     {{"value", s.power_calibration}, {"default", 1.0}, {"overridden", s.power_calibration != 1.0f}}}});
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

  // Pi-only: the settings an ESP32 user would put in YAML.
  svr.Get("/api/settings", [this](const httplib::Request &, httplib::Response &res) {
    Settings s = ctx_.current_settings();
    send_json(res, {{"serial_port", s.serial_port},
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
    Settings s = ctx_.current_settings();
    std::string err;
    try {
      if (j.contains("serial_port")) s.serial_port = j["serial_port"].get<std::string>();
      if (j.contains("power_calibration")) s.power_calibration = j["power_calibration"].get<float>();
      if (j.contains("publish_interval_s")) s.publish_interval_s = j["publish_interval_s"].get<int>();
      if (j.contains("mqtt") && j["mqtt"].is_object()) {
        const json &m = j["mqtt"];
        if (m.contains("host")) s.mqtt.host = m["host"].get<std::string>();
        if (m.contains("port")) s.mqtt.port = m["port"].get<int>();
        if (m.contains("username")) s.mqtt.username = m["username"].get<std::string>();
        // Absent means "keep"; the UI never sees the stored password.
        if (m.contains("password")) s.mqtt.password = m["password"].get<std::string>();
        if (m.contains("discovery_prefix")) s.mqtt.discovery_prefix = m["discovery_prefix"].get<std::string>();
        if (m.contains("base_topic")) s.mqtt.base_topic = m["base_topic"].get<std::string>();
      }
    } catch (const json::exception &) {
      err = "A setting has the wrong type.";
    }
    if (err.empty() && (s.mqtt.port < 1 || s.mqtt.port > 65535)) err = "MQTT port must be 1-65535.";
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
