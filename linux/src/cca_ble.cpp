#include "cca_ble.h"

#include "fsutil.h"
#include "log.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <systemd/sd-bus.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

namespace tigod {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

static const char *const BLUEZ = "org.bluez";
static const char *const CCA_SERVICE_UUID = "d3dadcba-e4fa-4016-ba33-8bcc671999a7";
static const char *const TIGO_OUI = "04:C0:5B";  // only the CCA advertises on it
static const uint32_t CCA_SESSION_SEED = 159260;  // the app's constant

static int64_t now_s() { return static_cast<int64_t>(time(nullptr)); }

// ---------------------------------------------------------------------------
// Reading BlueZ's object tree
// ---------------------------------------------------------------------------

struct Prop {
  std::string s;
  int64_t i = 0;
  bool b = false;
  std::vector<std::string> strv;
};
using Props = std::map<std::string, Prop>;
// path -> interface -> properties
using Objects = std::map<std::string, std::map<std::string, Props>>;

// Reads one variant into p; types other than the handful BlueZ uses for what
// we need are skipped.
static int read_variant(sd_bus_message *m, Prop &p) {
  const char *contents = nullptr;
  int r = sd_bus_message_peek_type(m, nullptr, &contents);
  if (r < 0) return r;
  if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0) return r;
  switch (contents[0]) {
    case 's':
    case 'o': {
      const char *v = nullptr;
      r = sd_bus_message_read_basic(m, contents[0], &v);
      if (r >= 0 && v) p.s = v;
      break;
    }
    case 'b': {
      int v = 0;
      r = sd_bus_message_read_basic(m, 'b', &v);
      p.b = v != 0;
      break;
    }
    case 'n': {
      int16_t v = 0;
      r = sd_bus_message_read_basic(m, 'n', &v);
      p.i = v;
      break;
    }
    case 'q': {
      uint16_t v = 0;
      r = sd_bus_message_read_basic(m, 'q', &v);
      p.i = v;
      break;
    }
    default:
      if (strcmp(contents, "as") == 0) {
        char **v = nullptr;
        r = sd_bus_message_read_strv(m, &v);
        for (char **x = v; x && *x; x++) {
          p.strv.emplace_back(*x);
          free(*x);
        }
        free(v);
      } else {
        r = sd_bus_message_skip(m, contents);
      }
  }
  if (r < 0) return r;
  return sd_bus_message_exit_container(m);
}

static int read_props(sd_bus_message *m, Props &props) {
  int r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0) return r;
  while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *name = nullptr;
    if ((r = sd_bus_message_read(m, "s", &name)) < 0) return r;
    if ((r = read_variant(m, props[name])) < 0) return r;
    if ((r = sd_bus_message_exit_container(m)) < 0) return r;
  }
  if (r < 0) return r;
  return sd_bus_message_exit_container(m);
}

static bool managed_objects(sd_bus *bus, Objects &out, std::string &err) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call_method(bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &e, &reply,
                             "");
  if (r < 0) {
    err = std::string("Bluetooth service not answering: ") + (e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return false;
  }
  r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
    const char *path = nullptr;
    if ((r = sd_bus_message_read(reply, "o", &path)) < 0) break;
    auto &ifaces = out[path];
    if ((r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}")) < 0) break;
    while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
      const char *iface = nullptr;
      if ((r = sd_bus_message_read(reply, "s", &iface)) < 0) break;
      if ((r = read_props(reply, ifaces[iface])) < 0) break;
      if ((r = sd_bus_message_exit_container(reply)) < 0) break;
    }
    if (r < 0) break;
    if ((r = sd_bus_message_exit_container(reply)) < 0) break;  // a{sa{sv}}
    if ((r = sd_bus_message_exit_container(reply)) < 0) break;  // e
  }
  sd_bus_message_unref(reply);
  if (r < 0) {
    err = std::string("cannot read Bluetooth devices: ") + strerror(-r);
    return false;
  }
  return true;
}

