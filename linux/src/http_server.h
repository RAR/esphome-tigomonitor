#pragma once

// Serves the same web UI as the ESP32 (components/tigo_server/web/app.html)
// and the /api/* routes it reads, in the firmware's JSON shapes — see
// site/screenshots/fixtures.mjs for the reference payloads. Routes the Pi
// has no data for yet (CCA, cloud, history database) are not served; the UI
// treats those as unavailable.
//
// Settings changes are written to settings.json and flagged for the main
// loop to apply, so MQTT is only ever reconfigured from one thread.

#include "cca_ble.h"
#include "cca_http.h"
#include "cloud.h"
#include "history.h"
#include "monitor.h"
#include "bus_master.h"
#include "settings.h"
#include "tls.h"
#include "bus_log.h"
#include "topology.h"
#include "web_session.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib {
class Server;
struct Request;
}

namespace tigod {

struct HttpContext {
  const Monitor &monitor;
  std::string web_dir;        // holds app.html
  std::string settings_path;
  std::string version;
  std::function<Settings()> current_settings;
  std::function<bool()> mqtt_connected;
  std::function<std::string()> mqtt_error;
  std::atomic<bool> &settings_changed;
  std::atomic<bool> &restart_requested;
  Monitor &mutable_monitor;
  Topology &topology;
  History &history;
  Cloud &cloud;
  CcaBle &cca;
  CcaHttp &cca_http;
  std::function<MasterStatus()> master_status;  // inactive when listening
  TlsCerts *tls = nullptr;  // HTTPS with these; null: plain HTTP
  bool tls_from_args = false;  // set by --tls-cert/--tls-key, not the web UI
  std::function<int64_t()> main_loop_late_ms;  // since the main loop's last pass
  // The owner's PV off or on: 200, or 409 with why (no bus master running,
  // or it is standing by).
  std::function<int(bool off, std::string &why)> request_pv;
  std::function<bool()> pv_unreadable;  // pv.json couldn't be read: nothing said about PV
  // The optimizers' last !Info/!Mppt answers by short id, while this monitor
  // runs the bus with queries on.
  std::function<std::map<uint16_t, esphome::tigo_monitor::master::Queries::Setup>()> setups;
  // The owner's discovery switch (renew or let it close): 200, or 409 with
  // why, as request_pv.
  std::function<int(bool renew, std::string &why)> request_discovery;
  // The decoded bus log and the TAP-reconfiguration alarm.
  BusLog *bus_log = nullptr;
  // The owner's add of an optimizer to a TAP's table (MAC, id 0 for the
  // lowest free, dry run, the TAP's id or 0 for the first): 200, or 409
  // with why, as request_pv.
  std::function<int(const esphome::tigo_monitor::master::Bytes &mac, uint16_t id, bool dry_run, std::string &why,
                    uint16_t tap)>
      request_table_add;
  // The owner's removal of one, or several in one pass (tap 0: the one the
  // first reports through): 200, or 409 with why.
  std::function<int(const std::vector<esphome::tigo_monitor::master::Bytes> &macs, bool dry_run, std::string &why,
                    uint16_t tap)>
      request_table_remove;
  // The owner's replace of one (old MAC, new MAC, dry run, tap 0: the old
  // one's): 200, or 409 with why.
  std::function<int(const esphome::tigo_monitor::master::Bytes &old_mac, const esphome::tigo_monitor::master::Bytes &new_mac,
                    bool dry_run, std::string &why, uint16_t tap)>
      request_table_replace;
  // Stop waiting for an added optimizer to join: 200, or 409 with why.
  std::function<int(const esphome::tigo_monitor::master::Bytes &mac, std::string &why)> request_table_stop_waiting;
  // The owner's herd (look for TAPs): 200, or 409 with why.
  std::function<int(std::string &why)> request_herd;
  // Retire a TAP that has gone for good (POST /api/master/taps/forget); 200, or 409 with why.
  std::function<int(uint16_t id, std::string &why)> forget_tap;
  // Give a bench TAP its radio settings (POST /api/master/taps/provision,
  // tap_config.h): its MAC, the channel, the PAN (0: from its MAC), reset
  // after, dry run. 200 with what it will give in given (no key), or 409 with why.
  std::function<int(const esphome::tigo_monitor::master::Bytes &tap_mac, uint16_t channel, uint16_t pan,
                    bool identify, bool reset, bool dry_run, int profile, bool keep_network,
                    std::string &why, std::string &given)>
      request_provision;
  // Bench only: ask one optimizer a read-shaped text (POST /api/master/query);
  // 200, or 409 with why.
  std::function<int(uint16_t node, const std::string &text, std::string &why)> request_ask;
  // Sets one TAP's report period (tap_config.h): its MAC, the code (1-11),
  // dry run. 200, or 409 with why.
  std::function<int(const esphome::tigo_monitor::master::Bytes &tap_mac, uint8_t code, bool dry_run, std::string &why)>
      request_period;
  // The owner's one-shot Leave to one optimizer (POST /api/master/leave),
  // only with dangerous_options in settings.json; 200, or 409 with why.
  std::function<int(uint16_t node, std::string &why)> request_leave;
};

// tigomonitord --health-check PORT, the container's HEALTHCHECK: GETs
// /api/health from the daemon on this machine and returns 0 for a 200.
int health_check(int port);

// With HTTPS on the default ports, port 80 answers every request with a
// redirect to the same path over HTTPS, so http:// bookmarks and the mDNS
// advert (which names port 80) still lead to the web UI.
class HttpsRedirect {
 public:
  HttpsRedirect();
  ~HttpsRedirect();
  bool start(int port, int https_port);
  void stop();

 private:
  std::unique_ptr<httplib::Server> svr_;
  std::unique_ptr<std::thread> thread_;
};

class HttpServer {
 public:
  explicit HttpServer(HttpContext ctx);
  ~HttpServer();
  // Listens on port in a background thread, or on fallback (when nonzero)
  // if port is taken. Returns the port it got, or -1 if it got neither
  // (port 0, any free port, returns 0).
  int start(int port, int fallback = 0);
  void stop();

 private:
  void routes_();
  std::vector<std::string> health_problems_() const;
  void new_server_();
  HttpContext ctx_;
  int64_t started_unix_ms_;
  std::unique_ptr<httplib::Server> svr_;
  std::unique_ptr<std::thread> thread_;
  int port_ = 0;
  std::mutex settings_mu_;  // serialises settings.json writes
  WebSessions sessions_;    // the web UI's sign-ins (web_session.h)
  // Signed in: a session cookie, or the username and password as HTTP Basic
  // (scripts, and browsers that still send what their popup was given).
  bool signed_in_(const httplib::Request &req, const Settings &s);
  // The Set-Cookie for a session, or with "" one that ends it.
  std::string session_cookie_(const std::string &token, const httplib::Request &req, const Settings &s) const;
  std::string client_(const httplib::Request &req, const Settings &s) const;  // who wrong passwords count against
};

}  // namespace tigod