static bool call(sd_bus *bus, const std::string &path, const char *iface, const char *method, std::string &err,
                 uint64_t timeout_us = 0) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_message *m = nullptr, *reply = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, BLUEZ, path.c_str(), iface, method);
  if (r >= 0) r = sd_bus_call(bus, m, timeout_us, &e, &reply);
  sd_bus_message_unref(m);
  sd_bus_message_unref(reply);
  if (r < 0) {
    err = std::string(method) + " failed: " + (e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return false;
  }
  return true;
}

static std::string device_path(const std::string &adapter, const std::string &mac) {
  std::string p = adapter + "/dev_" + mac;
  for (size_t i = adapter.size(); i < p.size(); i++)
    if (p[i] == ':') p[i] = '_';
  return p;
}

static bool valid_mac(const std::string &mac) {
  if (mac.size() != 17) return false;
  for (size_t i = 0; i < 17; i++) {
    if (i % 3 == 2 ? mac[i] != ':' : !isxdigit(static_cast<unsigned char>(mac[i]))) return false;
  }
  return true;
}

static std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return s;
}

static std::string session_key(uint64_t uts) {
  std::string in = std::to_string(uts + CCA_SESSION_SEED);
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(in.data(), in.size(), hash, &len, EVP_sha512(), nullptr);
  static const char *const HEX = "0123456789abcdef";
  std::string out;
  for (unsigned int i = 0; i < len; i++) {
    out.push_back(HEX[hash[i] >> 4]);
    out.push_back(HEX[hash[i] & 0xF]);
  }
  return out;
}

std::string CcaBle::uri_encode(const std::string &v) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : v) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 0xF]);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle, queue and cache
// ---------------------------------------------------------------------------

CcaBle::CcaBle(std::string state_path) : path_(std::move(state_path)) {}

CcaBle::~CcaBle() { stop(); }

void CcaBle::start() {
  load_();
  thread_ = std::thread([this] { run_(); });
}

void CcaBle::stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  cv_.notify_all();
  thread_.join();
}

void CcaBle::load_() {
  std::lock_guard<std::mutex> lock(mu_);
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return;
  auto str = [&j](const char *k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : ""; };
  v_.saved_mac = str("mac");
  v_.active_mac = v_.saved_mac.empty() ? str("last_mac") : v_.saved_mac;
  v_.device_info = str("device_info");
  v_.network_info = str("network_info");
  v_.info_epoch = j.value("info_epoch", int64_t{0});
}

void CcaBle::save_locked_() const {
  json j = {{"mac", v_.saved_mac},          {"last_mac", v_.active_mac},       {"device_info", v_.device_info},
            {"network_info", v_.network_info}, {"info_epoch", v_.info_epoch}};
  write_file_atomic(path_, j.dump(2) + "\n");
}

CcaBle::View CcaBle::view() const {
  std::lock_guard<std::mutex> lock(mu_);
  return v_;
}

bool CcaBle::result(const std::string &cmd, std::string &out, int64_t &epoch) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = results_.find(cmd);
  if (it == results_.end()) return false;
  out = it->second.first;
  epoch = it->second.second;
  return true;
}

void CcaBle::clear_hits() {
  std::lock_guard<std::mutex> lock(mu_);
  v_.hits.clear();
}

bool CcaBle::set_mac(const std::string &mac) {
  if (!valid_mac(mac)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  v_.saved_mac = v_.active_mac = upper(mac);
  save_locked_();
  LOGI("cca: using CCA %s", v_.active_mac.c_str());
  return true;
}

void CcaBle::reset_mac() {
  std::lock_guard<std::mutex> lock(mu_);
  v_.saved_mac.clear();
  v_.active_mac.clear();  // picked again by the next scan
  save_locked_();
}

void CcaBle::request_refresh() {
  std::lock_guard<std::mutex> lock(mu_);
  jobs_.push_back({Job::REFRESH, "", ""});
  cv_.notify_all();
}

void CcaBle::request_command(const std::string &cmd, const std::string &params) {
  std::lock_guard<std::mutex> lock(mu_);
  jobs_.push_back({Job::COMMAND, cmd, params});
  cv_.notify_all();
}

void CcaBle::request_scan() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto &j : jobs_)
    if (j.kind == Job::SCAN) return;  // one is enough
  jobs_.push_back({Job::SCAN, "", ""});
  cv_.notify_all();
}

void CcaBle::request_probe() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto &j : jobs_)
    if (j.kind == Job::PROBE) return;
  jobs_.push_back({Job::PROBE, "", ""});
  cv_.notify_all();
}

bool CcaBle::open_bus_() {
  if (bus_) return true;
  // TIGOD_DBUS_ADDRESS points this at a test bus with a mock BlueZ on it.
  const char *addr = std::getenv("TIGOD_DBUS_ADDRESS");
  int r;
  if (addr && *addr) {
    r = sd_bus_new(&bus_);
    if (r >= 0) r = sd_bus_set_address(bus_, addr);
    if (r >= 0) r = sd_bus_set_bus_client(bus_, 1);
    if (r >= 0) r = sd_bus_start(bus_);
  } else {
    r = sd_bus_open_system(&bus_);
  }
  if (r >= 0) return true;
  bus_ = sd_bus_unref(bus_);
  std::lock_guard<std::mutex> lock(mu_);
  v_.error = std::string("cannot reach the system bus: ") + strerror(-r);
  return false;
}

void CcaBle::run_() {
  std::string err;
  bool ok = open_bus_() && ensure_adapter_(err);
  {
    std::lock_guard<std::mutex> lock(mu_);
    v_.adapter = ok;
    if (!err.empty()) v_.error = err;
    if (!ok) LOGI("cca: %s; the CCA page is off until it appears", v_.error.c_str());
  }

  while (!stop_) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) break;
      job = jobs_.front();
      jobs_.pop_front();
      v_.busy = true;
    }
    do_job_(job);
    std::lock_guard<std::mutex> lock(mu_);
    v_.busy = false;
  }
  bus_ = sd_bus_flush_close_unref(bus_);
}

bool CcaBle::ensure_adapter_(std::string &err) {
  if (!bus_) return err = "no system bus", false;
  if (!adapter_path_.empty()) return true;
  Objects objs;
  if (!managed_objects(bus_, objs, err)) return false;
  for (const auto &o : objs) {
    auto it = o.second.find("org.bluez.Adapter1");
    if (it == o.second.end()) continue;
    adapter_path_ = o.first;
    if (!it->second.count("Powered") || !it->second.at("Powered").b) {
      // Off after boot on some images; the CCA page cannot work without it.
      sd_bus_error e = SD_BUS_ERROR_NULL;
      int r = sd_bus_set_property(bus_, BLUEZ, adapter_path_.c_str(), "org.bluez.Adapter1", "Powered", &e, "b", 1);
      if (r < 0) LOGW("cca: cannot power on %s: %s", adapter_path_.c_str(), e.message ? e.message : strerror(-r));
      sd_bus_error_free(&e);
    }
    LOGI("cca: Bluetooth adapter %s", adapter_path_.c_str());
    return true;
  }
  return err = "this Pi has no Bluetooth adapter", false;
}

void CcaBle::store_(const std::string &cmd, const std::string &reply) {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t now = now_s();
  if (cmd == "DEVICE_INFO") {
    v_.device_info = reply;
    v_.info_epoch = now;
    v_.info_failed_epoch = 0;
    save_locked_();
  } else if (cmd == "NETWORK_INFO") {
    v_.network_info = reply;
    save_locked_();
  } else if (cmd == "START_DISCOVERY" || cmd == "DISCOVERY_STATUS") {
    v_.discovery = reply;
    v_.discovery_epoch = now;
  } else {
    results_[cmd] = {reply, now};
  }
}

void CcaBle::do_job_(const Job &job) {
  std::string err;
  bool ok = open_bus_() && ensure_adapter_(err);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (ok && !v_.adapter) LOGI("cca: Bluetooth is available now");
    v_.adapter = ok;
    if (!ok) {
      if (!err.empty()) v_.error = err;
      if (job.kind == Job::REFRESH) v_.info_failed_epoch = now_s();
      return;
    }
    if (job.kind == Job::PROBE) {
      v_.error.clear();
      return;
    }
  }
  if (job.kind == Job::SCAN) {
    if (!scan_(6, err)) LOGW("cca: scan: %s", err.c_str());
    return;
  }
  std::vector<std::pair<std::string, std::string>> cmds;
  if (job.kind == Job::REFRESH) cmds.push_back({"NETWORK_INFO", ""});
  else cmds.push_back({job.cmd, job.params});
  bool done = session_(cmds, err);
  std::lock_guard<std::mutex> lock(mu_);
  v_.error = done ? "" : err;
  if (!done) {
    LOGW("cca: %s", err.c_str());
    if (job.kind == Job::REFRESH) v_.info_failed_epoch = now_s();
    else results_[job.cmd] = {json({{"msg", err}}).dump(), now_s()};
  }
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool CcaBle::scan_(int seconds, std::string &err) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  // LE only: the CCA is a BLE peripheral, and classic inquiry slows the scan.
  sd_bus_call_method(bus_, BLUEZ, adapter_path_.c_str(), "org.bluez.Adapter1", "SetDiscoveryFilter", &e, nullptr,
                     "a{sv}", 1, "Transport", "s", "le");
  sd_bus_error_free(&e);
  if (!call(bus_, adapter_path_, "org.bluez.Adapter1", "StartDiscovery", err)) {
    // Already discovering (another client) is fine; anything else is not.
    if (err.find("InProgress") == std::string::npos && err.find("in progress") == std::string::npos) return false;
    err.clear();
  }
  auto end = Clock::now() + std::chrono::seconds(seconds);
  while (Clock::now() < end && !stop_) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Objects objs;
    if (!managed_objects(bus_, objs, err)) break;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto &o : objs) {
      auto it = o.second.find("org.bluez.Device1");
      if (it == o.second.end() || o.first.compare(0, adapter_path_.size(), adapter_path_) != 0) continue;
      const Props &p = it->second;
      std::string mac = p.count("Address") ? upper(p.at("Address").s) : "";
      if (mac.compare(0, 8, TIGO_OUI) != 0 || !p.count("RSSI")) continue;  // RSSI: heard in this scan
      ScanHit *hit = nullptr;
      for (auto &h : v_.hits)
        if (h.mac == mac) hit = &h;
      if (!hit) {
        v_.hits.push_back({mac, p.count("Name") ? p.at("Name").s : "", 0, 0});
        hit = &v_.hits.back();
        LOGI("cca: found a Tigo CCA at %s", mac.c_str());
      }
      hit->rssi = static_cast<int>(p.at("RSSI").i);
      hit->seen = now_s();
    }
  }
  std::string ignored;
  call(bus_, adapter_path_, "org.bluez.Adapter1", "StopDiscovery", ignored);
  return err.empty();
}

// Discovers until BlueZ hears dev advertise (a Device1 with an RSSI) or
// max_s pass, then stops discovering: connecting while discovering is
// unreliable on the Pi's controller.
bool CcaBle::hear_(const std::string &dev, int max_s) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus_, BLUEZ, adapter_path_.c_str(), "org.bluez.Adapter1", "SetDiscoveryFilter", &e, nullptr,
                     "a{sv}", 1, "Transport", "s", "le");
  sd_bus_error_free(&e);
  std::string err;
  call(bus_, adapter_path_, "org.bluez.Adapter1", "StartDiscovery", err);  // in progress is fine
  bool heard = false;
  for (auto end = Clock::now() + std::chrono::seconds(max_s); !heard && Clock::now() < end && !stop_;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Objects objs;
    if (!managed_objects(bus_, objs, err)) break;
    auto it = objs.find(dev);
    heard = it != objs.end() && it->second.count("org.bluez.Device1") &&
            it->second.at("org.bluez.Device1").count("RSSI");
  }
  call(bus_, adapter_path_, "org.bluez.Adapter1", "StopDiscovery", err);
  return heard;
}

// ---------------------------------------------------------------------------
// One connection: DEVICE_INFO for the sid, then each command
// ---------------------------------------------------------------------------

int cca_on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *) {
  auto *self = static_cast<CcaBle *>(userdata);
  const char *iface = nullptr;
  if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.GattCharacteristic1") != 0) return 0;
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char *name = nullptr;
    sd_bus_message_read(m, "s", &name);
    if (name && strcmp(name, "Value") == 0 && sd_bus_message_enter_container(m, 'v', "ay") >= 0) {
      const void *data = nullptr;
      size_t len = 0;
      if (sd_bus_message_read_array(m, 'y', &data, &len) >= 0 && len >= 4) {
        auto *b = static_cast<const uint8_t *>(data);
        self->rx_.insert(self->rx_.end(), b + 3, b + len);  // byte 0 reqNum, 1-2 overhead
        if (!self->rx_.empty() && self->rx_.back() == '\0') self->rx_done_ = true;
      }
      sd_bus_message_exit_container(m);
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);
  }
  return 0;
}

bool CcaBle::exchange_(const std::string &write_path, const std::string &text, std::string &reply,
                       std::string &err) {
  if (++req_num_ == 0) req_num_ = 1;
  std::vector<uint8_t> payload;
  payload.push_back(req_num_);
  payload.insert(payload.end(), text.begin(), text.end());
  payload.push_back(0);
  rx_.clear();
  rx_done_ = false;

  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_message *m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, BLUEZ, write_path.c_str(), "org.bluez.GattCharacteristic1",
                                         "WriteValue");
  if (r >= 0) r = sd_bus_message_append_array(m, 'y', payload.data(), payload.size());
  // "request": write with response; BlueZ splits a value longer than the MTU
  // into a long write, which the CCA reassembles — as the firmware sends it.
  if (r >= 0) r = sd_bus_message_append(m, "a{sv}", 1, "type", "s", "request");
  if (r >= 0) r = sd_bus_call(bus_, m, 30 * 1000000ULL, &e, nullptr);
  sd_bus_message_unref(m);
  if (r < 0) {
    err = std::string("write to the CCA failed: ") + (e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return false;
  }
  auto deadline = Clock::now() + std::chrono::seconds(30);
  while (!rx_done_ && Clock::now() < deadline && !stop_) {
    r = sd_bus_process(bus_, nullptr);
    if (r < 0) return err = std::string("Bluetooth bus error: ") + strerror(-r), false;
    if (r == 0) sd_bus_wait(bus_, 200 * 1000);
  }
  if (!rx_done_) return err = "the CCA did not answer within 30 s", false;
  rx_.pop_back();  // the terminating '\0'
  reply.assign(rx_.begin(), rx_.end());
  return true;
}

bool CcaBle::session_(const std::vector<std::pair<std::string, std::string>> &cmds, std::string &err) {
  std::string mac;
  {
    std::lock_guard<std::mutex> lock(mu_);
    mac = v_.active_mac;
  }
  if (mac.empty()) {
    // With no saved CCA we have to find one: the nearest Tigo CCA wins.
    if (!scan_(8, err)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const ScanHit *best = nullptr;
    for (const auto &h : v_.hits)
      if (!best || h.rssi > best->rssi) best = &h;
    if (!best) return err = "no Tigo CCA found over Bluetooth; is it within range?", false;
    mac = v_.active_mac = best->mac;
    save_locked_();
    LOGI("cca: using the nearest CCA, %s", mac.c_str());
  }
  std::string dev = device_path(adapter_path_, mac);

  // BlueZ forgets a device it is not paired with soon after it last heard
  // it, and on the first real Pi a connect then failed with Connect "doesn't
  // exist" — the object was gone — while one right after a scan sometimes
  // timed out. So every connection starts from a fresh advertisement, and a
  // failed one gets one more.
  for (int attempt = 1;; attempt++) {
    if (!hear_(dev, 10)) {
      return err = "the CCA is not advertising over Bluetooth. It stops while another device is connected to it, "
                   "such as the Tigo app or an ESP32 monitor; try again in a minute",
             false;
    }
    LOGI("cca: connecting to %s", mac.c_str());
    if (call(bus_, dev, "org.bluez.Device1", "Connect", err, 30 * 1000000ULL)) break;
    if (attempt == 2) return false;
    LOGW("cca: %s; retrying", err.c_str());
    err.clear();
  }
  Objects objs;
  struct Disconnect {
    sd_bus *bus;
    std::string dev;
    ~Disconnect() {
      std::string ignored;
      call(bus, dev, "org.bluez.Device1", "Disconnect", ignored);
    }
  } guard{bus_, dev};

  // GATT services appear once BlueZ has resolved them.
  std::string notify_path, write_path;
  for (auto deadline = Clock::now() + std::chrono::seconds(15); Clock::now() < deadline;) {
    objs.clear();
    if (!managed_objects(bus_, objs, err)) return false;
    std::string service;
    for (const auto &o : objs) {
      auto it = o.second.find("org.bluez.GattService1");
      if (it != o.second.end() && o.first.compare(0, dev.size() + 1, dev + "/") == 0 && it->second.count("UUID") &&
          it->second.at("UUID").s == CCA_SERVICE_UUID)
        service = o.first;
    }
    for (const auto &o : objs) {
      auto it = o.second.find("org.bluez.GattCharacteristic1");
      if (service.empty() || it == o.second.end() || !it->second.count("Service") ||
          it->second.at("Service").s != service)
        continue;
      for (const auto &f : it->second.count("Flags") ? it->second.at("Flags").strv : std::vector<std::string>{}) {
        if (f == "notify" || f == "indicate") notify_path = o.first;
        if (f == "write") write_path = o.first;
      }
    }
    if (!notify_path.empty() && !write_path.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  if (notify_path.empty() || write_path.empty()) return err = "the CCA's Tigo service did not appear", false;

  sd_bus_slot *slot = nullptr;
  sd_bus_match_signal(bus_, &slot, BLUEZ, notify_path.c_str(), "org.freedesktop.DBus.Properties",
                      "PropertiesChanged", cca_on_properties_changed, this);
  bool ok = call(bus_, notify_path, "org.bluez.GattCharacteristic1", "StartNotify", err);
  std::string reply;
  if (ok) ok = exchange_(write_path, "cmd=DEVICE_INFO", reply, err);
  if (ok) {
    store_("DEVICE_INFO", reply);
    // uts is the nonce the session key is made from; take it as a number or
    // a string of digits, as the firmware does.
    json info = json::parse(reply, nullptr, false);
    uint64_t uts = 0;
    if (info.is_object() && info.contains("uts")) {
      if (info["uts"].is_number_unsigned()) uts = info["uts"].get<uint64_t>();
      else if (info["uts"].is_string()) uts = std::strtoull(info["uts"].get<std::string>().c_str(), nullptr, 10);
    }
    if (uts == 0) {
      ok = false;
      err = "the CCA's DEVICE_INFO had no uts, so no session key";
    } else {
      std::string sid = session_key(uts);
      for (const auto &c : cmds) {
        // Extra params come before sid, in the app's order.
        std::string text = "cmd=" + c.first + " " + (c.second.empty() ? "" : c.second + " ") + "sid=" + sid;
        if (!(ok = exchange_(write_path, text, reply, err))) break;
        if (reply.find("Unauthorized request") != std::string::npos) {
          ok = false;
          err = "the CCA rejected the session key for " + c.first;
          break;
        }
        store_(c.first, reply);
        LOGI("cca: %s: %zu bytes", c.first.c_str(), reply.size());
      }
    }
  }
  std::string ignored;
  call(bus_, notify_path, "org.bluez.GattCharacteristic1", "StopNotify", ignored);
  sd_bus_slot_unref(slot);
  return ok;
}

}  // namespace tigod
